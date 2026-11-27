#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "gta6rt_hud/hud.h"

namespace gta6rt::hud {

struct Raw {
    double t_s = 0;
    std::uint64_t have = 0;
    std::uint64_t presents = 0, frame_intervals = 0, frame_ns_sum = 0, frame_ns_max = 0;
    std::uint64_t draws = 0, dispatches = 0, submissions = 0, exec_cpu_ns = 0;
    std::uint64_t gpu_ns = 0, gpu_samples = 0;
    std::uint64_t latency_ns_sum = 0, latency_samples = 0, latency_ns_max = 0;
    std::uint64_t pipelines_built = 0, pipelines_pending = 0, cache_hits = 0, pso_failures = 0;
    std::uint64_t textures_waiting = 0, imports = 0;
    bool imports_valid = false;
    std::uint64_t footprint = 0, compressed = 0, resident = 0, ram = 0;
    int pressure = 0;
    double since_present_s = 0;
    std::string state, driver, profile;
    bool have_memory = false;
    std::uint64_t budget = 0, gpu_bytes = 0, gpu_limit = 0, governor_level = 0, mem_warning = 0;
    std::string mem_note;
};

struct Metrics {
    bool have_frames = false;
    double stall_s = 0;
    double fps = 0, fps_smooth = 0;
    double frame_avg_ms = 0, frame_max_ms = 0;
    double draws_pf = 0, dispatches_pf = 0, submissions_pf = 0, exec_ms_pf = 0, gpu_ms_pf = 0;
    double latency_ms = 0;
    double imports_per_s = 0;
    bool have_submissions = false, have_exec = false, have_dispatches = false, have_gpu = false, have_latency = false,
         have_pipelines = false, have_textures = false, have_imports = false;
    std::uint64_t pipelines_built = 0, pipelines_pending = 0, cache_hits = 0, pso_failures = 0, textures_waiting = 0;
    double footprint_gib = 0, compressed_gib = 0, resident_gib = 0, ram_gib = 0;
    int pressure = 0;
    std::string state, driver, profile;
    bool have_memory = false, mem_warning = false;
    double budget_gib = 0, gpu_gib = 0, gpu_limit_gib = 0;
    int governor_level = 0;
    std::string mem_note;
};

class Sampler {
public:
    explicit Sampler(double window_s = 1.0) : window_s_(window_s) {}
    Metrics update(const Raw& now);
    void reset();

private:
    double window_s_;
    std::deque<Raw> history_;
    double smooth_ = 0;
    bool smooth_init_ = false;
    double last_t_ = 0;
};

enum class Colour { normal, amber, red };

struct Span {
    std::size_t begin, length;
    Colour colour;
};

struct Line {
    std::string text;
    std::vector<Span> spans;
};

struct FormatOptions {
    gta6rt_hud_mode mode = GTA6RT_HUD_MIN;
    double budget_ms = 33.4;
};

std::vector<Line> format_lines(const Metrics& m, const FormatOptions& opt);
std::string format_title(const std::string& base, const Metrics& m, const FormatOptions& opt);

}  // namespace gta6rt::hud
