#include "internal.h"

#include <cerrno>
#include <cstdlib>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <malloc/malloc.h>
#include <sys/mman.h>

using namespace gta6rt_os;

namespace {

constexpr size_t kPage = GTA6RT_GUEST_PAGE_SIZE;

DWORD vm_err(gta6rt_status st) {
    switch (st) {
    case GTA6RT_E_ADDRESS_IN_USE:
    case GTA6RT_E_NOT_FOUND:
    case GTA6RT_E_ACCESS: return ERROR_INVALID_ADDRESS;
    case GTA6RT_E_NO_MEMORY: return ERROR_NOT_ENOUGH_MEMORY;
    default: return ERROR_INVALID_PARAMETER;
    }
}

bool self_process(HANDLE h) { return H(h) == kPseudoProcess || !h; }

uint32_t vm_flags(DWORD type) {
    uint32_t f = 0;
    if (type & MEM_WRITE_WATCH) f |= GTA6RT_VM_WRITE_WATCH;
    if (type & MEM_LARGE_PAGES) f |= GTA6RT_VM_LARGE_PAGES;
    if (type & MEM_TOP_DOWN) f |= GTA6RT_VM_TOP_DOWN;
    return f;
}

LPVOID alloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect, uint32_t extra_flags) {
    if (!size) {
        set_error(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (type & MEM_RESET) {
        gta6rt_status st = gta6rt_vm_reset(addr, size);
        if (st != GTA6RT_OK) {
            set_error(vm_err(st));
            return nullptr;
        }
        return addr;
    }
    if (type & MEM_RESET_UNDO) return addr;
    bool ok = true;
    uint32_t prot = win32_to_prot(protect & 0x1FF, &ok);
    bool commit = (type & MEM_COMMIT) != 0;
    bool reserve = (type & (MEM_RESERVE | MEM_RESERVE_PLACEHOLDER)) != 0;
    if (!ok && (commit || !(type & MEM_RESERVE_PLACEHOLDER))) {
        set_error(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (type & MEM_REPLACE_PLACEHOLDER) {
        gta6rt_status st = gta6rt_vm_placeholder_replace(addr, size, vm_flags(type));
        if (st != GTA6RT_OK) {
            set_error(ERROR_INVALID_ADDRESS);
            return nullptr;
        }
        if (commit && (st = gta6rt_vm_commit(addr, size, prot)) != GTA6RT_OK) {
            set_error(vm_err(st));
            return nullptr;
        }
        return addr;
    }
    if (!reserve && !commit) {
        set_error(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (reserve || !addr) {
        uint32_t flags = vm_flags(type) | extra_flags;
        if (type & MEM_RESERVE_PLACEHOLDER) flags |= GTA6RT_VM_PLACEHOLDER;
        if (addr) flags |= GTA6RT_VM_FIXED;
        void* base = nullptr;
        gta6rt_status st = gta6rt_vm_reserve(addr, size, flags, &base);
        if (st != GTA6RT_OK) {
            set_error(vm_err(st));
            return nullptr;
        }
        void* caddr = addr ? addr : base;
        if (commit && (st = gta6rt_vm_commit(caddr, size, prot)) != GTA6RT_OK) {
            gta6rt_vm_release(base);
            set_error(vm_err(st));
            return nullptr;
        }
        return base;
    }
    gta6rt_status st = gta6rt_vm_commit(addr, size, prot);
    if (st != GTA6RT_OK) {
        set_error(vm_err(st));
        return nullptr;
    }
    return (LPVOID)((uintptr_t)addr & ~(uintptr_t)(kPage - 1));
}

} // namespace

extern "C" {

LPVOID WINAPI VirtualAlloc(LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect) {
    return alloc(lpAddress, dwSize, flAllocationType, flProtect, 0);
}

LPVOID WINAPI VirtualAllocEx(HANDLE hProcess, LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect) {
    if (!self_process(hProcess)) {
        set_error(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    return alloc(lpAddress, dwSize, flAllocationType, flProtect, 0);
}

PVOID WINAPI VirtualAlloc2(HANDLE Process, PVOID BaseAddress, SIZE_T Size, ULONG AllocationType, ULONG PageProtection,
                           MEM_EXTENDED_PARAMETER* params, ULONG count) {
    if (!self_process(Process)) {
        set_error(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    uint32_t extra = 0;
    for (ULONG i = 0; params && i < count; i++) {
        if (params[i].Type == MemExtendedParameterAddressRequirements && params[i].Pointer) {
            auto* req = (MEM_ADDRESS_REQUIREMENTS*)params[i].Pointer;
            if (req->Alignment > (2u << 20)) {
                set_error(ERROR_NOT_SUPPORTED);
                return nullptr;
            }
            if (req->Alignment > GTA6RT_GUEST_ALLOCATION_GRANULARITY) extra |= GTA6RT_VM_LARGE_PAGES;
        }
    }
    return alloc(BaseAddress, Size, AllocationType, PageProtection, extra);
}

BOOL WINAPI VirtualFree(LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType) {
    if ((dwFreeType & MEM_RELEASE) && (dwFreeType & MEM_DECOMMIT)) {
        set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (dwFreeType & MEM_RELEASE) {
        gta6rt_status st;
        if (dwFreeType & MEM_PRESERVE_PLACEHOLDER) st = gta6rt_vm_placeholder_split(lpAddress, dwSize);
        else if (dwFreeType & MEM_COALESCE_PLACEHOLDERS) st = gta6rt_vm_placeholder_coalesce(lpAddress, dwSize);
        else if (dwSize) st = GTA6RT_E_INVALID_ARG;
        else st = gta6rt_vm_release(lpAddress);
        if (st != GTA6RT_OK) {
            set_error(st == GTA6RT_E_NOT_FOUND ? ERROR_INVALID_ADDRESS : ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        return TRUE;
    }
    if (dwFreeType & MEM_DECOMMIT) {
        size_t n = dwSize;
        if (!n) {
            gta6rt_vm_region r;
            if (gta6rt_vm_query(lpAddress, &r) != GTA6RT_OK || r.state == GTA6RT_VM_STATE_FREE) {
                set_error(ERROR_INVALID_ADDRESS);
                return FALSE;
            }
            n = (uintptr_t)r.allocation_base + r.allocation_size - (uintptr_t)lpAddress;
        }
        gta6rt_status st = gta6rt_vm_decommit(lpAddress, n);
        if (st != GTA6RT_OK) {
            set_error(ERROR_INVALID_ADDRESS);
            return FALSE;
        }
        return TRUE;
    }
    set_error(ERROR_INVALID_PARAMETER);
    return FALSE;
}

BOOL WINAPI VirtualProtect(LPVOID lpAddress, SIZE_T dwSize, DWORD flNewProtect, PDWORD lpflOldProtect) {
    bool ok;
    uint32_t prot = win32_to_prot(flNewProtect & 0x1FF, &ok);
    if (!ok) {
        set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    uint32_t old = 0;
    gta6rt_status st = gta6rt_vm_protect(lpAddress, dwSize, prot, &old);
    if (st != GTA6RT_OK) {
        set_error(st == GTA6RT_E_NOT_FOUND ? ERROR_INVALID_ADDRESS : status_to_win32(st));
        return FALSE;
    }
    if (lpflOldProtect) *lpflOldProtect = prot_to_win32(old);
    return TRUE;
}

SIZE_T WINAPI VirtualQuery(LPCVOID lpAddress, PMEMORY_BASIC_INFORMATION mbi, SIZE_T dwLength) {
    if (dwLength < sizeof(MEMORY_BASIC_INFORMATION) || !mbi) {
        set_error(24 /* ERROR_BAD_LENGTH */);
        return 0;
    }
    gta6rt_vm_region r;
    if (gta6rt_vm_query(lpAddress, &r) != GTA6RT_OK) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    memset(mbi, 0, sizeof *mbi);
    mbi->BaseAddress = r.base;
    mbi->RegionSize = r.size;
    if (r.state == GTA6RT_VM_STATE_FREE) {
        mbi->State = MEM_FREE;
        mbi->Protect = PAGE_NOACCESS;
        return sizeof *mbi;
    }
    mbi->AllocationBase = r.allocation_base;
    mbi->AllocationProtect = prot_to_win32(r.allocation_prot);
    mbi->State = r.state == GTA6RT_VM_STATE_COMMITTED ? MEM_COMMIT : MEM_RESERVE;
    mbi->Protect = r.state == GTA6RT_VM_STATE_COMMITTED ? prot_to_win32(r.prot) : 0;
    int t = vm_region_type(lpAddress);
    mbi->Type = r.is_image || t == 3 ? MEM_IMAGE : t == 2 ? MEM_MAPPED : MEM_PRIVATE;
    return sizeof *mbi;
}

SIZE_T WINAPI VirtualQueryEx(HANDLE hProcess, LPCVOID lpAddress, PMEMORY_BASIC_INFORMATION mbi, SIZE_T dwLength) {
    if (!self_process(hProcess)) {
        set_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    return VirtualQuery(lpAddress, mbi, dwLength);
}

BOOL WINAPI VirtualLock(LPVOID lpAddress, SIZE_T dwSize) {
    mlock(lpAddress, dwSize);
    return TRUE;
}

BOOL WINAPI VirtualUnlock(LPVOID lpAddress, SIZE_T dwSize) {
    munlock(lpAddress, dwSize);
    return TRUE;
}

UINT WINAPI GetWriteWatch(DWORD dwFlags, PVOID base, SIZE_T size, PVOID* addrs, ULONG_PTR* count, LPDWORD gran) {
    if (!count) {
        set_error(ERROR_INVALID_PARAMETER);
        return (UINT)-1;
    }
    size_t n = *count;
    gta6rt_status st = gta6rt_vm_get_write_watch(base, size, (dwFlags & WRITE_WATCH_FLAG_RESET) != 0, addrs, &n);
    if (st != GTA6RT_OK) {
        set_error(ERROR_INVALID_PARAMETER);
        return (UINT)-1;
    }
    *count = n;
    if (gran) *gran = (DWORD)kPage;
    return 0;
}

UINT WINAPI ResetWriteWatch(LPVOID base, SIZE_T size) {
    size_t n = 0;
    gta6rt_status st = gta6rt_vm_get_write_watch(base, size, 1, nullptr, &n);
    if (st != GTA6RT_OK) {
        set_error(ERROR_INVALID_PARAMETER);
        return (UINT)-1;
    }
    return 0;
}

BOOL WINAPI ReadProcessMemory(HANDLE hProcess, LPCVOID base, LPVOID buf, SIZE_T n, SIZE_T* out) {
    if (!self_process(hProcess)) {
        set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    mach_vm_size_t got = 0;
    kern_return_t kr = mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)base, n,
                                              (mach_vm_address_t)(uintptr_t)buf, &got);
    if (out) *out = kr == KERN_SUCCESS ? (SIZE_T)got : 0;
    if (kr != KERN_SUCCESS) {
        set_error(ERROR_PARTIAL_COPY);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI WriteProcessMemory(HANDLE hProcess, LPVOID base, LPCVOID buf, SIZE_T n, SIZE_T* out) {
    if (!self_process(hProcess)) {
        set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    kern_return_t kr = mach_vm_write(mach_task_self(), (mach_vm_address_t)(uintptr_t)base, (vm_offset_t)(uintptr_t)buf,
                                     (mach_msg_type_number_t)n);
    if (out) *out = kr == KERN_SUCCESS ? n : 0;
    if (kr != KERN_SUCCESS) {
        set_error(ERROR_NOACCESS);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI FlushInstructionCache(HANDLE, LPCVOID base, SIZE_T n) {
#if defined(__aarch64__)
    if (base && n) sys_icache_invalidate((void*)base, n);
#else
    (void)base, (void)n;
#endif
    return TRUE;
}

BOOL WINAPI PrefetchVirtualMemory(HANDLE, ULONG_PTR count, PWIN32_MEMORY_RANGE_ENTRY e, ULONG) {
    size_t hp = host_page();
    for (ULONG_PTR i = 0; i < count; i++) {
        uintptr_t a = (uintptr_t)e[i].VirtualAddress & ~(uintptr_t)(hp - 1);
        madvise((void*)a, (uintptr_t)e[i].VirtualAddress + e[i].NumberOfBytes - a, MADV_WILLNEED);
    }
    return TRUE;
}

BOOL WINAPI IsBadReadPtr(const void* lp, UINT_PTR ucb) {
    if (!ucb) return FALSE;
    if (!lp) return TRUE;
    uintptr_t a = (uintptr_t)lp, e = a + ucb;
    while (a < e) {
        gta6rt_vm_region r;
        if (gta6rt_vm_query((void*)a, &r) != GTA6RT_OK || r.state != GTA6RT_VM_STATE_COMMITTED ||
            !(r.prot & GTA6RT_PROT_READ) || (r.prot & GTA6RT_PROT_GUARD))
            return TRUE;
        a = (uintptr_t)r.base + r.size;
    }
    return FALSE;
}

BOOL WINAPI IsBadWritePtr(LPVOID lp, UINT_PTR ucb) {
    if (!ucb) return FALSE;
    if (!lp) return TRUE;
    uintptr_t a = (uintptr_t)lp, e = a + ucb;
    while (a < e) {
        gta6rt_vm_region r;
        if (gta6rt_vm_query((void*)a, &r) != GTA6RT_OK || r.state != GTA6RT_VM_STATE_COMMITTED ||
            !(r.prot & GTA6RT_PROT_WRITE) || (r.prot & GTA6RT_PROT_GUARD))
            return TRUE;
        a = (uintptr_t)r.base + r.size;
    }
    return FALSE;
}

// ---- heaps: all on the host allocator so HeapFree(GetProcessHeap()) of CRT memory works ----

HANDLE WINAPI GetProcessHeap(void) { return HV(0x50000); }

HANDLE WINAPI HeapCreate(DWORD, SIZE_T, SIZE_T) {
    static std::atomic<uint64_t> next{0x50010};
    return HV(next.fetch_add(0x10));
}

BOOL WINAPI HeapDestroy(HANDLE) { return TRUE; }

LPVOID WINAPI HeapAlloc(HANDLE, DWORD dwFlags, SIZE_T n) {
    void* p = (dwFlags & HEAP_ZERO_MEMORY) ? calloc(1, n ? n : 1) : malloc(n ? n : 1);
    if (!p) {
        set_error(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    // HeapReAlloc(HEAP_ZERO_MEMORY) zeroes from the old usable size, so the slack past n must start zeroed.
    size_t usable = malloc_size(p);
    if (usable > n) memset((char*)p + n, 0, usable - n);
    return p;
}

LPVOID WINAPI HeapReAlloc(HANDLE, DWORD dwFlags, LPVOID p, SIZE_T n) {
    if (!p) return HeapAlloc(nullptr, dwFlags, n);
    size_t old = malloc_size(p);
    if (dwFlags & HEAP_REALLOC_IN_PLACE_ONLY) {
        if (n <= old) return p;
        set_error(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    void* q = realloc(p, n ? n : 1);
    if (!q) {
        set_error(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    if ((dwFlags & HEAP_ZERO_MEMORY) && n > old) memset((char*)q + old, 0, n - old);
    return q;
}

BOOL WINAPI HeapFree(HANDLE, DWORD, LPVOID p) {
    free(p);
    return TRUE;
}

SIZE_T WINAPI HeapSize(HANDLE, DWORD, LPCVOID p) { return p ? malloc_size(p) : (SIZE_T)-1; }

HLOCAL WINAPI LocalAlloc(UINT uFlags, SIZE_T n) { return HeapAlloc(nullptr, (uFlags & LMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0, n); }

HLOCAL WINAPI LocalFree(HLOCAL h) {
    free(h);
    return nullptr;
}

HGLOBAL WINAPI GlobalAlloc(UINT uFlags, SIZE_T n) { return HeapAlloc(nullptr, (uFlags & GMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0, n); }

HGLOBAL WINAPI GlobalFree(HGLOBAL h) {
    free(h);
    return nullptr;
}

} // extern "C"
