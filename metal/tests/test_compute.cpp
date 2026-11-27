#include "test_util.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace gta6rt::gpu;

namespace {

constexpr std::uint32_t kUav = 0x8, kIndirect = 0x200;

struct Env {
    Device* dev = nullptr;
    Queue* q = nullptr;
    std::unique_ptr<ShaderLibrary> lib;
    std::unique_ptr<RootLayout> root;
    std::unique_ptr<Heap> arena;
    std::uint8_t* cpu = nullptr;
    std::uint64_t base = 0;
};

double since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

std::unique_ptr<ComputePipeline> make(Env& e, const char* entry, std::array<std::uint32_t, 3> threads = {0, 0, 0}) {
    ComputePipelineDesc d;
    d.cs = {e.lib.get(), entry};
    d.threads = threads;
    d.root = e.root.get();
    d.mode = CompileMode::eager;
    return e.dev->create_compute_pipeline(d);
}

void uav_barrier(CommandList& l) {
    Barrier b;
    b.type = BarrierType::uav;
    l.barrier(std::span<const Barrier>(&b, 1));
}

bool run(Env& e, CommandList& l) {
    if (l.close() != Status::ok) return false;
    CommandList* ls[] = {&l};
    return e.q->execute(ls) == Status::ok && e.q->wait_idle(30'000'000'000ull) == Status::ok;
}

void user(CommandList& l, std::initializer_list<std::uint32_t> v) {
    std::vector<std::uint32_t> u(v);
    l.set_user_data(0, u);
}

constexpr std::uint32_t kMark = 0x100000, kCounter = 0x40, kMarkCap = 4'000'000;

void large_grids(Env& e, ComputePipeline* mark) {
    struct Case { std::uint32_t x, y, z; };
    for (const Case c : {Case{131072, 1, 1}, Case{1, 70000, 1}, Case{300, 300, 2}, Case{200000, 1, 1}}) {
        const std::uint64_t n = std::uint64_t(c.x) * c.y * c.z;
        std::memset(e.cpu + kMark, 0, n * 4);
        std::memset(e.cpu + kCounter, 0, 4);
        auto l = e.dev->create_command_list(QueueType::compute);
        l->set_root_layout(e.root.get());
        l->set_heap_base(e.base);
        l->set_compute_pipeline(mark);
        user(*l, {kMark, kCounter, kMarkCap});
        l->dispatch(c.x, c.y, c.z);
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(run(e, *l), "dispatch %ux%ux%u", c.x, c.y, c.z);
        const double s = since(t0);
        const auto* m = reinterpret_cast<const std::uint32_t*>(e.cpu + kMark);
        std::uint64_t once = 0, other = 0;
        for (std::uint64_t i = 0; i < n; ++i) (m[i] == 1 ? once : other)++;
        const std::uint32_t threads = *reinterpret_cast<const std::uint32_t*>(e.cpu + kCounter);
        std::printf("dispatch %u x %u x %u groups (%llu): %llu groups ran once, %llu not once; threads %u (want %llu); %.3f s\n",
                    c.x, c.y, c.z, (unsigned long long)n, (unsigned long long)once, (unsigned long long)other, threads,
                    (unsigned long long)(n * 64), s);
        CHECK(once == n && threads == n * 64, "every group of a %ux%ux%u grid ran exactly once", c.x, c.y, c.z);
    }
}

void thread_counts(Env& e, ComputePipeline* mark) {
    struct Case { std::uint32_t x, y, z, flags; std::uint64_t threads, groups; };
    const Case cases[] = {{1000, 1, 1, dispatch_flag::thread_counts, 1000, 16},
                          {1000, 1, 1, 0, 64000, 1000},
                          {100, 30, 1, dispatch_flag::thread_counts, 3000, 60},
                          {65, 1, 1, dispatch_flag::thread_counts, 65, 2}};
    for (const Case& c : cases) {
        std::memset(e.cpu + kMark, 0, 1 << 20);
        std::memset(e.cpu + kCounter, 0, 4);
        auto l = e.dev->create_command_list(QueueType::compute);
        l->set_root_layout(e.root.get());
        l->set_heap_base(e.base);
        l->set_compute_pipeline(mark);
        user(*l, {kMark, kCounter, kMarkCap});
        l->dispatch(c.x, c.y, c.z, c.flags);
        CHECK(run(e, *l), "dispatch");
        const std::uint32_t threads = *reinterpret_cast<const std::uint32_t*>(e.cpu + kCounter);
        const auto* m = reinterpret_cast<const std::uint32_t*>(e.cpu + kMark);
        std::uint64_t groups = 0;
        for (std::uint32_t i = 0; i < 100000; ++i) groups += m[i];
        std::printf("dispatch(%u, %u, %u, flags 0x%x) with 64-thread groups: %u threads in %llu groups (want %llu in %llu)\n",
                    c.x, c.y, c.z, c.flags, threads, (unsigned long long)groups, (unsigned long long)c.threads,
                    (unsigned long long)c.groups);
        CHECK(threads == c.threads && groups == c.groups, "thread-count dispatch flag");
    }
}

int chain(Env& e, ComputePipeline* step, std::uint32_t k, bool barriers, std::uint32_t heavy, double* seconds) {
    const std::uint32_t n = 1u << 18;
    const std::uint32_t a = 0x800000, b = a + n * 4;
    std::memset(e.cpu + a, 0, n * 4);
    std::memset(e.cpu + b, 0, n * 4);
    auto l = e.dev->create_command_list(QueueType::compute);
    l->set_root_layout(e.root.get());
    l->set_heap_base(e.base);
    l->set_compute_pipeline(step);
    for (std::uint32_t i = 0; i < k; ++i) {
        const bool even = (i & 1) == 0;
        user(*l, {even ? a : b, even ? b : a, n, heavy});
        l->dispatch(n / 64, 1, 1);
        if (barriers && i + 1 < k) uav_barrier(*l);
    }
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(run(e, *l), "chain");
    *seconds = since(t0);
    const auto* out = reinterpret_cast<const std::uint32_t*>(e.cpu + ((k & 1) ? b : a));
    int bad = 0;
    for (std::uint32_t i = 0; i < n; ++i) bad += out[i] != k;
    return bad;
}

void dependent_chain(Env& e, ComputePipeline* step) {
    const auto before = e.dev->stats();
    double s = 0;
    const int bad = chain(e, step, 64, true, 64, &s);
    const auto after = e.dev->stats();
    std::printf("chain of 64 dependent dispatches with UAV barriers: %d of 262144 elements wrong; %.3f s; compute "
                "encoders %llu, in-encoder barriers %llu, fenced barriers %llu\n",
                bad, s, (unsigned long long)(after.compute_encoders - before.compute_encoders),
                (unsigned long long)(after.barriers_in_encoder - before.barriers_in_encoder),
                (unsigned long long)(after.barriers_fenced - before.barriers_fenced));
    CHECK(bad == 0, "UAV barriers order dependent dispatches");
    CHECK(after.barriers_in_encoder - before.barriers_in_encoder == 63 &&
              after.compute_encoders - before.compute_encoders == 1,
          "the barriers stayed inside one concurrent compute encoder");
    double s2 = 0;
    const int racy = chain(e, step, 64, false, 64, &s2);
    std::printf("negative control, same chain without barriers: %d of 262144 elements wrong (%.3f s)\n", racy, s2);
}

void indirect(Env& e, ComputePipeline* mark, ComputePipeline* writer) {
    constexpr std::uint32_t kArgs = 0x2000000, kCount = 0x2001000;
    struct Case { bool count; std::uint32_t want_groups; };
    for (const Case c : {Case{true, 70005}, Case{false, 71004}}) {
        std::memset(e.cpu + kMark, 0, 80000 * 4);
        std::memset(e.cpu + kCounter, 0, 4);
        std::memset(e.cpu + kArgs, 0xff, 64);
        std::memset(e.cpu + kCount, 0xff, 4);
        auto l = e.dev->create_command_list(QueueType::compute);
        l->set_root_layout(e.root.get());
        l->set_heap_base(e.base);
        l->set_compute_pipeline(writer);
        user(*l, {kArgs, kCount, 0, 0, 70000, 5, 999, 2});
        l->dispatch(1, 1, 1);
        Barrier b;
        b.resource = e.arena->arena();
        b.before = kUav;
        b.after = kIndirect;
        l->barrier(std::span<const Barrier>(&b, 1));
        l->set_compute_pipeline(mark);
        user(*l, {kMark, kCounter, kMarkCap});
        IndirectDesc d;
        d.kind = IndirectKind::dispatch;
        d.stride = 16;
        d.max_count = 3;
        d.arguments = e.arena->arena();
        d.argument_offset = kArgs;
        if (c.count) {
            d.count_buffer = e.arena->arena();
            d.count_offset = kCount;
        }
        l->execute_indirect(d);
        const auto before = e.dev->stats();
        CHECK(run(e, *l), "indirect");
        const auto after = e.dev->stats();
        const std::uint32_t threads = *reinterpret_cast<const std::uint32_t*>(e.cpu + kCounter);
        const auto* m = reinterpret_cast<const std::uint32_t*>(e.cpu + kMark);
        std::uint64_t groups = 0, twice = 0;
        for (std::uint32_t i = 0; i < 80000; ++i) {
            groups += m[i];
            twice += m[i] >= 2;
        }
        std::printf("indirect dispatch, GPU-written records (70000, 5, 999 groups), %s: %llu groups, %u threads, "
                    "%llu indices hit by more than one record; pre-passes %llu\n",
                    c.count ? "GPU-written count 2" : "no count buffer, max 3", (unsigned long long)groups, threads,
                    (unsigned long long)twice, (unsigned long long)(after.indirect_prepasses - before.indirect_prepasses));
        CHECK(groups == c.want_groups && threads == c.want_groups * 64ull, "indirect dispatch group total");
        CHECK(twice == (c.count ? 5u : 999u), "records overlap as expected");
    }
}

void threadgroup_limits(Env& e) {
    auto ok = make(e, "cs_tg32k");
    std::printf("cs_tg32k: %s, threadgroup memory %u B (device limit %u B), threads %u,%u,%u (from gl_WorkGroupSize), "
                "execution width %u\n",
                status_name(ok->status()), ok->threadgroup_memory(), e.dev->caps().max_threadgroup_memory,
                ok->threads()[0], ok->threads()[1], ok->threads()[2], ok->execution_width());
    const char* sv = std::getenv("MTL_SHADER_VALIDATION");
    const bool validation = sv && *sv && *sv != '0';
    if (validation) std::printf("  shader validation on: staticThreadgroupMemoryLength includes validation storage\n");
    CHECK(ok->status() == Status::ok && (validation || ok->threadgroup_memory() == 32768),
          "32 KiB of threadgroup memory builds");
    auto over = make(e, "cs_tg40k");
    std::printf("cs_tg40k: %s, limit %s value %llu limit %llu (%s): %s\n", status_name(over->status()),
                limit_kind_name(over->limit().kind), (unsigned long long)over->limit().value,
                (unsigned long long)over->limit().limit, over->limit().estimated ? "estimated" : "Metal",
                over->error().c_str());
    CHECK(over->status() == Status::limit_exceeded && over->limit().kind == PipelineLimit::Kind::threadgroup_memory &&
              over->limit().value == 40960 && over->limit().limit == 32768,
          "40 KiB of threadgroup memory is refused with the size");
    auto big = make(e, "cs_mark", {2048, 1, 1});
    CHECK(big->status() == Status::limit_exceeded && big->limit().kind == PipelineLimit::Kind::threads_per_group,
          "2048-thread groups are refused (%s)", big->error().c_str());
}

void encode_cost(Env& e, ComputePipeline* mark) {
    const std::uint32_t n = 10000;
    std::memset(e.cpu + kCounter, 0, 4);
    auto l = e.dev->create_command_list(QueueType::compute);
    l->set_root_layout(e.root.get());
    l->set_heap_base(e.base);
    l->set_compute_pipeline(mark);
    user(*l, {kMark, kCounter, kMarkCap});
    for (std::uint32_t i = 0; i < n; ++i) l->dispatch(1, 1, 1);
    l->close();
    CommandList* ls[] = {l.get()};
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(e.q->execute(ls) == Status::ok, "execute");
    const double enc = since(t0);
    CHECK(e.q->wait_idle(30'000'000'000ull) == Status::ok, "idle");
    const double all = since(t0);
    std::printf("10,000 one-group dispatches in one list: execute() %.2f ms (%.2f us per dispatch), done after %.2f ms\n",
                enc * 1e3, enc * 1e6 / n, all * 1e3);
    CHECK(*reinterpret_cast<const std::uint32_t*>(e.cpu + kCounter) == n * 64, "all dispatches ran");
}

struct GameCs {
    std::string stem, path, bindings, size;
    std::uint32_t lds = 0;
};

void game_compute(Env& e, const char* manifest) {
    std::ifstream mf(manifest);
    if (!mf) {
        std::printf("game compute shaders: no manifest (%s), skipped\n", manifest);
        return;
    }
    std::vector<GameCs> list;
    for (std::string line; std::getline(mf, line);) {
        std::istringstream s(line);
        std::string kind;
        s >> kind;
        if (kind != "compute") continue;
        GameCs g;
        s >> g.stem >> g.path >> g.lds >> g.bindings >> g.size;
        list.push_back(g);
    }
    RootLayoutDesc rd;
    for (std::uint32_t b = 0; b <= 28; ++b)
        rd.tables.push_back({b, b == 1 ? DescriptorKind::sampler : b == 2 ? DescriptorKind::buffer : DescriptorKind::texture});
    auto root = e.dev->create_root_layout(rd);
    for (const GameCs& g : list) {
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        auto lib = e.dev->create_library_msl(test::read_file(g.path), MslOptions{}, &err);
        const double lt = since(t0);
        CHECK(lib != nullptr, "%s: MSL compile: %s", g.stem.c_str(), err.c_str());
        if (!lib) continue;
        ComputePipelineDesc d;
        d.cs = {lib.get(), "main0"};
        d.root = root.get();
        std::sscanf(g.size.c_str(), "%u,%u,%u", &d.threads[0], &d.threads[1], &d.threads[2]);
        auto p = e.dev->create_compute_pipeline(d);
        std::set<std::uint32_t> want, got;
        if (g.bindings != "-") {
            std::istringstream b(g.bindings);
            for (std::string x; std::getline(b, x, ',');) want.insert(std::uint32_t(std::stoul(x)));
        }
        for (const auto& pe : p->reflection().plan)
            if (pe.what == BindingPlanEntry::What::table) got.insert(pe.binding);
        const auto t = p->threads();
        std::printf("game CS %s: lds_bytes %u, local_size %s; MSL %.3f s, pipeline %s in %.3f s, threads %u,%u,%u, "
                    "threadgroup memory %u B, limit %s %llu/%llu, tables %zu (index.json %zu), mismatches %zu\n",
                    g.stem.c_str(), g.lds, g.size.c_str(), lt, status_name(p->status()), p->compile_seconds(), t[0],
                    t[1], t[2], p->threadgroup_memory(), limit_kind_name(p->limit().kind),
                    (unsigned long long)p->limit().value, (unsigned long long)p->limit().limit, got.size(), want.size(),
                    p->reflection().mismatches.size());
        const std::string threads = std::to_string(t[0]) + "," + std::to_string(t[1]) + "," + std::to_string(t[2]);
        CHECK(threads == g.size, "%s: threadgroup size is index.json local_size", g.stem.c_str());
        if (g.lds <= e.dev->caps().max_threadgroup_memory) {
            CHECK(p->status() == Status::ok, "%s builds (%s)", g.stem.c_str(), p->error().c_str());
            std::string missing;
            for (std::uint32_t b : want)
                if (!got.count(b)) missing += std::to_string(b) + " ";
            const bool subset = std::includes(want.begin(), want.end(), got.begin(), got.end());
            std::printf("    index.json bindings with no argument buffer in the MSL: %s\n",
                        missing.empty() ? "none" : missing.c_str());
            CHECK(subset && !got.empty() && p->reflection().mismatches.empty(),
                  "%s: reflected tables are index.json bindings, no mismatch", g.stem.c_str());
        } else {
            CHECK(p->status() == Status::limit_exceeded && p->limit().kind == PipelineLimit::Kind::threadgroup_memory &&
                      p->limit().value == g.lds,
                  "%s refused with threadgroup memory = lds_bytes (%s)", g.stem.c_str(), p->error().c_str());
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    auto dev = gta6rt::metal::create_device();
    if (!dev) return 1;
    test::print_caps(dev->caps());
    const auto& c = dev->caps();
    std::printf("compute caps: max threads per group %u, max groups per dimension %llu, threadgroup memory %u B\n",
                c.max_threads_per_threadgroup, (unsigned long long)c.max_dispatch_groups, c.max_threadgroup_memory);
    auto queue = dev->create_queue(QueueType::compute, "compute");
    Env e;
    e.dev = dev.get();
    e.q = queue.get();
    std::string err;
    e.lib = dev->create_library_msl(test::read_file(test::shader_path("compute.metal")), MslOptions{}, &err);
    CHECK(e.lib != nullptr, "library: %s", err.c_str());
    if (!e.lib) return test::finish("metal_test_compute");
    e.root = dev->create_root_layout(RootLayoutDesc{});
    HeapDesc hd;
    hd.size = 64ull << 20;
    hd.memory = MemoryKind::shared;
    hd.console_arena = true;
    hd.label = "compute arena";
    e.arena = dev->create_heap(hd);
    CHECK(e.arena && e.arena->arena(), "arena");
    if (!e.arena) return test::finish("metal_test_compute");
    e.cpu = static_cast<std::uint8_t*>(e.arena->arena()->cpu_pointer());
    e.base = e.arena->host_gpu_address();

    auto mark = make(e, "cs_mark");
    auto step = make(e, "cs_step", {64, 1, 1});
    auto writer = make(e, "cs_write_args", {64, 1, 1});
    CHECK(mark->status() == Status::ok && step->status() == Status::ok && writer->status() == Status::ok,
          "pipelines: %s %s %s", mark->error().c_str(), step->error().c_str(), writer->error().c_str());
    CHECK(mark->reflection().mismatches.empty(), "binding plan: push block found");
    if (test::failures) return test::finish("metal_test_compute");

    large_grids(e, mark.get());
    thread_counts(e, mark.get());
    dependent_chain(e, step.get());
    indirect(e, mark.get(), writer.get());
    threadgroup_limits(e);
    encode_cost(e, mark.get());
    if (argc > 1) game_compute(e, argv[1]);
    test::print_stats(dev->stats());
    return test::finish("metal_test_compute");
}
