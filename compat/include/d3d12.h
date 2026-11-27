#pragma once

/*
 * Wrapper around DirectX-Headers d3d12.h. Known ABI gap for S1.6: without _WIN32 the headers declare
 * struct-returning methods (GetDesc, GetCPUDescriptorHandleForHeapStart, ... 21 in d3d12.h, 6 in d3d12video.h)
 * in the natural-return form. MSVC returns those through a hidden pointer, so guest-facing implementations
 * must use the explicit RetVal form, not these declarations.
 */

#include <windows.h>
#include_next <d3d12.h>
#include <gta6rt/dx_extra_uuids.h>
