#include "hud_model.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace gta6rt::hud {

namespace {

constexpr double kGiB = 1073741824.0;

double delta(std::uint64_t a, std::uint64_t b) { return b >= a ? double(b - a) : 0.0; }

std::string fmt(const char* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

std::string count_text(std::uint64_t n) {
    char buf[32];
    if (n >= 10000000) std::snprintf(buf, sizeof buf, "%.0fM", double(n) / 1e6);
    else if (n >= 1000000) std::snprintf(buf, sizeof buf, "%.1fM", double(n) / 1e6);
    else if (n >= 10000) std::snprintf(buf, sizeof buf, "%.0fk", double(n) / 1e3);
    else if (n >= 1000) std::snprintf(buf, sizeof buf, "%.1fk", double(n) / 1e3);
    else std::snprintf(buf, sizeof buf, "%llu", static_cast<unsigned long long>(n));
    return buf;
}

std::string rate_text(double v) {
    char buf[32];
    if (v >= 1e6) std::snprintf(buf, sizeof buf, "%.1fM", v / 1e6);
    else if (v >= 1e4) std::snprintf(buf, sizeof buf, "%.0fk", v / 1e3);
    else if (v >= 1e3) std::snprintf(buf, sizeof buf, "%.1fk", v / 1e3);
    else std::snprintf(buf, sizeof buf, "%.0f", v);
    return buf;
}

std::string per_frame(double v) {
    char buf[32];
    if (v >= 100) std::snprintf(buf, sizeof buf, "%.0f", v);
    else if (v >= 10) std::snprintf(buf, sizeof buf, "%.0f", v);
    else std::snprintf(buf, sizeof buf, "%.1f", v);
    return buf;
}

std::string gib_text(double g) {
    char buf[32];
    if (g >= 10) std::snprintf(buf, sizeof buf, "%.0f", g);
    else std::snprintf(buf, sizeof buf, "%.1f", g);
    return buf;
}

std::string ms_text(double ms) {
    char buf[32];
    if (ms >= 100) std::snprintf(buf, sizeof buf, "%.0fms", ms);
    else std::snprintf(buf, sizeof buf, "%.1fms", ms);
    return buf;
}

struct Builder {
    Line line;
    void add(const std::string& s, Colour c = Colour::normal) {
        if (c != Colour::normal) line.spans.push_back({line.text.size(), s.size(), c});
        line.text += s;
    }
    void sep() { line.text += " | "; }
};

Colour frame_colour(const Metrics& m, double budget) {
    if (!m.have_frames) return Colour::normal;
    if (m.frame_avg_ms > budget * 1.5) return Colour::red;
    if (m.frame_avg_ms > budget || m.frame_max_ms > budget * 2.0) return Colour::amber;
    return Colour::normal;
}

Colour memory_colour(const Metrics& m) {
    if (m.mem_warning || m.pressure >= 2) return Colour::red;
    if (m.pressure == 1) return Colour::amber;
    const double cap = m.have_memory && m.budget_gib > 0 ? m.budget_gib : m.ram_gib;
    if (cap > 0 && m.footprint_gib > 0.95 * cap) return Colour::red;
    if (cap > 0 && m.footprint_gib > 0.85 * cap) return Colour::amber;
    if (m.have_memory && m.gpu_limit_gib > 0 && m.gpu_gib > 0.9 * m.gpu_limit_gib) return Colour::amber;
    return Colour::normal;
}

std::string memory_text(const Metrics& m) {
    if (!m.have_memory || m.budget_gib <= 0) return "mem " + gib_text(m.footprint_gib) + "/" + gib_text(m.ram_gib) + "G";
    std::string s = "mem " + gib_text(m.footprint_gib) + "/" + gib_text(m.budget_gib) + "G gpu " + gib_text(m.gpu_gib) + "G";
    if (m.mem_warning) s += " LOW";
    return s;
}

Line first_line(const Metrics& m, const FormatOptions& opt) {
    Builder b;
    if (m.have_frames) {
        b.add(fmt("%.0f", m.fps) + " fps ");
        b.add(ms_text(m.frame_avg_ms) + " (max " + fmt("%.0f", m.frame_max_ms) + ")", frame_colour(m, opt.budget_ms));
    } else if (m.stall_s >= 0.5) {
        b.add("stalled " + fmt("%.1f", m.stall_s) + "s", m.stall_s > 2.0 ? Colour::red : Colour::amber);
    } else {
        b.add("idle");
    }
    b.sep();
    b.add(m.have_frames || m.draws_pf > 0 ? "draws " + per_frame(m.draws_pf) : std::string("draws -"));
    b.sep();
    b.add(m.have_gpu ? "gpu " + ms_text(m.gpu_ms_pf) : std::string("gpu -"));
    b.sep();
    if (m.have_pipelines)
        b.add("pso " + count_text(m.pipelines_built) + "/" + count_text(m.pipelines_pending) + " pending");
    else
        b.add("pso -");
    b.sep();
    b.add(memory_text(m), memory_colour(m));
    return b.line;
}

}  // namespace

void Sampler::reset() {
    history_.clear();
    smooth_ = 0;
    smooth_init_ = false;
    last_t_ = 0;
}

Metrics Sampler::update(const Raw& now) {
    Metrics m;
    history_.push_back(now);
    while (history_.size() > 2 && now.t_s - history_[1].t_s >= window_s_) history_.pop_front();
    while (history_.size() > 64) history_.pop_front();
    const Raw& old = history_.front();
    const double dt = now.t_s - old.t_s;

    double max_ns = 0;
    for (const Raw& r : history_) max_ns = std::max(max_ns, double(r.frame_ns_max));

    if (dt > 0 && history_.size() >= 2) {
        const double frames = delta(old.presents, now.presents);
        m.fps = frames / dt;
        m.have_frames = frames > 0;
        const double intervals = delta(old.frame_intervals, now.frame_intervals);
        if (intervals > 0) m.frame_avg_ms = delta(old.frame_ns_sum, now.frame_ns_sum) / intervals / 1e6;
        else if (frames > 0) m.frame_avg_ms = 1000.0 / m.fps;
        m.frame_max_ms = max_ns / 1e6;
        if (frames > 0) {
            m.draws_pf = delta(old.draws, now.draws) / frames;
            m.dispatches_pf = delta(old.dispatches, now.dispatches) / frames;
            m.submissions_pf = delta(old.submissions, now.submissions) / frames;
            m.exec_ms_pf = delta(old.exec_cpu_ns, now.exec_cpu_ns) / frames / 1e6;
            m.gpu_ms_pf = delta(old.gpu_ns, now.gpu_ns) / frames / 1e6;
        }
        const double lat_n = delta(old.latency_samples, now.latency_samples);
        if (lat_n > 0) m.latency_ms = delta(old.latency_ns_sum, now.latency_ns_sum) / lat_n / 1e6;
        if (now.imports_valid && old.imports_valid) m.imports_per_s = delta(old.imports, now.imports) / dt;
        const double step = now.t_s - last_t_;
        if (!smooth_init_) {
            smooth_ = m.fps;
            smooth_init_ = true;
        } else if (step > 0) {
            const double a = 1.0 - std::exp(-step / 3.0);
            smooth_ += a * (m.fps - smooth_);
        }
        m.fps_smooth = smooth_;
    }
    last_t_ = now.t_s;

    const std::uint64_t have = now.have;
    m.have_submissions = have & GTA6RT_HUD_HAVE_SUBMISSIONS;
    m.have_exec = have & GTA6RT_HUD_HAVE_EXEC_CPU;
    m.have_dispatches = have & GTA6RT_HUD_HAVE_DISPATCHES;
    if (!m.have_frames && now.presents > 0 && now.since_present_s > 0) m.stall_s = now.since_present_s;
    m.have_gpu = (have & GTA6RT_HUD_HAVE_GPU) && delta(old.gpu_samples, now.gpu_samples) > 0;
    m.have_latency = (have & GTA6RT_HUD_HAVE_LATENCY) && m.latency_ms > 0;
    m.have_pipelines = have & GTA6RT_HUD_HAVE_PIPELINES;
    m.have_textures = have & GTA6RT_HUD_HAVE_TEXTURES;
    m.have_imports = now.imports_valid && history_.size() >= 2;
    m.pipelines_built = now.pipelines_built;
    m.pipelines_pending = now.pipelines_pending;
    m.cache_hits = now.cache_hits;
    m.pso_failures = now.pso_failures;
    m.textures_waiting = now.textures_waiting;
    m.footprint_gib = double(now.footprint) / kGiB;
    m.compressed_gib = double(now.compressed) / kGiB;
    m.resident_gib = double(now.resident) / kGiB;
    m.ram_gib = double(now.ram) / kGiB;
    m.pressure = now.pressure;
    m.have_memory = now.have_memory;
    m.budget_gib = double(now.budget) / kGiB;
    m.gpu_gib = double(now.gpu_bytes) / kGiB;
    m.gpu_limit_gib = double(now.gpu_limit) / kGiB;
    m.governor_level = static_cast<int>(now.governor_level);
    m.mem_warning = now.mem_warning != 0;
    m.mem_note = now.mem_note;
    m.driver = now.driver;
    m.profile = now.profile;
    m.state = now.state;
    if (m.state.empty() && m.have_pipelines && m.pipelines_pending > 0)
        m.state = "compiling " + std::to_string(m.pipelines_pending) + " pipelines";
    return m;
}

std::vector<Line> format_lines(const Metrics& m, const FormatOptions& opt) {
    std::vector<Line> out;
    out.push_back(first_line(m, opt));
    if (opt.mode != GTA6RT_HUD_FULL) return out;

    Builder b2;
    b2.add("avg " + fmt("%.1f", m.fps_smooth));
    b2.sep();
    b2.add(m.have_submissions ? "sub " + per_frame(m.submissions_pf) + "/f" : std::string("sub -"));
    b2.sep();
    b2.add(m.have_dispatches ? "disp " + per_frame(m.dispatches_pf) + "/f" : std::string("disp -"));
    b2.sep();
    b2.add(m.have_exec ? "exec " + ms_text(m.exec_ms_pf) + "/f" : std::string("exec -"));
    b2.sep();
    b2.add(m.have_latency ? "lat " + ms_text(m.latency_ms) : std::string("lat -"));
    b2.sep();
    b2.add(m.have_imports ? "imp " + rate_text(m.imports_per_s) + "/s" : std::string("imp -"));
    b2.sep();
    b2.add(m.have_textures ? "tex " + count_text(m.textures_waiting) : std::string("tex -"));
    out.push_back(b2.line);

    Builder b3;
    if (m.have_pipelines) b3.add("cache " + count_text(m.cache_hits) + " psofail " + count_text(m.pso_failures));
    else b3.add("cache - psofail -");
    b3.sep();
    std::string mem3 = "rss " + gib_text(m.resident_gib) + "G cmp " + gib_text(m.compressed_gib) + "G";
    if (m.have_memory) {
        mem3 += " gov " + std::to_string(m.governor_level);
        if (m.gpu_limit_gib > 0) mem3 += " gpu/ws " + gib_text(m.gpu_gib) + "/" + gib_text(m.gpu_limit_gib) + "G";
        if (!m.mem_note.empty()) mem3 += " " + m.mem_note;
    }
    b3.add(mem3, memory_colour(m));
    b3.sep();
    std::string who = m.driver.empty() ? "-" : m.driver;
    if (!m.profile.empty()) who += " " + m.profile;
    b3.add(who);
    if (!m.state.empty()) {
        b3.sep();
        b3.add(m.state);
    }
    out.push_back(b3.line);
    return out;
}

std::string format_title(const std::string& base, const Metrics& m, const FormatOptions& opt) {
    FormatOptions one = opt;
    one.mode = GTA6RT_HUD_MIN;
    const std::string line = first_line(m, one).text;
    return base.empty() ? line : base + " - " + line;
}

}  // namespace gta6rt::hud
