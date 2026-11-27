// Test-only stand-in for shims_core's xmem: the two rt::xmem calls dstorage makes, over a model of physical pages that
// are 64 KiB frames mapped into reserved windows. Registered under "xmem.dll" with the four exports the DirectStorage
// test drives (XMemVirtualAlloc, XMemAllocatePhysicalPages, XMemMapPhysicalPages, XMemFreePhysicalPages).
#include <windows.h>

#include <gta6rt/shims.h>
#include <sys/mman.h>

#include <cstring>
#include <map>
#include <mutex>
#include <span>
#include <vector>

#include "gdk_title_memory.h"
#include "xmem.h"

namespace {
constexpr size_t kPage = 65536;
std::mutex g_mu;
std::map<uint64_t, uint8_t*> g_frame_address;
uint64_t g_next_frame = 1;
}  // namespace

namespace rt::xmem {

bool transfer_physical(std::span<const std::uint64_t> pages, std::uint16_t offset, std::span<std::uint8_t> staging,
                       bool write) {
    std::lock_guard lock(g_mu);
    size_t needed = 1 + (staging.size() ? (staging.size() - 1 + offset) / kPage : 0);
    for (size_t i = 0; i < needed; ++i) {
        const auto it = i < pages.size() ? g_frame_address.find(pages[i]) : g_frame_address.end();
        if (it == g_frame_address.end() || !it->second) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return false;
        }
    }
    size_t done = 0;
    for (size_t i = 0; i < pages.size() && done < staging.size(); ++i) {
        const auto it = g_frame_address.find(pages[i]);
        if (it == g_frame_address.end() || !it->second) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return false;
        }
        const size_t start = i == 0 ? offset : 0;
        const size_t n = std::min(kPage - start, staging.size() - done);
        if (write) {
            std::memcpy(it->second + start, staging.data() + done, n);
            rt::gdk::title_texel_write(it->second + start, n);
        }
        else std::memcpy(staging.data() + done, it->second + start, n);
        done += n;
    }
    return done == staging.size();
}

}  // namespace rt::xmem

namespace {

void* WINAPI XMemVirtualAlloc(void* address, SIZE_T size, DWORD, ULONGLONG, DWORD) {
    void* p = ::mmap(address, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | (address ? MAP_FIXED : 0), -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

BOOL WINAPI XMemAllocatePhysicalPages(uint64_t, uint64_t* count, uint64_t* pages) {
    std::lock_guard lock(g_mu);
    for (uint64_t i = 0; i < *count; ++i) {
        pages[i] = g_next_frame++;
        g_frame_address[pages[i]] = nullptr;
    }
    return TRUE;
}

BOOL WINAPI XMemFreePhysicalPages(uint64_t count, const uint64_t* pages) {
    std::lock_guard lock(g_mu);
    for (uint64_t i = 0; i < count; ++i) g_frame_address.erase(pages[i]);
    return TRUE;
}

// A window page shows the frame's bytes; the model keeps one copy, so the window memory itself is the frame.
void* WINAPI XMemMapPhysicalPages(void* window, uint64_t count, const uint64_t* pages) {
    std::lock_guard lock(g_mu);
    auto* base = static_cast<uint8_t*>(window);
    for (uint64_t i = 0; i < count; ++i) {
        if (!pages) continue;
        g_frame_address[pages[i]] = base + i * kPage;
    }
    return window;
}

const gta6rt_shim_export kExports[] = {
    {"XMemVirtualAlloc", 0, GTA6RT_SHIM_CODE_NATIVE, reinterpret_cast<const void*>(&XMemVirtualAlloc)},
    {"XMemAllocatePhysicalPages", 0, GTA6RT_SHIM_CODE_NATIVE, reinterpret_cast<const void*>(&XMemAllocatePhysicalPages)},
    {"XMemFreePhysicalPages", 0, GTA6RT_SHIM_CODE_NATIVE, reinterpret_cast<const void*>(&XMemFreePhysicalPages)},
    {"XMemMapPhysicalPages", 0, GTA6RT_SHIM_CODE_NATIVE, reinterpret_cast<const void*>(&XMemMapPhysicalPages)},
    {nullptr, 0, GTA6RT_SHIM_CODE_NATIVE, nullptr},
};
}  // namespace

void gdk_test_register_xmem_fake() { gta6rt_shims_register_dll("xmem.dll", kExports); }
