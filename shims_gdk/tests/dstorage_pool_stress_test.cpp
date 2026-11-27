#include <windows.h>
#include <d3d12.h>
#include <zlib.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "dstorage_xs_abi.h"
#include "file_system.h"
#include "shims.h"
#include <gta6rt/shims.h>

void gdk_test_register_xmem_fake();
void gdk_test_register_console_files();

using namespace dsx;
namespace fs = std::filesystem;

namespace {

constexpr GUID kFactoryId = {0x0d5e0001, 0x1111, 0x4111, {0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x01}};
constexpr GUID kQueueId = {0x0d5e0002, 0x1111, 0x4111, {0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x02}};
constexpr GUID kFileId = {0x0d5e0003, 0x1111, 0x4111, {0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x03}};
constexpr GUID kStatusId = {0x0d5e0004, 0x1111, 0x4111, {0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x04}};

int g_failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

struct Stream {
    uint64_t offset;
    uint32_t packed, plain;
    uint64_t dest;
    bool raw;
};

std::vector<uint8_t> make_plain(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    std::mt19937 rng(seed);
    const uint32_t mode = seed % 3;
    for (size_t i = 0; i < n; ++i) {
        if (mode == 0) v[i] = static_cast<uint8_t>(rng() % 16);
        else if (mode == 1) v[i] = static_cast<uint8_t>((i / 64) * 7 + (rng() % 4));
        else v[i] = static_cast<uint8_t>(i < 16 ? rng() : v[i - 16]);
    }
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    const int count = argc > 1 ? std::atoi(argv[1]) : 1500;
    const int rounds = argc > 2 ? std::atoi(argv[2]) : 3;
    GDK_TEST_REGISTER();
    WCHAR temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const fs::path root = fs::path(temp) / ("leonida_ds_stress_" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(root);
    fs::create_directories(root / u"game" / u"data");
    rt::fs::configure(root / u"game", root / u"overlay");
    gdk_test_register_xmem_fake();
    gdk_test_register_console_files();

    std::vector<Stream> streams;
    std::vector<uint8_t> file, expected;
    std::mt19937 rng(12345);
    for (int i = 0; i < count; ++i) {
        const uint32_t plain = 1024 + (rng() % 8 == 0 ? rng() % (3u << 20) : rng() % 70000);
        const auto raw = make_plain(plain, static_cast<uint32_t>(i));
        const bool is_raw = i % 4 == 3;
        uLongf cap = compressBound(plain);
        std::vector<uint8_t> z(cap);
        if (is_raw) {
            z = raw;
            cap = plain;
        } else {
            compress2(z.data(), &cap, raw.data(), plain, 1 + i % 9);
        }
        while (file.size() % 16) file.push_back(0);
        const uint64_t off = file.size();
        file.insert(file.end(), z.begin(), z.begin() + cap);
        streams.push_back({off, static_cast<uint32_t>(cap), plain, expected.size(), is_raw});
        expected.insert(expected.end(), raw.begin(), raw.end());
    }
    {
        FILE* f = std::fopen((root / u"game" / u"data" / u"z.bin").c_str(), "wb");
        std::fwrite(file.data(), 1, file.size(), f);
        std::fclose(f);
    }

    using GetFactory = HRESULT(WINAPI*)(REFIID, void**);
    auto get = reinterpret_cast<GetFactory>(rt::shims::find("dstorage_xs.dll", "DStorageGetFactory"));
    void* fv = nullptr;
    check(get && SUCCEEDED(get(kFactoryId, &fv)), "factory");
    auto* factory = static_cast<IDStorageFactoryX2*>(fv);

    for (int round = 0; round < rounds; ++round) {
        std::vector<uint8_t> dest(expected.size() + 64, 0xCD);
        void* fo = nullptr;
        check(SUCCEEDED(factory->OpenFile(u"GAME:\\data\\z.bin", kFileId, &fo)), "open");
        auto* dfile = static_cast<IDStorageFileX*>(fo);
        DSTORAGE_QUEUE_DESC1 d{};
        d.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        d.Capacity = 0x400;
        d.Priority = DSTORAGE_PRIORITY_NORMAL;
        d.Name = "stress";
        void* qv = nullptr;
        check(SUCCEEDED(factory->CreateQueue1(&d, kQueueId, &qv)), "queue");
        auto* q = static_cast<IDStorageQueueX2*>(qv);
        const int marks = count / 5 + 1;
        void* sv = nullptr;
        check(SUCCEEDED(factory->CreateStatusArray(marks, "s", kStatusId, &sv)), "status array");
        auto* status = static_cast<IDStorageStatusArrayX*>(sv);

        std::vector<int> mark_after;
        int mark = 0;
        for (int i = 0; i < count; ++i) {
            DSTORAGE_REQUEST r{};
            r.Options.ZlibDecompress = streams[i].raw ? 0 : 1;
            r.File = dfile;
            r.FileOffset = streams[i].offset;
            r.SourceSize = streams[i].packed;
            r.Destination = dest.data() + streams[i].dest;
            r.DestinationSize = streams[i].plain;
            r.CancellationTag = i;
            q->EnqueueRequest(&r);
            if (i % 5 == 4 && mark < marks) {
                q->EnqueueStatus(status, mark);
                mark_after.push_back(i);
                ++mark;
            }
        }
        q->Submit();

        int verified = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (verified < mark && std::chrono::steady_clock::now() < deadline) {
            if (!status->IsComplete(verified)) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                continue;
            }
            check(status->GetHResult(verified) == S_OK, "status hresult");
            const int last = mark_after[verified];
            const uint64_t end = streams[last].dest + streams[last].plain;
            const uint64_t from = verified ? streams[mark_after[verified - 1]].dest + streams[mark_after[verified - 1]].plain : 0;
            check(std::memcmp(dest.data() + from, expected.data() + from, end - from) == 0, "data present when its status completes");
            ++verified;
        }
        check(verified == mark, "every status completed");
        q->Close();
        const int tail_from = mark_after.empty() ? 0 : mark_after.back() + 1;
        const uint64_t tail = tail_from < count ? streams[tail_from].dest : expected.size();
        check(std::memcmp(dest.data() + tail, expected.data() + tail, expected.size() - tail) == 0, "tail after Close");
        check(std::memcmp(dest.data(), expected.data(), expected.size()) == 0, "all data");
        check(dest[expected.size()] == 0xCD, "no write past the end");
        q->Release();
        status->Release();
        dfile->Release();
    }
    {
        constexpr int kQueues = 4;
        std::vector<uint8_t> dest(expected.size() + 64, 0xCD);
        void* fo = nullptr;
        check(SUCCEEDED(factory->OpenFile(u"GAME:\\data\\z.bin", kFileId, &fo)), "open (queues)");
        auto* dfile = static_cast<IDStorageFileX*>(fo);
        std::vector<IDStorageQueueX2*> qs(kQueues);
        std::vector<IDStorageStatusArrayX*> ss(kQueues);
        for (int k = 0; k < kQueues; ++k) {
            DSTORAGE_QUEUE_DESC1 d{};
            d.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
            d.Capacity = 0x400;
            d.Priority = DSTORAGE_PRIORITY_NORMAL;
            d.Name = "stress-multi";
            void* qv = nullptr;
            check(SUCCEEDED(factory->CreateQueue1(&d, kQueueId, &qv)), "queue (queues)");
            qs[k] = static_cast<IDStorageQueueX2*>(qv);
            void* sv = nullptr;
            check(SUCCEEDED(factory->CreateStatusArray(1, "s", kStatusId, &sv)), "status array (queues)");
            ss[k] = static_cast<IDStorageStatusArrayX*>(sv);
        }
        std::vector<std::thread> feeders;
        for (int k = 0; k < kQueues; ++k)
            feeders.emplace_back([&, k] {
                for (int i = k; i < count; i += kQueues) {
                    DSTORAGE_REQUEST r{};
                    r.Options.ZlibDecompress = streams[i].raw ? 0 : 1;
                    r.File = dfile;
                    r.FileOffset = streams[i].offset;
                    r.SourceSize = streams[i].packed;
                    r.Destination = dest.data() + streams[i].dest;
                    r.DestinationSize = streams[i].plain;
                    qs[k]->EnqueueRequest(&r);
                }
                qs[k]->EnqueueStatus(ss[k], 0);
                qs[k]->Submit();
            });
        for (auto& t : feeders) t.join();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        for (int k = 0; k < kQueues; ++k) {
            while (!ss[k]->IsComplete(0) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::microseconds(200));
            check(ss[k]->IsComplete(0) && ss[k]->GetHResult(0) == S_OK, "queues: status");
        }
        check(std::memcmp(dest.data(), expected.data(), expected.size()) == 0, "queues: all data");
        for (int k = 0; k < kQueues; ++k) {
            qs[k]->Close();
            qs[k]->Release();
            ss[k]->Release();
        }
        dfile->Release();
    }
    for (int phase = 0; phase < 2; ++phase) {
        std::vector<uint8_t> dest(expected.size() + 64, 0xCD);
        void* fo = nullptr;
        check(SUCCEEDED(factory->OpenFile(u"GAME:\\data\\z.bin", kFileId, &fo)), "open (cancel)");
        auto* dfile = static_cast<IDStorageFileX*>(fo);
        DSTORAGE_QUEUE_DESC1 d{};
        d.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        d.Capacity = 0x1000;
        d.Priority = DSTORAGE_PRIORITY_NORMAL;
        d.Name = "stress-cancel";
        void* qv = nullptr;
        check(SUCCEEDED(factory->CreateQueue1(&d, kQueueId, &qv)), "queue (cancel)");
        auto* q = static_cast<IDStorageQueueX2*>(qv);
        void* sv = nullptr;
        check(SUCCEEDED(factory->CreateStatusArray(1, "s", kStatusId, &sv)), "status array (cancel)");
        auto* status = static_cast<IDStorageStatusArrayX*>(sv);
        for (int i = 0; i < count; ++i) {
            DSTORAGE_REQUEST r{};
            r.Options.ZlibDecompress = streams[i].raw ? 0 : 1;
            r.File = dfile;
            r.FileOffset = streams[i].offset;
            r.SourceSize = streams[i].packed;
            r.Destination = dest.data() + streams[i].dest;
            r.DestinationSize = streams[i].plain;
            r.CancellationTag = i % 2;
            q->EnqueueRequest(&r);
        }
        q->EnqueueStatus(status, 0);
        if (phase == 0) {
            q->CancelRequestsWithTag(1, 1);
            q->Submit();
        } else {
            q->Submit();
            std::this_thread::sleep_for(std::chrono::microseconds(300));
            q->CancelRequestsWithTag(1, 1);
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (!status->IsComplete(0) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::microseconds(200));
        check(status->IsComplete(0) && status->GetHResult(0) == S_OK, "cancel: status");
        int cancelled = 0;
        for (int i = 0; i < count; ++i) {
            const auto* got = dest.data() + streams[i].dest;
            const bool exact = std::memcmp(got, expected.data() + streams[i].dest, streams[i].plain) == 0;
            bool untouched = true;
            for (uint32_t b = 0; b < streams[i].plain && untouched; ++b) untouched = got[b] == 0xCD;
            if (i % 2 == 0) check(exact, "cancel: an uncancelled request is exact");
            else {
                check(exact || untouched, "cancel: a cancelled request is whole or untouched");
                cancelled += untouched;
            }
        }
        if (phase == 0) check(cancelled == count / 2, "cancel before submit: every tagged request left alone");
        check(dest[expected.size()] == 0xCD, "cancel: no write past the end");
        q->Close();
        q->Release();
        status->Release();
        dfile->Release();
    }
    fs::remove_all(root);
    std::printf("dstorage pool stress (every 4th stream raw; 1 queue, 4 queues, cancel): %d streams x %d rounds, %d failures\n", count, rounds, g_failures);
    return g_failures ? 1 : 0;
}
