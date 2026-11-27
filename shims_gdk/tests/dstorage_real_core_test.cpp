// The applied io backend against the real shims_core: the FileNativeFd row, F_NOCACHE on the descriptor a DirectStorage file holds,
// and zlib, raw and failing requests over shims_core's own handles.
#include <windows.h>
#include <zlib.h>

#include <fcntl.h>
#include <gta6rt/shims.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "dstorage_xs_abi.h"
#include "gdk_register.h"
#include "shims.h"


using namespace dsx;

namespace {
int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

constexpr GUID kFactoryId = {0x0d5e0001, 0x1111, 0x4111, {0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x01}};
constexpr GUID kQueueId = {0x0d5e0002, 0x1111, 0x4111, {0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x02}};
constexpr GUID kFileId = {0x0d5e0003, 0x1111, 0x4111, {0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x03}};
constexpr GUID kStatusId = {0x0d5e0004, 0x1111, 0x4111, {0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x04}};

double resident_pct(const std::string& path, uint64_t offset, size_t bytes, bool evict) {
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return -1;
    const size_t page = static_cast<size_t>(getpagesize());
    const uint64_t a = offset / page * page;
    const size_t len = (offset + bytes - a + page - 1) / page * page;
    void* m = mmap(nullptr, len, PROT_READ, MAP_SHARED, fd, static_cast<off_t>(a));
    double pct = -1;
    if (m != MAP_FAILED) {
        if (evict) msync(m, len, MS_INVALIDATE);
        std::vector<char> vec(len / page);
        if (mincore(m, len, vec.data()) == 0) {
            size_t n = 0;
            for (char c : vec) n += (c & MINCORE_INCORE) != 0;
            pct = 100.0 * static_cast<double>(n) / static_cast<double>(vec.size());
        }
        munmap(m, len);
    }
    close(fd);
    return pct;
}
}  // namespace

