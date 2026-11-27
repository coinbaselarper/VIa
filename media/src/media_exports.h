#pragma once

#include "guest_abi.h"

#include <cstdint>

// Guest-callable exports of xaudio2_9.dll, mfplat.dll and mfreadwrite.dll (GTA6RT_GUEST_ABI on x86_64).
namespace rt::media {

GTA6RT_GUEST_ABI HRESULT XAudio2Create(uint64_t* out, uint32_t flags, uint32_t processor);

GTA6RT_GUEST_ABI HRESULT MFStartup(uint32_t version, uint32_t flags);
GTA6RT_GUEST_ABI HRESULT MFShutdown();
GTA6RT_GUEST_ABI int64_t MFGetSystemTime();
GTA6RT_GUEST_ABI HRESULT MFCreateAttributes(uint64_t* out, uint32_t initial_size);
GTA6RT_GUEST_ABI HRESULT MFCreateMediaType(uint64_t* out);
GTA6RT_GUEST_ABI HRESULT MFCreateSourceResolver(uint64_t* out);
GTA6RT_GUEST_ABI HRESULT MFCreateSourceReaderFromMediaSource(uint64_t source, uint64_t attributes, uint64_t* out);

// Whether a guest path names an existing file, for IMFSourceResolver (default: shims_core's GetFileAttributesW row).
using FileProbe = bool (*)(const char16_t* path);
void set_file_probe(FileProbe probe);

// Attribute store introspection for tests: true if `object` is an IMFAttributes/IMFMediaType made here.
bool is_media_attributes(uint64_t object);

}  // namespace rt::media
