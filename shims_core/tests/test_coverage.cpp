#include <cstring>
#include <string>
#include <vector>

#include "env_stub.h"
#include "guest_address.h"
#include "shims.h"
#include "test.h"

namespace {
const char* const kTrivial[] = {
#include "trivial_imports.inc"
};
}

TEST(coverage_all_223_trivial_imports_resolve_to_real_code) {
    int resolved = 0, missing = 0;
    std::string missing_names;
    for (const char* entry : kTrivial) {
        const char* bang = std::strchr(entry, '!');
        const std::string dll(entry, bang), name(bang + 1);
        gta6rt_gva addr = 0;
        bool stub = false;
        const gta6rt_status st = rt::shims::guest_address(dll, name, &addr, false, &stub);
        if (st == GTA6RT_OK && addr && !stub) ++resolved;
        else {
            ++missing;
            missing_names += " " + dll + "!" + name;
        }
    }
    if (missing) std::fprintf(stderr, "  not served:%s\n", missing_names.c_str());
    CHECK_EQ(resolved + missing, 223);
    CHECK_EQ(missing, 0);
}

TEST(coverage_unserved_import_gets_named_stub_never_silent_zero) {
    gta6rt_import_ref r{};
    r.dll = "KERNEL32.dll";
    r.name = "NoSuchFunctionAnywhere";
    r.kind_guess = GTA6RT_IMPORT_CODE;
    gta6rt_import_binding b{};
    CHECK_EQ(gta6rt_shims_import_resolver(nullptr, &r, &b), GTA6RT_OK);
    const stub::ThunkRecord* th = stub::thunk(b.address);
    CHECK(th && th->kind == GTA6RT_THUNK_TRAP);
    CHECK(th && std::strstr(th->name, "kernel32.dll!NoSuchFunctionAnywhere"));
}

namespace {
const char* const kLeonidaServed[] = {
#include "leonida_served_imports.inc"
};

// DLLs whose leonida shims are other workers' modules: shims_gdk (GameInput, xapu, chad, AcpHal, XCRDAPI/DStorage),
// d3d12_vulkan (d3d12_xs) and the window code of the host (CreateWindowExA).
bool owned_elsewhere(const std::string& dll, const std::string& name) {
    static const char* const dlls[] = {"gameinput.dll", "xapu.dll", "chad.dll", "acphal.dll", "xcrdapi.dll", "d3d12_xs.dll"};
    for (const char* d : dlls)
        if (dll == d) return true;
    return name == "CreateWindowExA";
}
}  // namespace

TEST(coverage_leonida_served_imports_are_ported_or_owned_elsewhere) {
    int served = 0, elsewhere = 0, missing = 0;
    std::string missing_names;
    for (const char* entry : kLeonidaServed) {
        const char* bang = std::strchr(entry, '!');
        const std::string dll(entry, bang), name(bang + 1);
        gta6rt_gva addr = 0;
        bool stub = false;
        if (rt::shims::guest_address(dll, name, &addr, false, &stub) == GTA6RT_OK && addr && !stub) ++served;
        else if (owned_elsewhere(dll, name)) ++elsewhere;
        else {
            ++missing;
            missing_names += " " + dll + "!" + name;
        }
    }
    std::fprintf(stderr, "  leonida-served imports: %d served by shims_core, %d owned by other modules, %d missing:%s\n", served, elsewhere,
                 missing, missing_names.c_str());
    CHECK_EQ(served + elsewhere + missing, 116);
    CHECK_EQ(missing, 0);
}
