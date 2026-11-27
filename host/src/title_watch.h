#pragma once
#include <cstdint>
#include <string>

namespace host::title {

void start_peek(uint64_t exe_base, const std::string& spec, unsigned period_s, bool stages);
bool start_watchdog_kick(uint64_t exe_base);
bool patch_null_pso_bind(uint64_t exe_base, uint8_t out_patch[12], uint32_t* out_rva);
uint64_t null_pso_skips();
bool patch_fsr_null_pipeline(uint64_t dll_base, uint8_t out_patch[13], uint32_t* out_rva);
uint64_t fsr_skips();

}  // namespace host::title
