#include "title_memory_probe.h"

#include "../src/title_va_alias.h"

#include <gta6rt/os.h>

#include <mach/mach.h>
#include <mach/mach_vm.h>

#include <atomic>
#include <cstdio>

namespace rt::d3d12_vulkan {
namespace {

std::atomic<unsigned> g_logged{0};
std::atomic<std::uint64_t> g_guest{0}, g_host{0}, g_missed{0};

void note(const char* what, std::uint64_t va, std::uint64_t lo, std::uint64_t hi) {
    if (g_logged.fetch_add(1, std::memory_order_relaxed) < 16)
        std::fprintf(stderr, "[rt] d3d12: title memory probe: 0x%llx %s [0x%llx, 0x%llx)\n",
                     static_cast<unsigned long long>(va), what, static_cast<unsigned long long>(lo),
                     static_cast<unsigned long long>(hi));
}

bool probe(std::uint64_t va, std::uint64_t& lo, std::uint64_t& hi) {
    gta6rt_vm_region region{};
    if (gta6rt_vm_query(reinterpret_cast<const void*>(va), &region) == GTA6RT_OK && !region.is_image &&
        region.state != GTA6RT_VM_STATE_FREE && region.allocation_base && region.allocation_size) {
        lo = reinterpret_cast<std::uintptr_t>(region.allocation_base);
        hi = lo + region.allocation_size;
        g_guest.fetch_add(1, std::memory_order_relaxed);
        note("is in a guest allocation", va, lo, hi);
        return true;
    }
    mach_vm_address_t address = va;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    const kern_return_t kr = mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
                                            reinterpret_cast<vm_region_info_t>(&info), &count, &object);
    if (kr == KERN_SUCCESS && address <= va && va < address + size &&
        (info.protection & (VM_PROT_READ | VM_PROT_WRITE)) == (VM_PROT_READ | VM_PROT_WRITE) && !info.shared) {
        lo = address;
        hi = address + size;
        g_host.fetch_add(1, std::memory_order_relaxed);
        note("is in a private host mapping (XMemAlloc heap)", va, lo, hi);
        return true;
    }
    g_missed.fetch_add(1, std::memory_order_relaxed);
    note("is in no guest allocation or writable private mapping", va, 0, 0);
    return false;
}

}  // namespace

void install_title_memory_probe() { leonida::title_va_aliases().set_memory_probe(&probe); }

void title_memory_probe_counts(std::uint64_t* guest, std::uint64_t* host, std::uint64_t* missed) {
    if (guest) *guest = g_guest.load(std::memory_order_relaxed);
    if (host) *host = g_host.load(std::memory_order_relaxed);
    if (missed) *missed = g_missed.load(std::memory_order_relaxed);
}

}  // namespace rt::d3d12_vulkan
