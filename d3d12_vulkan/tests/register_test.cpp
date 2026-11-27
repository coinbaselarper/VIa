// gta6rt_d3d12_vulkan_register hands the d3d12_xs.dll table to shims_core; the title's three imports (and the rest of
// the table) must then resolve through gta6rt_shims_import_resolver. arm64 reports GTA6RT_E_UNSUPPORTED (no frame
// adapters for this table yet).
#include "register.h"

#include <gta6rt/isa.h>
#include <gta6rt/shims.h>

#include <cstdio>
#include <cstring>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}
}  // namespace

int main() {
    gta6rt_isa_config cfg{};
    check(gta6rt_isa_init(&cfg) == GTA6RT_OK, "isa_init");
    check(gta6rt_shims_init() == GTA6RT_OK, "shims_init");
    const gta6rt_status registered = gta6rt_d3d12_vulkan_register();
#if defined(__x86_64__)
    check(registered == GTA6RT_OK, "d3d12_vulkan_register");
    check(gta6rt_d3d12_vulkan_register() == GTA6RT_OK, "d3d12_vulkan_register again");
    const char* names[] = {"D3D12XboxCreateDevice", "D3D12ConfigureVirtualMemoryX", "D3D12SerializeVersionedRootSignature",
                           "D3D12SerializeRootSignature", "D3D12CreateRootSignatureDeserializer",
                           "D3D12CreateVersionedRootSignatureDeserializer"};
    for (const char* name : names) {
        gta6rt_import_ref ref{};
        ref.dll = "d3d12_xs.dll";
        ref.name = name;
        gta6rt_import_binding out{};
        const gta6rt_status s = gta6rt_shims_import_resolver(nullptr, &ref, &out);
        char what[160];
        std::snprintf(what, sizeof(what), "resolve d3d12_xs.dll!%s -> 0x%llx (%s)", name,
                      static_cast<unsigned long long>(out.address), gta6rt_status_name(s));
        check(s == GTA6RT_OK && out.address, what);
    }
    gta6rt_import_ref upper{};
    upper.dll = "D3D12_XS.DLL";
    upper.name = "D3D12XboxCreateDevice";
    gta6rt_import_binding out{};
    check(gta6rt_shims_import_resolver(nullptr, &upper, &out) == GTA6RT_OK && out.address, "resolve upper-case dll name");
    gta6rt_import_ref by_ordinal{};
    by_ordinal.dll = "d3d12_xs.dll";
    by_ordinal.ordinal = 7;
    out = {};
    check(gta6rt_shims_import_resolver(nullptr, &by_ordinal, &out) == GTA6RT_OK && out.address, "resolve ordinal 7");
    gta6rt_import_ref missing{};
    missing.dll = "d3d12_xs.dll";
    missing.name = "D3D12NoSuchExport";
    out = {};
    const gta6rt_status unknown = gta6rt_shims_import_resolver(nullptr, &missing, &out);
    std::printf("info unknown export -> %s, 0x%llx (shims_core's named trap thunk)\n", gta6rt_status_name(unknown),
                static_cast<unsigned long long>(out.address));
#else
    check(registered == GTA6RT_E_UNSUPPORTED, "d3d12_vulkan_register is unsupported on arm64");
#endif
    check(rt::d3d12_vulkan::export_count() == 7, "7 rows in the table");
    check(rt::d3d12_vulkan::find_export("d3d12_xs.dll", "D3D12XboxCreateDevice") != nullptr, "find_export");
    std::printf("register_test: %s (%d failed)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
