#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <shared_mutex>

#include "loader_internal.h"

namespace {

gta6rt_loader_host_ops g_ops;
bool g_ops_ready = false;
std::once_flag g_ops_once;

void fill_defaults(gta6rt_loader_host_ops& o) {
    if (!o.vm_reserve) o.vm_reserve = gta6rt_vm_reserve;
    if (!o.vm_commit) o.vm_commit = gta6rt_vm_commit;
    if (!o.vm_protect) o.vm_protect = gta6rt_vm_protect;
    if (!o.vm_release) o.vm_release = gta6rt_vm_release;
    if (!o.vm_page_size) o.vm_page_size = gta6rt_vm_host_page_size;
    if (!o.file_map_readonly) o.file_map_readonly = gta6rt_file_map_readonly;
    if (!o.file_unmap) o.file_unmap = gta6rt_file_unmap;
    if (!o.make_thunk) o.make_thunk = gta6rt_isa_make_thunk;
    if (!o.patch_code) o.patch_code = gta6rt_isa_patch_code;
    if (!o.flush_icache) o.flush_icache = gta6rt_isa_flush_icache;
    if (!o.call_guest) o.call_guest = gta6rt_isa_call_guest;
    if (!o.tls_register_image) o.tls_register_image = gta6rt_isa_tls_register_image;
    if (!o.tls_unregister_image) o.tls_unregister_image = gta6rt_isa_tls_unregister_image;
    o.size = sizeof(o);
}

void vlog(gta6rt_log_level lvl, const char* fmt, va_list ap) {
    char buf[1024];
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    gta6rt_log(lvl, "loader", "%s", buf);
}

std::shared_mutex g_reg_mutex;
std::vector<gta6rt_image*> g_registry;

}  // namespace

void gta6rt_loader_set_host_ops(const gta6rt_loader_host_ops* ops) {
    g_ops = {};
    if (ops) {
        const size_t n = ops->size < sizeof(g_ops) ? ops->size : sizeof(g_ops);
        std::memcpy(&g_ops, ops, n);
    }
    fill_defaults(g_ops);
    g_ops_ready = true;
}

namespace ldr {

const gta6rt_loader_host_ops& ops() {
    std::call_once(g_ops_once, [] {
        if (!g_ops_ready) {
            g_ops = {};
            fill_defaults(g_ops);
            g_ops_ready = true;
        }
    });
    return g_ops;
}

#define LDR_LOG_FN(name, lvl)             \
    void name(const char* fmt, ...) {     \
        va_list ap;                       \
        va_start(ap, fmt);                \
        vlog(lvl, fmt, ap);               \
        va_end(ap);                       \
    }
LDR_LOG_FN(log_error, GTA6RT_LOG_ERROR)
LDR_LOG_FN(log_warn, GTA6RT_LOG_WARN)
LDR_LOG_FN(log_info, GTA6RT_LOG_INFO)
LDR_LOG_FN(log_debug, GTA6RT_LOG_DEBUG)
#undef LDR_LOG_FN

std::string lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}

void registry_add(Image* img) {
    std::unique_lock l(g_reg_mutex);
    g_registry.push_back(img);
}

void registry_remove(Image* img) {
    std::unique_lock l(g_reg_mutex);
    for (size_t i = 0; i < g_registry.size(); ++i)
        if (g_registry[i] == img) {
            g_registry.erase(g_registry.begin() + static_cast<long>(i));
            return;
        }
}

std::vector<Image*> registry_snapshot() {
    std::shared_lock l(g_reg_mutex);
    return g_registry;
}

}  // namespace ldr

const gta6rt_image* gta6rt_loader_find_image(gta6rt_gva addr) {
    std::shared_lock l(g_reg_mutex);
    for (const auto* img : g_registry) {
        const uint64_t b = reinterpret_cast<uint64_t>(img->base);
        if (addr >= b && addr - b < img->size) return img;
    }
    return nullptr;
}

void gta6rt_loader_describe_address(gta6rt_gva addr, char* buf, size_t buf_size) {
    if (!buf || !buf_size) return;
    if (const auto* img = gta6rt_loader_find_image(addr))
        std::snprintf(buf, buf_size, "%s+0x%llx", img->name.c_str(),
                      static_cast<unsigned long long>(addr - reinterpret_cast<uint64_t>(img->base)));
    else
        std::snprintf(buf, buf_size, "0x%llx", static_cast<unsigned long long>(addr));
}
