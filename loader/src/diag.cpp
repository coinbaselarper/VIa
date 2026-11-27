#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "loader_internal.h"

namespace {

std::mutex g_prov_mutex;
gta6rt_stack_provider g_provider = nullptr;
void* g_provider_user = nullptr;

std::mutex g_sampler_mutex;
std::condition_variable g_sampler_cv;
std::thread g_sampler;
bool g_sampler_stop = false;

void print_traces(uint32_t at, const gta6rt_thread_stack_sample* s, uint32_t n) {
    std::string out = "[loader] stacks t=" + std::to_string(at) + ": " + std::to_string(n) + " threads\n";
    std::vector<std::vector<uint64_t>> seen(n);
    for (uint32_t i = 0; i < n; ++i) {
        out += "[loader] stacks t=" + std::to_string(at) + " thread " + std::to_string(s[i].thread_id) + " (" +
               s[i].label + "):\n";
        uint64_t pcs[64];
        const uint32_t cnt = gta6rt_seh_backtrace(&s[i].ctx, pcs, 64);
        seen[i].assign(pcs, pcs + cnt);
        uint32_t same = n;
        for (uint32_t j = 0; j < i && same == n; ++j)
            if (cnt && seen[j] == seen[i]) same = j;
        if (same != n) {
            out += "   same as thread " + std::to_string(s[same].thread_id) + "\n";
        } else if (!cnt) {
            out += "   stack unavailable\n";
        } else {
            for (uint32_t k = 0; k < cnt; ++k) {
                char d[160];
                gta6rt_loader_describe_address(pcs[k], d, sizeof d);
                out += "   #" + std::to_string(k) + " " + d + "\n";
            }
        }
    }
    std::fputs(out.c_str(), stderr);
    std::fflush(stderr);
}

}  // namespace

extern "C" {

size_t gta6rt_loader_format_backtrace(const gta6rt_x64_context* ctx, char* buf, size_t buf_size, uint32_t max_frames) {
    if (!buf || !buf_size) return 0;
    buf[0] = 0;
    if (!ctx) return 0;
    uint64_t pcs[128];
    if (max_frames > 128) max_frames = 128;
    const uint32_t n = gta6rt_seh_backtrace(ctx, pcs, max_frames);
    size_t used = 0;
    for (uint32_t i = 0; i < n && used + 1 < buf_size; ++i) {
        char d[160];
        gta6rt_loader_describe_address(pcs[i], d, sizeof d);
        const int w = std::snprintf(buf + used, buf_size - used, "#%u %s\n", i, d);
        if (w < 0) break;
        used += std::min<size_t>(static_cast<size_t>(w), buf_size - used - 1);
    }
    return used;
}

void gta6rt_loader_set_stack_provider(gta6rt_stack_provider fn, void* user) {
    std::lock_guard l(g_prov_mutex);
    g_provider = fn;
    g_provider_user = user;
}

int gta6rt_loader_parse_stack_times(const char* text, uint32_t* out, uint32_t max, uint32_t* out_count) {
    if (out_count) *out_count = 0;
    if (!text || !*text) return -1;
    std::vector<uint32_t> v;
    const char* p = text;
    for (;;) {
        char* end = nullptr;
        if (*p < '0' || *p > '9') return -1;
        const unsigned long long x = std::strtoull(p, &end, 10);
        if (x > 0xFFFFFFFFull) return -1;
        v.push_back(static_cast<uint32_t>(x));
        if (*end == 0) break;
        if (*end != ',') return -1;
        p = end + 1;
    }
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    if (v.size() > max) return -1;
    for (size_t i = 0; i < v.size(); ++i) out[i] = v[i];
    if (out_count) *out_count = static_cast<uint32_t>(v.size());
    return 0;
}

gta6rt_status gta6rt_loader_start_stack_sampler(void) {
    const char* spec = std::getenv("GTA6RT_STACKS");
    if (!spec || !*spec) return GTA6RT_OK;
    uint32_t times[256], n = 0;
    if (gta6rt_loader_parse_stack_times(spec, times, 256, &n) != 0) {
        ldr::log_error("GTA6RT_STACKS: expected comma-separated whole seconds");
        return GTA6RT_E_INVALID_ARG;
    }
    std::lock_guard l(g_sampler_mutex);
    if (g_sampler.joinable()) return GTA6RT_E_ALREADY_EXISTS;
    g_sampler_stop = false;
    const auto start = std::chrono::steady_clock::now();
    g_sampler = std::thread([start, v = std::vector<uint32_t>(times, times + n)] {
        for (uint32_t at : v) {
            std::unique_lock lk(g_sampler_mutex);
            if (g_sampler_cv.wait_until(lk, start + std::chrono::seconds(at), [] { return g_sampler_stop; })) return;
            lk.unlock();
            gta6rt_stack_provider fn;
            void* user;
            {
                std::lock_guard pl(g_prov_mutex);
                fn = g_provider;
                user = g_provider_user;
            }
            if (!fn) {
                std::fprintf(stderr, "[loader] stacks t=%u: no thread-context provider is set, cannot sample\n", at);
                continue;
            }
            std::vector<gta6rt_thread_stack_sample> s(512);
            const uint32_t cnt = fn(user, s.data(), static_cast<uint32_t>(s.size()));
            print_traces(at, s.data(), std::min<uint32_t>(cnt, static_cast<uint32_t>(s.size())));
        }
    });
    return GTA6RT_OK;
}

void gta6rt_loader_stop_stack_sampler(void) {
    {
        std::lock_guard l(g_sampler_mutex);
        g_sampler_stop = true;
    }
    g_sampler_cv.notify_all();
    if (g_sampler.joinable()) g_sampler.join();
}

}  // extern "C"
