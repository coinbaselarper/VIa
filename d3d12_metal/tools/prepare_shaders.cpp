// prepare_shaders: builds the Metal path's pipeline archive (MTLBinaryArchive) without running the title, from
// pcache manifests (what the title used, port/d3d12_metal/cache) and spec lists (cache/tools/boot_set.py: the boot
// PSO set from the package's pipeline-library records and the PSO census logs). Same archive key and file as the
// title's (same backend, translator version, GPU, OS build, process arch), so build it for the arch the title runs
// as (x86_64 under Rosetta). Resumable: a rerun finds what the earlier shards hold as archive hits.
// Shards: one per phase at least (tag = phase name), and a new one before a shard would pass --shard-mb (150).
// While a gta6rt runs, at most --title-jobs compiles start (default 0: it waits; STATUS-cache.md has the measurement).
//
//   prepare_shaders [--manifest F]... [--specs F]... [--archive-dir D] [--catalog M] [--phases a,b] [--limit N]
//                   [--sample N --seed S] [--jobs J<=3] [--flush-every S] [--report F] [--fresh] [--hold]
//                   [--shard-mb M] [--title-jobs N] [--order manifest|file] [--dry-run]

#include "pcache.h"

#include <gta6rt_metal/metal_backend.h>
#include <gta6rt_shaderc_msl/catalog.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <map>
#include <mach/mach.h>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace gpu = gta6rt::gpu;
namespace pc = gta6rt::pcache;
namespace sc = gta6rt::shaderc_msl;

namespace {

double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::uint64_t footprint() {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) return 0;
    return info.phys_footprint;
}

// MTLCompilerService processes (pid -> RSS KiB). The ones not there before this tool opened its device are its own.
std::map<int, std::uint64_t> compiler_processes() {
    std::map<int, std::uint64_t> out;
    FILE* p = ::popen("/bin/ps -axo pid=,rss=,comm=", "r");
    if (!p) return out;
    char line[1024];
    while (std::fgets(line, sizeof line, p)) {
        int pid = 0;
        unsigned long long rss = 0;
        char comm[900] = {};
        if (std::sscanf(line, "%d %llu %899[^\n]", &pid, &rss, comm) == 3 && std::strstr(comm, "MTLCompilerService"))
            out[pid] = rss;
    }
    ::pclose(p);
    return out;
}

std::uint64_t file_size(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 ? std::uint64_t(st.st_size) : 0;
}

struct Item {
    std::string key, text;
    pc::Phase phase = pc::Phase::boot;
    double t = 0;
    pc::Spec spec;
};

struct Lib {
    std::unique_ptr<gpu::ShaderLibrary> library;
    std::string entry;
};

