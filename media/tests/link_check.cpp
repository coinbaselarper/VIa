// Links gta6rt_media against the real shims_core, isa and os: registers the three DLLs, resolves the title's and
// Bink's imports through gta6rt_shims_import_resolver (by name and by ordinal, with the import spellings of the PEs),
// and checks the resolver's file probe through shims_core's GetFileAttributesW row on a scratch package directory.
#include <gta6rt/isa.h>
#include <gta6rt/media.h>
#include <gta6rt/shims.h>

#include "media_common.h"
#include "media_exports.h"
#include "shims.h"
#include "test_util.h"

#include <cctype>
#include <cstdlib>
#include <string>
#include <unistd.h>

using namespace rt::media;

int main() {
    gta6rt_isa_config cfg{};
    std::printf("isa_init: %d (%s)\n", gta6rt_isa_init(&cfg), gta6rt_isa_backend_name());
    CHECK(gta6rt_shims_init() == GTA6RT_OK);
    const gta6rt_status s = gta6rt_media_register();
    std::printf("media_register: %d (%s)\n", s, gta6rt_status_name(s));
#if !defined(__x86_64__)
    CHECK(s == GTA6RT_E_UNSUPPORTED);
    return mt::finish("media_link_check (arm64: registration unsupported, as shims_gdk)");
#else
    CHECK(s == GTA6RT_OK);
    struct Probe {
        const char* dll;
        const char* name;
        uint16_t ordinal;
        const void* want;
    } probes[] = {
        {"XAudio2_9.dll", nullptr, 1, reinterpret_cast<const void*>(&XAudio2Create)},
        {"xaudio2_9.dll", "XAudio2Create", 0, reinterpret_cast<const void*>(&XAudio2Create)},
        {"MFPlat.DLL", "MFStartup", 0, reinterpret_cast<const void*>(&MFStartup)},
        {"MFPlat.DLL", "MFShutdown", 0, reinterpret_cast<const void*>(&MFShutdown)},
        {"MFPlat.DLL", "MFCreateAttributes", 0, reinterpret_cast<const void*>(&MFCreateAttributes)},
        {"MFPlat.DLL", "MFCreateMediaType", 0, reinterpret_cast<const void*>(&MFCreateMediaType)},
        {"MFPlat.DLL", "MFCreateSourceResolver", 0, reinterpret_cast<const void*>(&MFCreateSourceResolver)},
        {"MFPlat.DLL", "MFGetSystemTime", 0, reinterpret_cast<const void*>(&MFGetSystemTime)},
        {"MFReadWrite.dll", "MFCreateSourceReaderFromMediaSource", 0,
         reinterpret_cast<const void*>(&MFCreateSourceReaderFromMediaSource)},
    };
    for (const Probe& p : probes) {
        gta6rt_import_ref ref{};
        ref.dll = p.dll;
        ref.name = p.name;
        ref.ordinal = p.ordinal;
        ref.kind_guess = GTA6RT_IMPORT_CODE;
        gta6rt_import_binding b{};
        const gta6rt_status r = gta6rt_shims_import_resolver(nullptr, &ref, &b);
        std::string lower = p.dll;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const void* row = rt::shims::find(lower, p.name ? p.name : "#" + std::to_string(p.ordinal));
        std::printf("%s!%s -> status %d address %#llx row %p\n", p.dll, p.name ? p.name : "#1", r,
                    static_cast<unsigned long long>(b.address), row);
        CHECK(r == GTA6RT_OK && b.address != 0 && row == p.want);
    }

    // The resolver's probe on real shims_core paths.
    char tmpl[] = "/tmp/media_link_check.XXXXXX";
    const char* root = mkdtemp(tmpl);
    CHECK(root != nullptr);
    const std::string game = std::string(root) + "/game", overlay = std::string(root) + "/overlay";
    CHECK(std::system(("mkdir -p '" + game + "/movies' '" + overlay + "'").c_str()) == 0);
    CHECK(std::system(("printf 'not media' > '" + game + "/movies/present.bin'").c_str()) == 0);
    CHECK(gta6rt_shims_configure_paths(game.c_str(), overlay.c_str()) == GTA6RT_OK);
    uint64_t resolver = 0;
    CHECK_HR(MFCreateSourceResolver(&resolver), kSOk);
    uint32_t type = 0;
    uint64_t obj = 0;
    CHECK_HR(mt::call<int32_t>(resolver, 3, u"G:\\movies\\present.bin", 0x10001u, uint64_t{0}, &type, &obj),
             kMfEUnsupportedByteStreamType);
    CHECK_HR(mt::call<int32_t>(resolver, 3, u"G:\\movies\\absent.mp4", 0x10001u, uint64_t{0}, &type, &obj),
             kEFileNotFound);
    CHECK_HR(mt::call<int32_t>(resolver, 3, u"G:\\movies", 0x10001u, uint64_t{0}, &type, &obj), kEFileNotFound);
    mt::call<uint32_t>(resolver, 2);
    std::system(("rm -rf '" + std::string(root) + "'").c_str());
    return mt::finish("media_link_check");
#endif
}
