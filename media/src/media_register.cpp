#include <gta6rt/media.h>
#include <gta6rt/shims.h>

#include "media_common.h"
#include "media_exports.h"
#include "shims.h"

#include <mutex>

namespace rt::media {
namespace {

using GetFileAttributesWFn = uint32_t(GTA6RT_GUEST_ABI*)(const char16_t*);

bool shims_file_probe(const char16_t* path) {
    auto fn = reinterpret_cast<GetFileAttributesWFn>(rt::shims::find("kernel32.dll", "GetFileAttributesW"));
    if (!fn) return true;
    const uint32_t attrs = fn(path);
    return attrs != 0xFFFFFFFFu && !(attrs & 0x10u);
}

#define X(name, ord, fn) {name, ord, GTA6RT_SHIM_CODE_NATIVE, reinterpret_cast<const void*>(&fn)}

const gta6rt_shim_export kXAudio2[] = {
    X("XAudio2Create", 1, XAudio2Create),
    {nullptr, 0, GTA6RT_SHIM_CODE_NATIVE, nullptr},
};

const gta6rt_shim_export kMfPlat[] = {
    X("MFStartup", 0, MFStartup),
    X("MFShutdown", 0, MFShutdown),
    X("MFGetSystemTime", 0, MFGetSystemTime),
    X("MFCreateAttributes", 0, MFCreateAttributes),
    X("MFCreateMediaType", 0, MFCreateMediaType),
    X("MFCreateSourceResolver", 0, MFCreateSourceResolver),
    {nullptr, 0, GTA6RT_SHIM_CODE_NATIVE, nullptr},
};

const gta6rt_shim_export kMfReadWrite[] = {
    X("MFCreateSourceReaderFromMediaSource", 0, MFCreateSourceReaderFromMediaSource),
    {nullptr, 0, GTA6RT_SHIM_CODE_NATIVE, nullptr},
};

[[maybe_unused]] gta6rt_status register_tables() {
    set_file_probe(&shims_file_probe);
    gta6rt_status s = gta6rt_shims_register_dll("xaudio2_9.dll", kXAudio2);
    if (s == GTA6RT_OK) s = gta6rt_shims_register_dll("mfplat.dll", kMfPlat);
    if (s == GTA6RT_OK) s = gta6rt_shims_register_dll("mfreadwrite.dll", kMfReadWrite);
    MLOG(INFO, "media", "registered xaudio2_9.dll (1), mfplat.dll (6), mfreadwrite.dll (1): %s", gta6rt_status_name(s));
    return s;
}

}  // namespace
}  // namespace rt::media

extern "C" gta6rt_status gta6rt_media_register(void) {
#if defined(__x86_64__)
    static std::once_flag once;
    static gta6rt_status result = GTA6RT_OK;
    std::call_once(once, [] { result = rt::media::register_tables(); });
    return result;
#else
    return GTA6RT_E_UNSUPPORTED;
#endif
}
