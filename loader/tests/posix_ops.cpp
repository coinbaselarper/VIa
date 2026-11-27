#include "posix_ops.h"

#include <gta6rt/os.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

namespace testops {
namespace {

std::mutex g_mu;
std::map<void*, size_t> g_regions;
std::vector<GuestCall> g_calls;
std::vector<std::string> g_thunks;
const char* g_mode = "default";
constexpr uint64_t kThunkBase = 0xFFFF000000000000ull;

int to_prot(uint32_t p) {
    int r = 0;
    if (p & GTA6RT_PROT_READ) r |= PROT_READ;
    if (p & GTA6RT_PROT_WRITE) r |= PROT_WRITE;
#if defined(__x86_64__)
    if (p & GTA6RT_PROT_EXEC) r |= PROT_EXEC;
#endif
    return r;
}

size_t g_res_plain = 0, g_res_image = 0;
std::map<std::string, size_t> g_file_maps;

struct TlsReg {
    gta6rt_tls_block_ctor ctor;
    gta6rt_tls_block_dtor dtor;
    void* user;
};
std::map<uint32_t, TlsReg> g_tls_regs;
std::vector<void**> g_tls_arrays;
uint32_t g_tid = 1;

gta6rt_status p_tls_register(uint32_t idx, gta6rt_tls_block_ctor ctor, gta6rt_tls_block_dtor dtor, void* user) {
    std::lock_guard l(g_mu);
    if (idx >= kTlsSlots || g_tls_regs.count(idx)) return GTA6RT_E_INVALID_ARG;
    g_tls_regs[idx] = {ctor, dtor, user};
    for (auto* a : g_tls_arrays) a[idx] = ctor(idx, g_tid++, user);
    return GTA6RT_OK;
}

gta6rt_status p_tls_unregister(uint32_t idx) {
    std::lock_guard l(g_mu);
    auto it = g_tls_regs.find(idx);
    if (it == g_tls_regs.end()) return GTA6RT_E_NOT_FOUND;
    for (auto* a : g_tls_arrays) {
        if (a[idx]) it->second.dtor(a[idx], it->second.user);
        a[idx] = nullptr;
    }
    g_tls_regs.erase(it);
    return GTA6RT_OK;
}

gta6rt_status p_reserve(void* addr, size_t size, uint32_t flags, void** out) {
    {
        std::lock_guard l(g_mu);
        ++((flags & GTA6RT_VM_IMAGE) ? g_res_image : g_res_plain);
    }
    void* p = mmap(addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return GTA6RT_E_NO_MEMORY;
    if ((flags & GTA6RT_VM_FIXED) && p != addr) {
        munmap(p, size);
        return GTA6RT_E_ADDRESS_IN_USE;
    }
    std::lock_guard l(g_mu);
    g_regions[p] = size;
    *out = p;
    return GTA6RT_OK;
}

gta6rt_status p_commit(void* addr, size_t size, uint32_t prot) {
    return mprotect(addr, size, to_prot(prot)) == 0 ? GTA6RT_OK : GTA6RT_E_ACCESS;
}

gta6rt_status p_protect(void* addr, size_t size, uint32_t prot, uint32_t* old) {
    if (old) *old = 0;
    return mprotect(addr, size, to_prot(prot)) == 0 ? GTA6RT_OK : GTA6RT_E_ACCESS;
}

gta6rt_status p_release(void* base) {
    std::lock_guard l(g_mu);
    auto it = g_regions.find(base);
    if (it == g_regions.end()) return GTA6RT_E_NOT_FOUND;
    munmap(base, it->second);
    g_regions.erase(it);
    return GTA6RT_OK;
}

size_t p_page() { return static_cast<size_t>(getpagesize()); }

gta6rt_status p_map_file(const char* path, const void** data, size_t* size) {
    {
        std::lock_guard l(g_mu);
        ++g_file_maps[path];
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) return GTA6RT_E_NOT_FOUND;
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return GTA6RT_E_IO;
    }
    void* p = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return GTA6RT_E_IO;
    *data = p;
    *size = static_cast<size_t>(st.st_size);
    return GTA6RT_OK;
}

void p_unmap_file(const void* data, size_t size) { munmap(const_cast<void*>(data), size); }

gta6rt_status p_thunk(const gta6rt_thunk_desc* d, gta6rt_gva* out) {
    std::lock_guard l(g_mu);
    g_thunks.push_back(d->debug_name ? d->debug_name : "?");
    *out = kThunkBase + 16 * (g_thunks.size() - 1);
    return GTA6RT_OK;
}

gta6rt_status p_patch(gta6rt_gva, const void*, size_t) { return GTA6RT_E_NOT_IMPLEMENTED; }
void p_flush(gta6rt_gva, size_t) {}

gta6rt_status p_call(gta6rt_gva target, const gta6rt_guest_args* a, gta6rt_guest_ret* r) {
    std::lock_guard l(g_mu);
    g_calls.push_back({target, a->args[0], a->args[1], a->args[2]});
    r->rax = 1;
    r->xmm0_lo = 0;
    return GTA6RT_OK;
}

}  // namespace

bool os_available() {
    void* b = nullptr;
    auto st = gta6rt_vm_reserve(nullptr, 1 << 16, 0, &b);
    if (st == GTA6RT_OK) gta6rt_vm_release(b);
    return st != GTA6RT_E_NOT_IMPLEMENTED;
}

void install(bool use_os) {
    if (use_os) {
        gta6rt_loader_host_ops o{};
        o.size = sizeof o;
        o.call_guest = p_call;
        o.make_thunk = p_thunk;
        o.tls_register_image = p_tls_register;
        o.tls_unregister_image = p_tls_unregister;
        gta6rt_loader_set_host_ops(&o);
        g_mode = "gta6rt_os memory, synthetic thunks, recording call_guest";
        return;
    }
    gta6rt_loader_host_ops o{};
    o.size = sizeof o;
    o.vm_reserve = p_reserve;
    o.vm_commit = p_commit;
    o.vm_protect = p_protect;
    o.vm_release = p_release;
    o.vm_page_size = p_page;
    o.file_map_readonly = p_map_file;
    o.file_unmap = p_unmap_file;
    o.make_thunk = p_thunk;
    o.patch_code = p_patch;
    o.flush_icache = p_flush;
    o.call_guest = p_call;
    o.tls_register_image = p_tls_register;
    o.tls_unregister_image = p_tls_unregister;
    gta6rt_loader_set_host_ops(&o);
    g_mode = "posix mmap, synthetic thunks, recording call_guest (os/isa are placeholders)";
}

const char* mode() { return g_mode; }
const std::vector<GuestCall>& guest_calls() { return g_calls; }
void clear_guest_calls() { g_calls.clear(); }
bool is_synthetic_thunk(uint64_t a) { return a >= kThunkBase && a < kThunkBase + 16 * g_thunks.size(); }
std::string thunk_name(uint64_t a) { return is_synthetic_thunk(a) ? g_thunks[(a - kThunkBase) / 16] : ""; }
void** attach_thread() {
    auto* a = static_cast<void**>(std::calloc(kTlsSlots, sizeof(void*)));
    std::lock_guard l(g_mu);
    for (auto& [idx, r] : g_tls_regs) a[idx] = r.ctor(idx, g_tid++, r.user);
    g_tls_arrays.push_back(a);
    return a;
}

void detach_thread(void** a) {
    if (!a) return;
    std::lock_guard l(g_mu);
    for (auto& [idx, r] : g_tls_regs)
        if (a[idx]) r.dtor(a[idx], r.user);
    g_tls_arrays.erase(std::find(g_tls_arrays.begin(), g_tls_arrays.end(), a));
    std::free(a);
}

size_t registered_tls_images() {
    std::lock_guard l(g_mu);
    return g_tls_regs.size();
}
size_t reserves_without_image_flag() { return g_res_plain; }
size_t reserves_with_image_flag() { return g_res_image; }
size_t file_map_count(const std::string& suffix) {
    std::lock_guard l(g_mu);
    size_t n = 0;
    for (auto& [k, v] : g_file_maps)
        if (k.size() >= suffix.size() && k.compare(k.size() - suffix.size(), suffix.size(), suffix) == 0) n += v;
    return n;
}
size_t thunk_count() { return g_thunks.size(); }

}  // namespace testops