std::string fmt_s(double s) {
    char b[32];
    if (s < 120) std::snprintf(b, sizeof b, "%.0f s", s);
    else if (s < 7200) std::snprintf(b, sizeof b, "%.1f min", s / 60);
    else std::snprintf(b, sizeof b, "%.2f h", s / 3600);
    return b;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string port = GTA6RT_PORT_ROOT;
#if defined(__x86_64__)
    const std::string arch = "x86_64";
#else
    const std::string arch = "arm64";
#endif
    std::vector<std::string> manifests, specs;
    std::string archive_dir = port + "/build/d3d12_metal-" + arch + "/pipeline-archive";
    std::string catalog_path = port + "/build/shaderc_msl/runs/main/manifest.tsv";
    std::string report_path;
    std::set<pc::Phase> phases;
    std::size_t limit = 0, sample = 0;
    unsigned seed = 1, jobs = 3;
    double flush_every = 120;
    bool fresh = false, hold = false, dry = false;
    double shard_mb = 150;
    unsigned title_jobs = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--manifest") manifests.push_back(next());
        else if (a == "--specs") specs.push_back(next());
        else if (a == "--archive-dir") archive_dir = next();
        else if (a == "--catalog") catalog_path = next();
        else if (a == "--report") report_path = next();
        else if (a == "--limit") limit = std::size_t(std::atol(next().c_str()));
        else if (a == "--sample") sample = std::size_t(std::atol(next().c_str()));
        else if (a == "--seed") seed = unsigned(std::atoi(next().c_str()));
        else if (a == "--jobs") jobs = unsigned(std::atoi(next().c_str()));
        else if (a == "--flush-every") flush_every = std::atof(next().c_str());
        else if (a == "--fresh") fresh = true;
        else if (a == "--hold") hold = true;
        else if (a == "--dry-run") dry = true;
        else if (a == "--shard-mb") shard_mb = std::atof(next().c_str());
        else if (a == "--title-jobs") title_jobs = unsigned(std::atoi(next().c_str()));
        else if (a == "--phases") {
            std::stringstream ss(next());
            for (std::string p; std::getline(ss, p, ',');) {
                const auto ph = pc::phase_from_name(p);
                if (!ph) {
                    std::fprintf(stderr, "unknown phase %s\n", p.c_str());
                    return 2;
                }
                phases.insert(*ph);
            }
        } else {
            std::fprintf(stderr, "unknown argument %s (see the header of prepare_shaders.cpp)\n", a.c_str());
            return 2;
        }
    }
    if (jobs < 1 || jobs > 3) {
        std::fprintf(stderr, "--jobs must be 1..3 (at most 3 Metal compiler processes on this machine)\n");
        return 2;
    }
    if (manifests.empty() && specs.empty()) manifests.push_back(archive_dir + "/pcache-manifest.txt");

    std::vector<Item> items;
    std::set<std::string> seen;
    std::size_t undecodable = 0;
    const auto add = [&](const std::string& key, const std::string& text, pc::Phase phase, double t) {
        if (!phases.empty() && !phases.count(phase)) return;
        if (!seen.insert(key).second) return;
        Item it;
        it.key = key;
        it.text = text;
        it.phase = phase;
        it.t = t;
        std::string why;
        if (!pc::decode(text, it.spec, &why)) {
            ++undecodable;
            if (undecodable <= 3) std::fprintf(stderr, "undecodable pipeline (%s): %.120s\n", why.c_str(), text.c_str());
            return;
        }
        items.push_back(std::move(it));
    };
    for (const auto& m : manifests) {
        pc::Manifest man;
        std::string error;
        if (!man.load(m, &error)) {
            std::fprintf(stderr, "manifest %s: %s\n", m.c_str(), error.c_str());
            return 1;
        }
        for (const auto& e : man.ordered()) add(e.key, e.text, e.phase, e.t);
        std::printf("manifest %s: %zu pipelines\n", m.c_str(), man.size());
    }
    for (const auto& path : specs) {
        std::ifstream in(path);
        if (!in) {
            std::fprintf(stderr, "cannot read %s\n", path.c_str());
            return 1;
        }
        std::size_t n = 0;
        for (std::string line; std::getline(in, line);) {
            if (line.empty() || line[0] == '#') continue;
            const auto tab = line.find('\t');
            if (tab == std::string::npos) continue;
            const auto phase = pc::phase_from_name(line.substr(0, tab));
            if (!phase) continue;
            const std::string text = line.substr(tab + 1);
            add(pc::key_of(text), text, *phase, double(n++));
        }
        std::printf("specs %s: %zu lines\n", path.c_str(), n);
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return a.phase != b.phase ? a.phase < b.phase : a.t < b.t;
    });
    if (sample && sample < items.size()) {
        std::mt19937 rng(seed);
        std::shuffle(items.begin(), items.end(), rng);
        items.resize(sample);
    }
    if (limit && limit < items.size()) items.resize(limit);

    sc::Catalog catalog;
    std::string error;
    if (!catalog.load(catalog_path, &error)) {
        std::fprintf(stderr, "catalog %s: %s\n", catalog_path.c_str(), error.c_str());
        return 1;
    }

    gta6rt::metal::DeviceOptions options;
    options.compile_threads = jobs;
    const auto compilers_before = compiler_processes();
    auto device = gta6rt::metal::create_device(options);
    if (!device) {
        std::fprintf(stderr, "no Metal device\n");
        return 1;
    }
    const std::uint64_t fp0 = footprint();
    std::string open_log;
    if (shard_mb > 0) ::setenv("GTA6RT_METAL_PCACHE_SHARD_MB", std::to_string(shard_mb).c_str(), 1);
    if (pc::open_archive(*device, archive_dir, pc::archive_version(port, catalog_path), !fresh, &open_log) !=
        gpu::Status::ok) {
        std::fprintf(stderr, "archive: %s\n", open_log.c_str());
        return 1;
    }
    std::printf("%s\n", open_log.c_str());
    const auto st0 = device->pipeline_cache_stats();
    const std::uint64_t fp_loaded = footprint();
    std::printf("archive %s-*: %s, %u shards, %llu bytes, %.3f s to open; footprint %.1f -> %.1f MiB\nkey: %s\n",
                st0.path.c_str(), st0.loaded ? "loaded" : "new", st0.shards, (unsigned long long)st0.file_bytes,
                st0.load_seconds, fp0 / 1048576.0, fp_loaded / 1048576.0, st0.key.c_str());

    std::printf("%zu pipelines to build (%zu undecodable), %u compile threads; a pipeline the loaded archive holds is "
                "a hit (about 1 ms), so a rerun resumes where the last flushed archive ended\n",
                items.size(), undecodable, jobs);
    if (dry) return 0;

    gpu::RootLayoutDesc rd;
    rd.push_bytes = 144;
    rd.user_dwords = 32;
    rd.split_sets = true;
    for (std::uint32_t b = 0; b < 29; ++b) {
        gpu::RootTable t;
        t.binding = b;
        t.kind = b == 1 ? gpu::DescriptorKind::sampler : b == 2 ? gpu::DescriptorKind::buffer : gpu::DescriptorKind::texture;
        rd.tables.push_back(t);
    }
    auto root = device->create_root_layout(rd);

    std::map<std::string, Lib> libs;
    double lib_seconds = 0;
    const auto lib_of = [&](const std::string& name, std::string& why) -> const Lib* {
        const sc::ShaderEntry* e = catalog.find(name);
        if (!e) {
            why = "not in the shader store";
            return nullptr;
        }
        auto it = libs.find(e->key);
        if (it != libs.end()) return &it->second;
        const double t0 = now_s();
        std::ifstream in(catalog.library_path(*e), std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (bytes.empty()) {
            why = "metallib missing: " + catalog.library_path(*e);
            return nullptr;
        }
        Lib lib;
        lib.library = device->create_library_metallib(bytes, &why);
        if (!lib.library) return nullptr;
        const auto names = lib.library->function_names();
        lib.entry = names.empty() ? "main0" : names.front();
        for (const auto& n : names)
            if (n == "main0") lib.entry = n;
        lib_seconds += now_s() - t0;
        return &(libs[e->key] = std::move(lib));
    };

    FILE* report = report_path.empty() ? nullptr : std::fopen(report_path.c_str(), "w");
    if (report) std::fprintf(report, "key\tphase\tstatus\tseconds\tfrom_archive\tlabel\terror\tbuild_seconds\n");

    struct Flight {
        const Item* item;
        std::unique_ptr<gpu::Pipeline> pipeline;
        double t0;
    };
    std::deque<Flight> flight;
    std::vector<std::unique_ptr<gpu::Pipeline>> kept;
    std::map<std::string, std::size_t> failures;
    std::size_t ok = 0, failed = 0, missing = 0, hits = 0;
    double busy = 0;
    const double start = now_s();
    double last_flush = start, last_progress = start;
    unsigned max_compilers = 0;
    std::uint64_t max_compiler_rss = 0;
    std::uint64_t max_fp = 0;
    const std::size_t window = std::size_t(jobs) * 2;

    std::size_t flush_failures = 0;
    const auto flush = [&](const char* why) {
        const double t0 = now_s();
        std::string e;
        const auto before = device->pipeline_cache_stats();
        if (!before.pending) {
            last_flush = now_s();
            return;
        }
        if (device->flush_pipeline_cache(&e) != gpu::Status::ok) {
            ++flush_failures;
            std::fprintf(stderr, "flush (%s) failed: %llu pipelines of this shard lost, the shards on disk are unchanged: "
                                 "%s\n", why, (unsigned long long)before.pending, e.c_str());
            last_flush = now_s();
            return;
        }
        const auto s = device->pipeline_cache_stats();
        std::printf("  flush (%s): shard %u, %llu pipelines, %llu bytes in %.2f s (compiles went on); %llu bytes on disk\n",
                    why, s.shards, (unsigned long long)(s.written - before.written),
                    (unsigned long long)(s.file_bytes - before.file_bytes), now_s() - t0, (unsigned long long)s.file_bytes);
        std::fflush(stdout);
        last_flush = now_s();
    };
    const auto title_running = [] {
        FILE* p = ::popen("/bin/ps -axo comm=", "r");
        if (!p) return false;
        char line[1024];
        bool found = false;
        while (std::fgets(line, sizeof line, p)) {
            std::string n = line;
            while (!n.empty() && (n.back() == '\n' || n.back() == ' ')) n.pop_back();
            const auto slash = n.rfind('/');
            if (slash != std::string::npos) n = n.substr(slash + 1);
            if (n == "gta6rt" || n.rfind("gta6rt_", 0) == 0) found = true;
        }
        ::pclose(p);
        return found;
    };
    double title_checked = 0, title_wait = 0;
    bool title_up = false;
    pc::Phase shard_phase = items.empty() ? pc::Phase::boot : items.front().phase;
    gta6rt::metal::set_pipeline_cache_tag(device.get(), pc::phase_name(shard_phase));
    const auto finish = [&](Flight& f) {
        gpu::Pipeline* p = f.pipeline.get();
        const double dt = now_s() - f.t0;
        busy += dt;
        const bool good = p->state() == gpu::PipelineState::ready;
        if (good) {
            ++ok;
            if (p->from_archive()) ++hits;
        } else {
            ++failed;
            ++failures[p->error().substr(0, 90)];
        }
        if (report)
            std::fprintf(report, "%s\t%s\t%s\t%.4f\t%d\t%s\t%s\t%.4f\n", f.item->key.c_str(), pc::phase_name(f.item->phase),
                         good ? "ok" : "failed", dt, good && p->from_archive() ? 1 : 0,
                         (f.item->spec.names[0] + (f.item->spec.names[1].empty() ? "" : "," + f.item->spec.names[1])).c_str(),
                         good ? "" : p->error().substr(0, 200).c_str(), p->compile_seconds());
        if (hold) kept.push_back(std::move(f.pipeline));
    };
    const auto progress = [&](bool final) {
        const double el = now_s() - start;
        const std::size_t n = ok + failed + missing;
        const double rate = el > 0 ? n / el : 0;
        const double eta = rate > 0 ? (items.size() - n) / rate : 0;
        const std::uint64_t fp = footprint();
        max_fp = std::max(max_fp, fp);
        unsigned cp = 0;
        std::uint64_t rss = 0;
        for (const auto& [pid, kib] : compiler_processes())
            if (!compilers_before.count(pid)) {
                ++cp;
                rss += kib;
            }
        max_compilers = std::max(max_compilers, cp);
        max_compiler_rss = std::max(max_compiler_rss, rss);
        double load[3] = {};
        getloadavg(load, 3);
        std::printf("%s %zu/%zu (%.1f%%) in %s: %.1f/s, ETA %s; ok %zu (archive %zu) failed %zu missing %zu; footprint "
                    "%.0f MiB; own MTLCompilerService processes %u (%.0f MiB RSS); load %.1f\n",
                    final ? "done" : "progress", n, items.size(), items.empty() ? 100.0 : 100.0 * n / items.size(),
                    fmt_s(el).c_str(), rate, fmt_s(eta).c_str(), ok, hits, failed, missing, fp / 1048576.0, cp, rss / 1024.0, load[0]);
        std::fflush(stdout);
        last_progress = now_s();
    };

    const auto drain = [&] {
        while (!flight.empty()) {
            for (auto it = flight.begin(); it != flight.end();) {
                const auto s = it->pipeline->state();
                if (s == gpu::PipelineState::ready || s == gpu::PipelineState::failed) {
                    finish(*it);
                    it = flight.erase(it);
                } else {
                    ++it;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    for (const Item& item : items) {
        if (item.phase != shard_phase) {
            drain();
            flush("phase");
            shard_phase = item.phase;
            gta6rt::metal::set_pipeline_cache_tag(device.get(), pc::phase_name(shard_phase));
        }
        for (;;) {
            if (now_s() - title_checked > 5) {
                title_checked = now_s();
                const bool up = title_running();
                if (up != title_up) {
                    std::printf("  a gta6rt is %s: %u compile%s may start\n", up ? "running" : "gone",
                                up ? title_jobs : jobs, (up ? title_jobs : jobs) == 1 ? "" : "s");
                    std::fflush(stdout);
                }
                title_up = up;
            }
            if (!title_up || flight.size() < title_jobs) break;
            const double t0 = now_s();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            for (auto it = flight.begin(); it != flight.end();) {
                const auto s = it->pipeline->state();
                if (s == gpu::PipelineState::ready || s == gpu::PipelineState::failed) {
                    finish(*it);
                    it = flight.erase(it);
                } else {
                    ++it;
                }
            }
            title_wait += now_s() - t0;
        }
        if (shard_mb > 0 && device->pipeline_cache_stats().pending_bytes >= std::uint64_t(shard_mb * 1e6 * 0.85)) {
            drain();
            flush("shard size");
        }
        while (flight.size() >= window) {
            bool any = false;
            for (auto it = flight.begin(); it != flight.end();) {
                const auto s = it->pipeline->state();
                if (s == gpu::PipelineState::ready || s == gpu::PipelineState::failed) {
                    finish(*it);
                    it = flight.erase(it);
                    any = true;
                } else {
                    ++it;
                }
            }
            if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (now_s() - last_progress > 10) progress(false);
        if (now_s() - last_flush > flush_every) flush("periodic");

        gpu::ShaderRef refs[2];
        std::string why;
        bool have = true;
        for (int i = 0; i < 2 && have; ++i) {
            if (item.spec.names[i].empty()) continue;
            const Lib* lib = lib_of(item.spec.names[i], why);
            if (!lib) {
                have = false;
                why = item.spec.names[i] + ": " + why;
                break;
            }
            refs[i] = {lib->library.get(), lib->entry};
        }
        if (!have) {
            ++missing;
            ++failures["no library: " + why.substr(why.find(": ") + 2, 60)];
            if (report) std::fprintf(report, "%s\t%s\tmissing\t0\t0\t%s\t%s\n", item.key.c_str(), pc::phase_name(item.phase),
                                     item.spec.names[0].c_str(), why.c_str());
            continue;
        }
        Flight f;
        f.item = &item;
        f.t0 = now_s();
        switch (item.spec.kind) {
        case pc::Kind::render: {
            gpu::RenderPipelineDesc d = item.spec.render;
            d.vs = refs[0];
            d.ps = refs[1];
            d.root = root.get();
            d.mode = gpu::CompileMode::async;
            d.priority = gpu::Priority::background;
            d.not_ready = gpu::NotReady::skip;
            f.pipeline = device->create_render_pipeline(d);
            break;
        }
        case pc::Kind::mesh: {
            gpu::MeshPipelineDesc d = item.spec.mesh;
            d.mesh = refs[0];
            d.ps = refs[1];
            d.root = root.get();
            d.mode = gpu::CompileMode::async;
            d.priority = gpu::Priority::background;
            d.not_ready = gpu::NotReady::skip;
            f.pipeline = device->create_mesh_pipeline(d);
            break;
        }
        case pc::Kind::compute: {
            gpu::ComputePipelineDesc d = item.spec.compute;
            d.cs = refs[0];
            d.root = root.get();
            d.mode = gpu::CompileMode::async;
            d.priority = gpu::Priority::background;
            d.not_ready = gpu::NotReady::skip;
            f.pipeline = device->create_compute_pipeline(d);
            break;
        }
        }
        if (!f.pipeline) {
            ++failed;
            ++failures["backend refused the description"];
            continue;
        }
        flight.push_back(std::move(f));
    }
    drain();
    const double build_s = now_s() - start;
    const std::uint64_t fp_built = footprint();
    flush("end");
    progress(true);
    const auto st1 = device->pipeline_cache_stats();
    std::printf("built %zu pipelines in %s wall (%.1f s summed in flight, %.1f s loading %zu metallibs): %zu ok (%zu "
                "already in the archive), %zu failed, %zu without a library\n",
                ok + failed, fmt_s(build_s).c_str(), busy, lib_seconds, libs.size(), ok, hits, failed, missing);
    std::printf("archive %s-*: %u shards, %llu bytes (%.1f MiB) on disk, %llu added this run (%llu written), longest "
                "shard write %.2f s, %zu write failures; %.1f KiB per added pipeline; build time: lookups %.1f s, "
                "compiles %.1f s, archive adds %.1f s; waited %.0f s for a running gta6rt\n",
                st1.path.c_str(), st1.shards, (unsigned long long)st1.file_bytes, st1.file_bytes / 1048576.0,
                (unsigned long long)st1.added, (unsigned long long)st1.written, st1.serialize_longest, flush_failures,
                st1.written ? (double(st1.file_bytes) - double(st0.file_bytes)) / 1024.0 / double(st1.written) : 0.0,
                st1.lookup_seconds, st1.compile_seconds, st1.add_seconds, title_wait);
    std::printf("footprint: start %.0f MiB, archive open %.0f MiB, after builds %.0f MiB (peak sampled %.0f MiB), %s; "
                "own MTLCompilerService processes at most %u, their RSS at most %.0f MiB\n",
                fp0 / 1048576.0, fp_loaded / 1048576.0, fp_built / 1048576.0, std::max(max_fp, fp_built) / 1048576.0,
                hold ? "pipelines held" : "pipelines released after build", max_compilers, max_compiler_rss / 1024.0);
    for (const auto& [why, n] : failures) std::printf("  failure x%zu: %s\n", n, why.c_str());
    if (report) std::fclose(report);
    kept.clear();
    return failed + missing + flush_failures ? 3 : 0;
}
