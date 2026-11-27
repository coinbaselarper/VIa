// Archive shards, keying and invalidation: shards are used only by the same key (translator version here; the backend
// adds GPU, OS build and process arch); every shard with the key is loaded; a flush writes only what is new, as a new
// file, and never changes a written one (not on a failed write, not when its key file is stale, not when it is torn);
// a flush does not hold up compiles started meanwhile.

#include "pcache.h"

#include <gta6rt_metal/metal_backend.h>
#include <gta6rt_shaderc_msl/catalog.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sys/stat.h>
#include <thread>

namespace gpu = gta6rt::gpu;
namespace pc = gta6rt::pcache;
namespace sc = gta6rt::shaderc_msl;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

namespace {

struct Run {
    std::uint64_t hits = 0, misses = 0, ok = 0, written = 0;
    std::uint32_t shards_open = 0, shards_end = 0, unreadable = 0;
    bool loaded = false, flushed = true;
    std::string prefix, log;
    double flush_s = 0;
};

using Snapshot = std::map<std::string, std::tuple<std::uint64_t, std::uint64_t, std::int64_t>>;

Snapshot shards_of(const fs::path& dir) {
    Snapshot out;
    for (const auto& e : fs::directory_iterator(dir)) {
        const std::string n = e.path().filename().string();
        if (n.size() < 13 || n.compare(n.size() - 13, 13, ".metalarchive") != 0) continue;
        struct stat sb {};
        ::stat(e.path().c_str(), &sb);
        out[n] = {std::uint64_t(sb.st_ino), std::uint64_t(sb.st_size), std::int64_t(sb.st_mtimespec.tv_nsec + sb.st_mtimespec.tv_sec * 1000000000ll)};
    }
    return out;
}

// Every file of `before` is still there, unchanged (same inode, size, mtime).
bool unchanged(const Snapshot& before, const Snapshot& after) {
    for (const auto& [n, v] : before) {
        auto it = after.find(n);
        if (it == after.end() || it->second != v) {
            std::fprintf(stderr, "  shard %s changed or vanished\n", n.c_str());
            return false;
        }
    }
    return true;
}

struct Opts {
    std::vector<std::uint32_t> formats{28u, 87u, 10u, 24u};
    bool concurrent_flush = false;
    bool readonly_dir_at_flush = false;
};

Run build(const sc::Catalog& catalog, const std::string& dir, const std::string& version, const Opts& o = {}) {
    Run r;
    auto device = gta6rt::metal::create_device({});
    pc::open_archive(*device, dir, version, true, &r.log);
    r.shards_open = device->pipeline_cache_stats().shards;
    r.unreadable = device->pipeline_cache_stats().shards_unreadable;
    gpu::RootLayoutDesc rd;
    for (std::uint32_t b = 0; b < 29; ++b)
        rd.tables.push_back({b, b == 1 ? gpu::DescriptorKind::sampler : b == 2 ? gpu::DescriptorKind::buffer
                                                                                : gpu::DescriptorKind::texture});
    auto root = device->create_root_layout(rd);
    std::map<std::string, std::unique_ptr<gpu::ShaderLibrary>> libs;
    const auto lib = [&](const std::string& name) {
        auto& l = libs[name];
        if (!l) {
            std::ifstream in(catalog.library_path(*catalog.find(name)), std::ios::binary);
            std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            l = device->create_library_metallib(bytes, nullptr);
        }
        return l.get();
    };
    std::atomic<bool> stop{false};
    std::atomic<int> flushes{0};
    std::thread flusher;
    if (o.concurrent_flush)
        flusher = std::thread([&] {
            while (!stop) {
                device->flush_pipeline_cache(nullptr);
                ++flushes;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
    std::vector<std::unique_ptr<gpu::RenderPipeline>> ps;
    for (std::uint32_t format : o.formats) {
        gpu::RenderPipelineDesc d;
        d.vs = {lib("VS_Blit_6e5520d1_Wrapped"), "main0"};
        d.ps = {lib("PS_BlitColour_6e5520d1_Wrapped"), "main0"};
        d.inputs = {{0, 6, 0, 0, false, 1}, {1, 2, 0, 12, false, 1}, {2, 16, 0, 28, false, 1}};
        d.rt_count = 1;
        d.rt_formats[0] = format;
        d.root = root.get();
        if (o.concurrent_flush) {
            d.mode = gpu::CompileMode::async;
            d.not_ready = gpu::NotReady::skip;
        }
        ps.push_back(device->create_render_pipeline(d));
    }
    for (auto& p : ps) r.ok += p && p->wait() == gpu::Status::ok;
    stop = true;
    if (flusher.joinable()) flusher.join();
    if (o.readonly_dir_at_flush) ::chmod(dir.c_str(), 0555);
    const auto t0 = std::chrono::steady_clock::now();
    r.flushed = device->flush_pipeline_cache(nullptr) == gpu::Status::ok;
    r.flush_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (o.readonly_dir_at_flush) ::chmod(dir.c_str(), 0755);
    const auto st = device->pipeline_cache_stats();
    r.hits = st.hits;
    r.misses = st.misses;
    r.loaded = st.loaded;
    r.prefix = st.path;
    r.written = st.written;
    r.shards_end = st.shards;
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string port = GTA6RT_PORT_ROOT;
    const fs::path dir = argc > 1 ? argv[1] : "pcache-archive-tmp";
    fs::remove_all(dir);
    fs::create_directories(dir);
    sc::Catalog catalog;
    if (!catalog.load(port + "/build/shaderc_msl/runs/main/manifest.tsv") || !catalog.find("VS_Blit_6e5520d1_Wrapped")) {
        std::printf("SKIP: no shader store\n");
        return 77;
    }
    const auto show = [](const char* what, const Run& r) {
        std::printf("%-40s ok %llu, hits %llu, misses %llu, %s, shards %u -> %u, written %llu, unreadable %u | %s\n", what,
                    (unsigned long long)r.ok, (unsigned long long)r.hits, (unsigned long long)r.misses,
                    r.loaded ? "loaded" : "new", r.shards_open, r.shards_end, (unsigned long long)r.written, r.unreadable,
                    r.log.c_str());
    };
    const Run cold = build(catalog, dir.string(), "version A");
    show("cold, version A", cold);
    CHECK(cold.ok == 4 && cold.hits == 0 && !cold.loaded && cold.shards_end == 1 && cold.written == 4);
    Snapshot s1 = shards_of(dir);
    CHECK(s1.size() == 1);

    const Run warm = build(catalog, dir.string(), "version A");
    show("warm, version A", warm);
    CHECK(warm.ok == 4 && warm.hits == 4 && warm.loaded && warm.shards_open == 1 && warm.shards_end == 1);
    CHECK(unchanged(s1, shards_of(dir)) && shards_of(dir).size() == 1);

    Opts more;
    more.formats = {28u, 87u, 10u, 24u, 2u, 11u};
    const Run grow = build(catalog, dir.string(), "version A", more);
    show("warm + 2 new, version A", grow);
    CHECK(grow.ok == 6 && grow.hits == 4 && grow.written == 2 && grow.shards_end == 2);
    CHECK(unchanged(s1, shards_of(dir)));
    Snapshot s2 = shards_of(dir);
    CHECK(s2.size() == 2);

    const Run both = build(catalog, dir.string(), "version A", more);
    show("both shards, version A", both);
    CHECK(both.ok == 6 && both.hits == 6 && both.shards_open == 2 && both.written == 0);

    const Run other = build(catalog, dir.string(), "version B");
    show("other translator version B", other);
    CHECK(other.ok == 4 && other.hits == 0 && !other.loaded && other.prefix != warm.prefix);
    CHECK(unchanged(s2, shards_of(dir)));

    Opts ro = more;
    ro.formats.push_back(16u);
    s2 = shards_of(dir);
    Opts ro_dir = ro;
    ro_dir.readonly_dir_at_flush = true;
    const Run failed_write = build(catalog, dir.string(), "version A", ro_dir);
    show("write fails (directory read-only)", failed_write);
    CHECK(failed_write.ok == 7 && !failed_write.flushed && failed_write.written == 0);
    CHECK(unchanged(s2, shards_of(dir)));
    const Run after_fail = build(catalog, dir.string(), "version A", ro);
    show("after the failed write", after_fail);
    CHECK(after_fail.ok == 7 && after_fail.hits == 6 && after_fail.written == 1 && after_fail.flushed);
    CHECK(unchanged(s2, shards_of(dir)));
    Snapshot s3 = shards_of(dir);

    const std::string first = warm.prefix + "-run-0001.metalarchive";
    { std::ofstream(first + ".key") << "gta6rt_metal binary archive v1; translator version A; gpu Some Other GPU"; }
    const Run stale = build(catalog, dir.string(), "version A", ro);
    show("first shard's key of another GPU/OS", stale);
    CHECK(stale.ok == 7 && stale.hits == 3 && stale.shards_open == 2 && stale.written == 4);
    CHECK(unchanged(s3, shards_of(dir)));

    const std::string second = warm.prefix + "-run-0002.metalarchive";
    {
        const auto size = fs::file_size(second);
        fs::resize_file(second, size / 3);
        std::fstream f(second, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(64);
        const char junk[64] = {1, 2, 3, 4, 5, 6, 7, 8};
        f.write(junk, sizeof junk);
    }
    const Run torn = build(catalog, dir.string(), "version A", ro);
    show("torn second shard, right key", torn);
    CHECK(torn.ok == 7 && torn.unreadable == 1 && fs::exists(second + ".unreadable") && !fs::exists(second));
    const Run after = build(catalog, dir.string(), "version A", ro);
    show("after the torn one", after);
    CHECK(after.ok == 7 && after.hits == 7 && after.loaded && after.written == 0);

    Opts conc;
    conc.formats = {28u, 87u, 10u, 24u, 2u, 11u, 16u, 41u, 34u, 35u, 49u, 54u, 56u, 13u};
    conc.concurrent_flush = true;
    const Snapshot s4 = shards_of(dir);
    const Run concurrent = build(catalog, dir.string(), "version A", conc);
    show("async compiles with a flush every 5 ms", concurrent);
    CHECK(concurrent.ok == conc.formats.size());
    CHECK(unchanged(s4, shards_of(dir)));
    const Run concurrent_warm = build(catalog, dir.string(), "version A", conc);
    show("their next run", concurrent_warm);
    CHECK(concurrent_warm.ok == conc.formats.size() && concurrent_warm.hits == conc.formats.size());

    std::printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
