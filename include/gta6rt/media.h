#pragma once

/*
 * gta6rt_media: XAudio2 2.9 (xaudio2_9.dll, Bink2Scarlett's import) and the Media Foundation subset the title imports
 * (mfplat.dll, mfreadwrite.dll). Implemented by port/media. host/ calls gta6rt_media_register() after
 * gta6rt_shims_init(), next to gta6rt_shims_gdk_register().
 */

#include <gta6rt/abi.h>

GTA6RT_API_BEGIN

#define GTA6RT_MEDIA_API_VERSION 1

/* Registers xaudio2_9.dll (XAudio2Create, ordinal 1), mfplat.dll and mfreadwrite.dll with gta6rt_shims_register_dll.
 * x86_64 only: the exports and vtables are ms_abi functions; arm64 returns GTA6RT_E_UNSUPPORTED (as shims_gdk). */
gta6rt_status gta6rt_media_register(void);

GTA6RT_API_END
