#include "test_util.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

using namespace gta6rt::gpu;

namespace {

struct Input {
    std::uint32_t reg = 0, count = 0;
    std::string type;
};

struct Shader {
    std::string stem, split, metallib;
    std::vector<Input> inputs;
    std::vector<std::pair<std::uint32_t, std::string>> colors;
    std::set<std::uint32_t> bindings;
};

struct Pair {
    std::string label, vs, ps;
};

double since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

bool load(const char* path, std::vector<Pair>& pairs, std::map<std::string, Shader>& shaders) {
    std::ifstream mf(path);
    if (!mf) return false;
    for (std::string line; std::getline(mf, line);) {
        std::istringstream s(line);
        std::string kind;
        s >> kind;
        if (kind == "pair") {
            Pair p;
            s >> p.label >> p.vs >> p.ps;
            pairs.push_back(p);
        } else if (kind == "shader") {
            Shader sh;
            std::string stage, base, bindings, s52, moved;
            std::uint32_t push;
            s >> sh.stem >> stage >> base >> sh.split >> push >> bindings >> s52 >> moved >> sh.metallib;
            if (bindings != "-") {
                std::istringstream b(bindings);
                for (std::string x; std::getline(b, x, ',');) sh.bindings.insert(std::uint32_t(std::stoul(x)));
            }
            auto& dst = shaders[sh.stem];
            sh.inputs = dst.inputs;
            sh.colors = dst.colors;
            dst = sh;
        } else if (kind == "input") {
            std::string stem, sem;
            Input in;
            s >> stem >> in.reg >> in.type >> in.count >> sem;
            shaders[stem].inputs.push_back(in);
        } else if (kind == "color") {
            std::string stem, type;
            std::uint32_t idx;
            s >> stem >> idx >> type;
            shaders[stem].colors.emplace_back(idx, type);
        }
    }
    return !pairs.empty();
}

std::uint32_t input_format(const Input& in) {
    if (in.type == "uint" || in.type == "int") return in.count == 1 ? format::r32_uint : format::r32g32b32a32_uint;
    switch (in.count) {
    case 1: return format::r32_float;
    case 2: return format::r32g32_float;
    case 3: return format::r32g32b32_float;
    default: return format::r32g32b32a32_float;
    }
}

RenderPipelineDesc pair_desc(const Shader& vs, const Shader& ps, ShaderLibrary* vl, ShaderLibrary* pl,
                             const RootLayout* root, std::uint32_t rt_format) {
    RenderPipelineDesc pd;
    pd.vs = {vl, "main0"};
    pd.ps = {pl, "main0"};
    std::uint32_t offset = 0;
    for (const Input& in : vs.inputs) {
        pd.inputs.push_back({in.reg, input_format(in), 0, offset});
        offset += 4 * in.count;
    }
    for (const auto& [idx, type] : ps.colors) {
        pd.rt_formats[idx] = type.rfind("uint", 0) == 0 ? format::r8g8b8a8_uint : rt_format;
        pd.rt_count = std::max(pd.rt_count, idx + 1);
    }
    pd.root = root;
    pd.mode = CompileMode::eager;
    return pd;
}

std::unique_ptr<RootLayout> split_root(Device& dev) {
    RootLayoutDesc rd;
    for (std::uint32_t b = 0; b <= 28; ++b)
        rd.tables.push_back({b, b == 1 ? DescriptorKind::sampler : b == 2 ? DescriptorKind::buffer : DescriptorKind::texture});
    return dev.create_root_layout(rd);
}

const char* kTranslator = "route_a split v1 (spirv-cross --msl 30000, set-0 binding b -> set 1+b)";

void phase_cache(Device& dev, const char* dir, bool warm, bool again, std::vector<Pair>& pairs,
                 std::map<std::string, Shader>& shaders) {
    if (!warm) std::filesystem::remove_all(dir);
    std::string err;
    PipelineCacheDesc cd;
    cd.directory = dir;
    cd.translator_version = kTranslator;
    CHECK(dev.open_pipeline_cache(cd, &err) == Status::ok, "open cache: %s", err.c_str());
    const PipelineCacheStats st0 = dev.pipeline_cache_stats();
    std::printf("pipeline cache %s: %s, loaded %d (%.4f s, %llu B), key \"%s\"\n", warm ? "warm" : "cold", st0.path.c_str(),
                st0.loaded, st0.load_seconds, (unsigned long long)st0.file_bytes, st0.key.c_str());
    CHECK(st0.loaded == warm, "archive %s", warm ? "loaded" : "fresh");
    auto root = split_root(dev);
    double lib_total = 0, pso_total = 0, lib_ml = 0, pso_ml = 0;
    int hits = 0, hits_ml = 0, n = 0, n_ml = 0;
    for (const Pair& p : pairs) {
        const Shader& vs = shaders[p.vs];
        const Shader& ps = shaders[p.ps];
        for (int variant = 0; variant < (warm ? 2 : 1); ++variant) {
            std::unique_ptr<ShaderLibrary> vl, pl;
            const auto t0 = std::chrono::steady_clock::now();
            if (variant == 0) {
                vl = dev.create_library_msl(test::read_file(vs.split), MslOptions{}, &err);
                pl = dev.create_library_msl(test::read_file(ps.split), MslOptions{}, &err);
            } else {
                const std::string vb = test::read_file(vs.metallib), pb = test::read_file(ps.metallib);
                vl = dev.create_library_metallib({reinterpret_cast<const std::uint8_t*>(vb.data()), vb.size()}, &err);
                pl = dev.create_library_metallib({reinterpret_cast<const std::uint8_t*>(pb.data()), pb.size()}, &err);
            }
            const double lt = since(t0);
            CHECK(vl && pl, "%s libraries: %s", p.label.c_str(), err.c_str());
            if (!vl || !pl) continue;
            const auto t1 = std::chrono::steady_clock::now();
            auto pso = dev.create_render_pipeline(pair_desc(vs, ps, vl.get(), pl.get(), root.get(), format::r8g8b8a8_unorm));
            const double pt = since(t1);
            CHECK(pso->status() == Status::ok, "%s pipeline: %s", p.label.c_str(), pso->error().c_str());
            std::printf("  %-12s %-8s libraries %.4f s, pipeline %.4f s, from archive %d\n", p.label.c_str(),
                        variant == 0 ? "MSL" : "metallib", lt, pt, pso->from_archive());
            if (variant == 0) {
                lib_total += lt;
                pso_total += pt;
                hits += pso->from_archive();
                ++n;
            } else {
                lib_ml += lt;
                pso_ml += pt;
                hits_ml += pso->from_archive();
                ++n_ml;
            }
        }
    }
    CHECK(dev.flush_pipeline_cache(&err) == Status::ok, "flush: %s", err.c_str());
    const PipelineCacheStats st = dev.pipeline_cache_stats();
    std::printf("%s: %d pairs from MSL: libraries %.4f s, pipelines %.4f s in total, %d archive hits; ", warm ? "warm" : "cold",
                n, lib_total, pso_total, hits);
    if (warm) std::printf("from metallibs: libraries %.4f s, pipelines %.4f s, %d hits; ", lib_ml, pso_ml, hits_ml);
    std::printf("archive hits %llu misses %llu added %llu add failures %llu, file %llu B, serialize %.4f s\n",
                (unsigned long long)st.hits, (unsigned long long)st.misses, (unsigned long long)st.added,
                (unsigned long long)st.add_failures, (unsigned long long)st.file_bytes, st.serialize_seconds);
    const char* sv = std::getenv("MTL_SHADER_VALIDATION");
    if (sv && *sv && *sv != '0') {
        std::printf("shader validation on: archive hits are reported, not checked (%d of %d MSL, %d of %d metallib)\n", hits, n,
                    hits_ml, n_ml);
        return;
    }
    if (warm)
        CHECK(hits == n, "every pipeline built from MSL came from the archive (%d of %d)", hits, n);
    else
        CHECK(st.added == std::uint64_t(n) && st.hits == 0, "cold run added every pipeline to the archive");
    if (again)
        CHECK(hits_ml == n_ml, "metallib pipelines came from the archive on the second warm run (%d of %d)", hits_ml, n_ml);
}

std::unique_ptr<RenderPipeline> async_pipeline(Device& dev, const Shader& vs, const Shader& ps, ShaderLibrary* vl,
                                               ShaderLibrary* pl, const RootLayout* root, std::uint32_t fmt,
                                               Priority prio) {
    RenderPipelineDesc d = pair_desc(vs, ps, vl, pl, root, fmt);
    d.mode = CompileMode::async;
    d.not_ready = NotReady::skip;
    d.priority = prio;
    return dev.create_render_pipeline(d);
}

void phase_async(Device& dev, Device& single, std::vector<Pair>& pairs, std::map<std::string, Shader>& shaders) {
    auto root = split_root(dev);
    auto q = dev.create_queue(QueueType::direct, "async");
    const Pair& bink = pairs.back();
    const Shader& vs = shaders[bink.vs];
    const Shader& ps = shaders[bink.ps];
    std::string err;
    MslOptions deferred;
    deferred.deferred = true;
    const std::string vsrc = test::read_file(vs.split), psrc = test::read_file(ps.split);
    auto t0 = std::chrono::steady_clock::now();
    auto vl = dev.create_library_msl(vsrc, deferred, &err);
    auto pl = dev.create_library_msl(psrc, deferred, &err);
    const double lib_t = since(t0);
    t0 = std::chrono::steady_clock::now();
    auto pso = async_pipeline(dev, vs, ps, vl.get(), pl.get(), root.get(), format::r8g8b8a8_unorm, Priority::demand);
    const double create_t = since(t0);

    TextureDesc td;
    td.format = format::r8g8b8a8_unorm;
    td.width = td.height = 64;
    td.usage = usage::render_target;
    auto rt = dev.create_texture(td);
    BufferDesc vbd{4096, MemoryKind::upload, usage::vertex, "zero vertices"};
    auto vb = dev.create_buffer(vbd);
    std::memset(vb->cpu_pointer(), 0, 4096);
    std::vector<std::unique_ptr<DescriptorHeap>> tables;
    auto list = dev.create_command_list(QueueType::direct);
    TargetView v{rt.get(), 0, 0, 0};
    list->set_render_targets(std::span<const TargetView>(&v, 1), nullptr);
    list->set_pipeline(pso.get());
    list->set_root_layout(root.get());
    for (std::uint32_t b : ps.bindings) {
        tables.push_back(dev.create_descriptor_heap({b == 1 ? DescriptorKind::sampler : b == 2 ? DescriptorKind::buffer
                                                                                                : DescriptorKind::texture,
                                                     16, "empty"}));
        list->set_descriptor_table(b, tables.back().get());
    }
    VertexBufferView vv{vb->gpu_va(), 4096, 64};
    list->set_vertex_buffers(0, std::span<const VertexBufferView>(&vv, 1));
    list->set_topology(topology::triangle_list);
    list->draw(3, 1, 0, 0);
    list->close();
    CommandList* ls[] = {list.get()};
    const auto s0 = dev.stats();
    t0 = std::chrono::steady_clock::now();
    const PipelineState at_submit = pso->state();
    CHECK(q->execute(ls) == Status::ok, "execute with a compiling pipeline");
    const double exec_t = since(t0);
    const auto s1 = dev.stats();
    q->wait_idle(10'000'000'000ull);
    t0 = std::chrono::steady_clock::now();
    CHECK(pso->wait() == Status::ok, "async pipeline: %s", pso->error().c_str());
    const double rest = since(t0);
    CHECK(q->execute(ls) == Status::ok, "execute once ready");
    q->wait_idle(10'000'000'000ull);
    const auto s2 = dev.stats();
    std::printf("async + skip (bink pair, deferred MSL libraries, MTL_SHADER_CACHE_SIZE=%s): create libraries %.6f s, "
                "create pipeline %.6f s, pipeline state at submit %u, execute() %.6f s, draws skipped %llu; the build "
                "finished %.3f s later (compile %.3f s on a worker); next execute drew it (skipped %llu more)\n",
                std::getenv("MTL_SHADER_CACHE_SIZE") ? std::getenv("MTL_SHADER_CACHE_SIZE") : "default", lib_t, create_t,
                unsigned(at_submit), exec_t, (unsigned long long)(s1.draws_skipped_not_ready - s0.draws_skipped_not_ready),
                rest, pso->compile_seconds(),
                (unsigned long long)(s2.draws_skipped_not_ready - s1.draws_skipped_not_ready));
    CHECK(lib_t < 0.01 && create_t < 0.01 && exec_t < 0.02, "nothing compiled on the submitting thread");
    if (at_submit != PipelineState::ready)
        CHECK(s1.draws_skipped_not_ready - s0.draws_skipped_not_ready == 1, "the draw was skipped, not waited for");
    CHECK(s2.draws_skipped_not_ready == s1.draws_skipped_not_ready, "drawn once ready");

    struct Job {
        std::unique_ptr<ShaderLibrary> vl, pl;
        std::unique_ptr<RenderPipeline> p;
        std::string name;
        double done = 0, started = 0;
        bool first = false;
    };
    auto sroot = split_root(single);
    std::vector<Job> jobs;
    const std::uint32_t fmts[3] = {format::r8g8b8a8_unorm, format::r16g16b16a16_float, format::b8g8r8a8_unorm};
    for (std::uint32_t f : fmts)
        for (const Pair& p : pairs) {
            Job j;
            j.vl = single.create_library_msl(test::read_file(shaders[p.vs].split), deferred, &err);
            j.pl = single.create_library_msl(test::read_file(shaders[p.ps].split), deferred, &err);
            j.name = p.label + "/" + std::to_string(f);
            jobs.push_back(std::move(j));
        }
    const auto start = std::chrono::steady_clock::now();
    std::size_t k = 0;
    for (std::uint32_t f : fmts)
        for (const Pair& p : pairs) {
            jobs[k].p = async_pipeline(single, shaders[p.vs], shaders[p.ps], jobs[k].vl.get(), jobs[k].pl.get(),
                                       sroot.get(), f, Priority::background);
            ++k;
        }
    jobs[jobs.size() - 1].first = jobs[jobs.size() - 2].first = true;
    Pipeline* first[2] = {jobs[jobs.size() - 2].p.get(), jobs[jobs.size() - 1].p.get()};
    const CompileQueueStats before = single.compile_queue_stats();
    if (!std::getenv("GTA6RT_TEST_NO_PRECOMPILE")) single.precompile(first, Priority::first_scene);
    std::size_t done = 0;
    while (done < jobs.size()) {
        for (std::size_t x = 0; x < jobs.size(); ++x) {
            Job& j = jobs[x];
            const PipelineState st = j.p->state();
            if (j.started == 0 && st != PipelineState::queued && st != PipelineState::deferred) {
                j.started = since(start);
            }
            if (j.done == 0 && (st == PipelineState::ready || st == PipelineState::failed)) {
                j.done = since(start);
                ++done;
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    const CompileQueueStats after = single.compile_queue_stats();
    double last_first = 0;
    for (const Job& j : jobs)
        if (j.first) last_first = std::max(last_first, j.started);
    int bg_before = 0;
    for (const Job& j : jobs)
        if (!j.first && j.started < last_first) ++bg_before;
    std::sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.done < b.done; });
    std::printf("precompile: 12 pipelines queued at background on %u compile thread(s) (%llu queued, %llu running when "
                "the first-scene request came); order (polled every 100 us):\n",
                after.threads, (unsigned long long)before.queued[2], (unsigned long long)before.running);
    int i = 0;
    for (const Job& j : jobs) {
        ++i;
        std::printf("    %2d %-26s started %.3f s, done %.3f s%s\n", i, j.name.c_str(), j.started, j.done,
                    j.first ? "  <- first scene" : "");
    }
    const int allowed = int(before.running) + 1;
    std::printf("background pipelines that started before the last first-scene one: %d (allowed %d: the %llu running "
                "at the stats snapshot + 1 the worker may pop between the snapshot and the reorder)\n",
                bg_before, allowed, (unsigned long long)before.running);
    CHECK(bg_before <= allowed, "first-scene pipelines start ahead of the background queue");
    CHECK(after.completed[0] >= 2, "first-scene compiles counted (%llu)", (unsigned long long)after.completed[0]);
}

}  // namespace

int main(int argc, char** argv) {
    std::string phase = "cold", dir, manifest;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--phase" && i + 1 < argc) phase = argv[++i];
        else if (a == "--dir" && i + 1 < argc) dir = argv[++i];
        else manifest = a;
    }
    std::vector<Pair> pairs;
    std::map<std::string, Shader> shaders;
    if (manifest.empty() || !load(manifest.c_str(), pairs, shaders)) {
        std::printf("SKIP: no manifest\n");
        return 77;
    }
    auto dev = gta6rt::metal::create_device();
    if (!dev) return 1;
    std::printf("phase %s, MTL_SHADER_CACHE_SIZE=%s\n", phase.c_str(),
                std::getenv("MTL_SHADER_CACHE_SIZE") ? std::getenv("MTL_SHADER_CACHE_SIZE") : "(default)");
    if (phase == "cold" || phase == "warm" || phase == "warm2")
        phase_cache(*dev, dir.c_str(), phase != "cold", phase == "warm2", pairs, shaders);
    if (phase == "async") {
        gta6rt::metal::DeviceOptions one;
        one.compile_threads = 1;
        auto single = gta6rt::metal::create_device(one);
        phase_async(*dev, *single, pairs, shaders);
    }
    test::print_stats(dev->stats());
    return test::finish("metal_test_pipeline_cache");
}
