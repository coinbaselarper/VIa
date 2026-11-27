#pragma once

#include <gta6rt/loader.h>

#include <cstdint>
#include <string>
#include <vector>

namespace testops {

struct GuestCall {
    uint64_t target;
    uint64_t a0, a1, a2;
};

// Installs host ops for running the loader without os/isa/seh: mmap-backed memory, synthetic thunk addresses and a
// recording call_guest that never executes guest code. With use_os, the real gta6rt_os / gta6rt_isa are kept.
void install(bool use_os);
const char* mode();

const std::vector<GuestCall>& guest_calls();
void clear_guest_calls();
bool is_synthetic_thunk(uint64_t addr);
std::string thunk_name(uint64_t addr);
size_t thunk_count();
bool os_available();

// Stand-in for isa's implicit-TLS ownership: attach_thread() builds the ThreadLocalStoragePointer array for a new
// thread from every registered image; images registered later are added to every live array.
constexpr uint32_t kTlsSlots = 64;
void** attach_thread();
void detach_thread(void** array);
size_t registered_tls_images();
// Number of vm_reserve calls made without / with GTA6RT_VM_IMAGE (the loader only reserves images).
size_t reserves_without_image_flag();
size_t reserves_with_image_flag();
size_t file_map_count(const std::string& path_suffix);

}  // namespace testops