int main() {
    gta6rt_shims_init();
    check(rt::gdk::register_all() == GTA6RT_OK, "register_all");
    char tmpl[] = "/tmp/dstorage_real_core_XXXXXX";
    const std::string root = mkdtemp(tmpl);
    const std::string game = root + "/game";
    check(std::system(("mkdir -p '" + game + "/data'").c_str()) == 0, "scratch folders");
    check(gta6rt_shims_configure_paths(game.c_str(), (root + "/overlay").c_str()) == GTA6RT_OK, "configure_paths");

    std::vector<unsigned char> plain(300000), file;
    for (size_t i = 0; i < plain.size(); ++i) plain[i] = static_cast<unsigned char>((i / 64) * 7 + i % 5);
    uLongf cap = compressBound(plain.size());
    std::vector<unsigned char> z(cap);
    check(compress2(z.data(), &cap, plain.data(), plain.size(), 6) == Z_OK, "compress");
    file.assign(16, 0xAB);
    file.insert(file.end(), z.begin(), z.begin() + cap);
    while (file.size() % 16) file.push_back(0);
    const uint64_t raw_at = file.size();
    file.insert(file.end(), plain.begin(), plain.begin() + 5000);
    const uint64_t big_at = (file.size() + 16383) / 16384 * 16384;
    file.resize(big_at + (4u << 20));
    for (size_t i = big_at; i < file.size(); ++i) file[i] = static_cast<unsigned char>(i * 13);
    if (FILE* f = std::fopen((game + "/data/z.bin").c_str(), "wb")) {
        std::fwrite(file.data(), 1, file.size(), f);
        std::fclose(f);
    }

    using NativeFd = int(WINAPI*)(HANDLE);
    auto native = reinterpret_cast<NativeFd>(rt::shims::find("gta6rt.internal", "FileNativeFd"));
    check(native != nullptr, "shims_core registers gta6rt.internal!FileNativeFd");
    if (native) {
        check(native(INVALID_HANDLE_VALUE) == -1, "FileNativeFd of an invalid handle is -1");
        check(native(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x7777777))) == -1, "FileNativeFd of an unknown handle is -1");
    }

    using GetFactory = HRESULT(WINAPI*)(REFIID, void**);
    auto get = reinterpret_cast<GetFactory>(rt::shims::find("dstorage_xs.dll", "DStorageGetFactory"));
    void* fv = nullptr;
    check(get && SUCCEEDED(get(kFactoryId, &fv)), "factory");
    auto* factory = static_cast<IDStorageFactoryX2*>(fv);
    DSTORAGE_QUEUE_DESC1 d{};
    d.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    d.Capacity = 0x80;
    d.Priority = DSTORAGE_PRIORITY_NORMAL;
    d.Name = "real-core";
    void *qv = nullptr, *sv = nullptr;
    check(SUCCEEDED(factory->CreateQueue1(&d, kQueueId, &qv)), "queue");
    check(SUCCEEDED(factory->CreateStatusArray(4, "s", kStatusId, &sv)), "status array");
    auto* queue = static_cast<IDStorageQueueX2*>(qv);
    auto* status = static_cast<IDStorageStatusArrayX*>(sv);

    void* fo = nullptr;
    check(SUCCEEDED(factory->OpenFile(u"GAME:\\data\\z.bin", kFileId, &fo)), "OpenFile");
    auto* dfile = static_cast<IDStorageFileX*>(fo);
    const std::string path = game + "/data/z.bin";
    check(resident_pct(path, big_at, 4u << 20, true) < 5.0, "the 4 MiB test range starts out of the page cache");

    std::vector<unsigned char> zout(plain.size(), 0xEE), rout(5000, 0xEE), bout(plain.size(), 0xEE);
    DSTORAGE_REQUEST r{};
    r.Options.ZlibDecompress = 1;
    r.File = dfile;
    r.FileOffset = 16;
    r.SourceSize = static_cast<uint32_t>(cap);
    r.Destination = zout.data();
    r.DestinationSize = static_cast<uint32_t>(zout.size());
    queue->EnqueueRequest(&r);
    DSTORAGE_REQUEST raw{};
    raw.File = dfile;
    raw.FileOffset = raw_at;
    raw.SourceSize = 5000;
    raw.Destination = rout.data();
    raw.DestinationSize = 5000;
    queue->EnqueueRequest(&raw);
    queue->EnqueueStatus(status, 0);
    DSTORAGE_REQUEST bad = r;
    bad.FileOffset = 0;
    bad.Destination = bout.data();
    queue->EnqueueRequest(&bad);
    queue->EnqueueStatus(status, 1);
    std::vector<unsigned char> big(4u << 20, 0xEE);
    DSTORAGE_REQUEST bigr{};
    bigr.File = dfile;
    bigr.FileOffset = big_at;
    bigr.SourceSize = 4u << 20;
    bigr.Destination = big.data();
    bigr.DestinationSize = 4u << 20;
    queue->EnqueueRequest(&bigr);
    queue->EnqueueStatus(status, 2);
    queue->Submit();
    for (int i = 0; i < 5000 && !status->IsComplete(2); ++i) usleep(1000);
    check(status->IsComplete(0) && status->GetHResult(0) == S_OK, "zlib and raw requests complete");
    check(std::memcmp(zout.data(), plain.data(), plain.size()) == 0, "the zlib output is the plain data");
    check(status->IsComplete(1) && FAILED(status->GetHResult(1)), "a stream that is not zlib fails with a typed error through the native fd path");
    check(std::memcmp(rout.data(), plain.data(), 5000) == 0, "the raw output is the file bytes");
    check(bout[0] == 0xEE && bout[plain.size() - 1] == 0xEE, "a failed request publishes nothing");

    check(status->IsComplete(2) && status->GetHResult(2) == S_OK && std::memcmp(big.data(), file.data() + big_at, big.size()) == 0, "a 4 MiB raw read returns the file bytes");
    const double after = resident_pct(path, big_at, 4u << 20, false);
    std::printf("range resident after the DirectStorage read: %.1f%%\n", after);
    check(after < 25.0, "DirectStorage's read left the 4 MiB range out of the page cache (F_NOCACHE on its descriptor)");
    {
        const int fd = open(path.c_str(), O_RDONLY);
        std::vector<char> sink(4u << 20);
        check(fd >= 0 && pread(fd, sink.data(), sink.size(), static_cast<off_t>(big_at)) == static_cast<ssize_t>(sink.size()), "control: a plain pread");
        close(fd);
        check(resident_pct(path, big_at, 4u << 20, false) > 75.0, "control: a plain pread does leave the range in the page cache");
    }
    queue->Close();
    queue->Release();
    status->Release();
    dfile->Release();
    std::system(("rm -rf '" + root + "'").c_str());
    std::printf("dstorage real core: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
