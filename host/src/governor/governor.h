#pragma once
#include <cstdint>
#include <string>

namespace host::governor {

enum class Mode { off, observe, on };

enum Action : uint32_t {
    kActLayerEvict = 1u << 0,
    kActCompileShrink = 1u << 1,
    kActHostCaches = 1u << 2,
    kActPoolPages = 1u << 3,
    kActWarn = 1u << 4,
    kActAll = 0x1f
};

struct Options {
    Mode mode = Mode::on;
    std::string budget = "auto";
    uint32_t actions = kActAll;
    unsigned poll_ms = 500;
    unsigned report_s = 0;
};

struct Budget {
    uint64_t physical = 0;
    uint64_t os_reserve = 0;
    uint64_t static_ceiling = 0;
    uint64_t available_now = 0;
    uint64_t dynamic_ceiling = 0;
    uint64_t budget = 0;
    bool fixed = false;
    uint64_t title_s = 0, title_x = 0, host_add = 0;
};

bool parse_mode(const char* text, Mode* out);
bool parse_actions(const char* text, uint32_t* out);
bool parse_size(const char* text, uint64_t* out);

// Before the first vm reservation: takes the periodic memory report over from os/ when report_s is set.
void prepare(const Options& opt);
Budget compute_budget(uint64_t own_footprint);
const char* profile_for_budget(const Budget& b, std::string* why);
// After shims_init, before the exe's imports are bound: counts the title's MEM_RESET/MEM_DECOMMIT/MEM_RELEASE.
void install_title_hooks();
void start(uint64_t exe_base);
void finish(const char* why);
// Action totals and the governor's own cost; no memory walk (also called from the fatal-signal path).
void summary();

}  // namespace host::governor
