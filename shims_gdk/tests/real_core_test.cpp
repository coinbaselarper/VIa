// Links the real shims_core (not tests/support stand-ins) and drives the GDK shims through the registry rows the title
// would call: DirectStorage reads over the handle shims_core's CreateFileW hands out, and CoCreateInstance of the audio
// device enumerator through shims_core's COM shim.
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>

#include <gta6rt/shims.h>

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "dstorage_xs_abi.h"
#include "gdk_register.h"
#include "silent_audio.h"

using namespace dsx;
using rt::silent_audio::kClsidMMDeviceEnumerator;

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

std::vector<unsigned char> write_file(const std::string& path, size_t size, unsigned seed) {
    std::vector<unsigned char> bytes(size);
    for (size_t i = 0; i < size; ++i) bytes[i] = static_cast<unsigned char>(i * 7 + seed);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f) {
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
    }
    return bytes;
}

struct Reader {
    IDStorageFactoryX2* factory = nullptr;
    IDStorageQueueX2* queue = nullptr;
    IDStorageStatusArrayX* status = nullptr;
    UINT32 next = 0;

    bool init() {
        auto get = reinterpret_cast<DStorageGetFactoryFn>(rt::shims::find("dstorage_xs.dll", "DStorageGetFactory"));
        if (!get || FAILED(get(kFactoryId, reinterpret_cast<void**>(&factory)))) return false;
        DSTORAGE_QUEUE_DESC1 d{};
        d.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        d.Capacity = 0x80;
        d.Priority = DSTORAGE_PRIORITY_NORMAL;
        d.Name = "q";
        void* q = nullptr;
        void* s = nullptr;
        if (FAILED(factory->CreateQueue1(&d, kQueueId, &q)) || FAILED(factory->CreateStatusArray(16, "s", kStatusId, &s)))
            return false;
        queue = static_cast<IDStorageQueueX2*>(q);
        status = static_cast<IDStorageStatusArrayX*>(s);
        return true;
    }

    HRESULT read(const char16_t* path, uint64_t offset, std::vector<unsigned char>& out) {
        void* file = nullptr;
        HRESULT hr = factory->OpenFile(path, kFileId, &file);
        if (FAILED(hr)) return hr;
        DSTORAGE_REQUEST r{};
        r.File = static_cast<IDStorageFileX*>(file);
        r.FileOffset = offset;
        r.Destination = out.data();
        r.DestinationSize = static_cast<uint32_t>(out.size());
        r.SourceSize = static_cast<uint32_t>(out.size());
        const UINT32 slot = next++ % 16;
        queue->EnqueueRequest(&r);
        queue->EnqueueStatus(status, slot);
        queue->Submit();
        for (int i = 0; i < 5000 && !status->IsComplete(slot); ++i) usleep(1000);
        hr = status->IsComplete(slot) ? status->GetHResult(slot) : HRESULT(E_PENDING);
        static_cast<IUnknown*>(file)->Release();
        return hr;
    }
};

void test_dstorage_over_shims_core_files(const std::string& root) {
    const std::string game = root + "/game", overlay = root + "/overlay";
    check(std::system(("mkdir -p '" + game + "/data' '" + overlay + "/data'").c_str()) == 0, "scratch folders");
    const auto b = write_file(game + "/data/b.bin", 8192, 3);
    const auto over = write_file(overlay + "/data/o.bin", 6000, 11);
    check(gta6rt_shims_configure_paths(game.c_str(), overlay.c_str()) == GTA6RT_OK, "configure_paths");
    void* create = rt::shims::find("kernel32.dll", "CreateFileW");
    check(create != nullptr, "kernel32!CreateFileW is registered by shims_core");

    Reader reader;
    check(reader.init(), "DStorageGetFactory, CreateQueue1 and CreateStatusArray");
    if (!reader.queue) return;

    std::vector<unsigned char> got(4096, 0xEE);
    HRESULT hr = reader.read(u"GAME:\\data\\b.bin", 129, got);
    check(hr == S_OK, "a 4 KiB read through shims_core's CreateFileW handle succeeds");
    check(std::memcmp(got.data(), b.data() + 129, 4096) == 0, "and returns the bytes at that offset");

    got.assign(1000, 0xEE);
    hr = reader.read(u"GAME:\\data\\o.bin", 4000, got);
    check(hr == S_OK && std::memcmp(got.data(), over.data() + 4000, 1000) == 0, "the overlay copy is read, at its offset");

    got.assign(4096, 0xEE);
    hr = reader.read(u"GAME:\\data\\b.bin", 8000, got);
    check(hr == E_DSTORAGE_END_OF_FILE, "a read past the end of the file reports end of file");

    // A handle's low descriptor numbers are not what the shim reads: with many files open, a misread would hit another file.
    std::vector<FILE*> filler;
    for (int i = 0; i < 300; ++i) {
        FILE* f = std::fopen((game + "/data/b.bin").c_str(), "rb");
        if (!f) break;
        filler.push_back(f);
    }
    got.assign(4096, 0xEE);
    hr = reader.read(u"GAME:\\data\\b.bin", 0, got);
    check(hr == S_OK && std::memcmp(got.data(), b.data(), 4096) == 0, "the read is right with 300 other descriptors open");
    for (FILE* f : filler) std::fclose(f);
}

void test_audio_enumerator_through_com_shim() {
    using CoCreate = HRESULT(WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
    auto co_create = reinterpret_cast<CoCreate>(rt::shims::find("ole32.dll", "CoCreateInstance"));
    check(co_create != nullptr, "ole32!CoCreateInstance is registered by shims_core");
    if (!co_create) return;
    setenv("LEONIDA_AUDIO", "silent", 1);
    IMMDeviceEnumerator* e = nullptr;
    const HRESULT hr = co_create(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                 reinterpret_cast<void**>(&e));
    check(hr == S_OK && e, "CoCreateInstance(CLSID_MMDeviceEnumerator) succeeds");
    if (e) {
        IMMDevice* d = nullptr;
        check(e->GetDefaultAudioEndpoint(eRender, eConsole, &d) == S_OK && d, "the default render endpoint exists");
        if (d) d->Release();
        e->Release();
    }
    unsetenv("LEONIDA_AUDIO");
}
}  // namespace

int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/gdk_real_core." + std::to_string(getpid());
    gta6rt_shims_init();
    check(rt::gdk::register_all() == GTA6RT_OK, "register_all");
    test_dstorage_over_shims_core_files(root);
    test_audio_enumerator_through_com_shim();
    std::system(("rm -rf '" + root + "'").c_str());
    std::printf("%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
