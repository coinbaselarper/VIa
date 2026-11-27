// gta6rt_isa_reexec_for_cpuid: started without ROSETTA_ADVERTISE_AVX, the process re-executes itself once and then
// sees the S0.6 policy in Rosetta's own cpuid answer.

#include <gta6rt/isa.h>

#include "test_common.h"

#include <unistd.h>

int main(int argc, char** argv) {
    (void)argc;
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* re = getenv("GTA6RT_ISA_REEXECED");
    std::printf("isa_reexec_test: pid %d, phase %s, advertised 0x%llx\n", getpid(), re ? "after re-exec" : "first start",
                (unsigned long long)gta6rt_isa_advertised_features());
    const gta6rt_status st = gta6rt_isa_reexec_for_cpuid(argv);
    CHECK(st == GTA6RT_OK, "reexec_for_cpuid returned %d", st);
    CHECK(gta6rt_isa_advertised_features() == GTA6RT_CPUID_DEFAULT, "advertised 0x%llx after the call",
          (unsigned long long)gta6rt_isa_advertised_features());
    CHECK(gta6rt_isa_cpuid_enforced() == 1, "cpuid enforced without patching any site");
    return finish("isa_reexec_test");
}
