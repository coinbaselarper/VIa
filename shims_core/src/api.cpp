// The C interface of shims_core beyond the registry (include/gta6rt/shims.h, API version 2).
#include <string>
#include <vector>

#include <gta6rt/isa.h>
#include <gta6rt/os.h>
#include <gta6rt/shims.h>

#include "console_registry.h"
#include "crt_hooks.h"
#include "file_system.h"
#include "lifecycle.h"
#include "process_shim.h"
#include "shims.h"
#include "util.h"

namespace rt::crt {
void run_thread_exit_callback() {
    const uint64_t cb = thread_exit_callback();
    if (!cb) return;
    uint64_t base = 0;
    for (size_t i = 0; gta6rt_module* m = gta6rt_module_at(i); ++i)
        if (gta6rt_module_info(m)->kind == GTA6RT_MODULE_GUEST_IMAGE) {
            base = gta6rt_module_info(m)->base;
            break;
        }
    gta6rt_isa_call_guest4(cb, base, 3, 0, 0);
}
}  // namespace rt::crt

extern "C" {

gta6rt_status gta6rt_shims_configure_paths(const char* game_dir, const char* overlay_dir) {
    if (!game_dir || !overlay_dir) return GTA6RT_E_INVALID_ARG;
    rt::fs::configure(game_dir, overlay_dir);
    rt::shims::console_registry::set_store_dir(*overlay_dir ? std::string(overlay_dir) + "/.gta6rt-registry" : std::string());
    return GTA6RT_OK;
}

gta6rt_status gta6rt_shims_set_command_line(const char* program, const char* const* args, size_t count) {
    if (!program || (count && !args)) return GTA6RT_E_INVALID_ARG;
    std::vector<std::u16string> wide;
    for (size_t i = 0; i < count; ++i) wide.push_back(rt::widen(args[i]));
    rt::process::set_command_line(rt::widen(program), wide);
    return GTA6RT_OK;
}

int gta6rt_shims_run_entry(gta6rt_gva entry, size_t stack_bytes) { return rt::process::run_entry(entry, stack_bytes ? stack_bytes : (8u << 20)); }

void gta6rt_shims_request_suspend(void) { rt::lifecycle::request_suspend(); }

void gta6rt_shims_thread_detach(void) { rt::crt::run_thread_exit_callback(); }

size_t gta6rt_shims_report_unimplemented(void) {
    rt::shims::report_called_unimplemented();
    return rt::shims::unimplemented_call_count();
}

}  // extern "C"
