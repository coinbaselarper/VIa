// Archive keying and invalidation: an archive is used only by the same key (translator version here; the backend
// adds GPU, OS build and process arch); a key file that does not match (another OS build or GPU under the same file
// name) or an unreadable archive file is never used, and caching continues with a new archive.

#include "pcache.h"

#include <gta6rt_metal/metal_backend.h>
#include <gta6rt_shaderc_msl/catalog.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

namespace gpu = gta6rt::gpu;
namespace pc = gta6rt::pcache;
namespace sc = gta6rt::shaderc_msl;

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
    std::uint64_t hits = 0, misses = 0, ok = 0;
    bool loaded = false;
    std::string path, log;
};

Run build(const sc::Catalog& catalog, const std::string& dir, const std::string& version) {
    Run r;
    auto device = gta6rt::metal::create_device({});
    pc::open_archive(*device, dir, version, true, &r.log);
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
    for (std::uint32_t format : {28u, 87u, 10u, 24u}) {
        gpu::RenderPipelineDesc d;
        d.vs = {lib("VS_Blit_6e5520d1_Wrapped"), "main0"};
        d.ps = {lib("PS_BlitColour_6e5520d1_Wrapped"), "main0"};
        d.inputs = {{0, 6, 0, 0, false, 1}, {1, 2, 0, 12, false, 1}, {2, 16, 0, 28, false, 1}};
        d.rt_count = 1;
        d.rt_formats[0] = format;
        d.root = root.get();
        auto p = device->create_render_pipeline(d);
        r.ok += p && p->state() == gpu::PipelineState::ready;
    }
    device->flush_pipeline_cache(nullptr);
    const auto st = device->pipeline_cache_stats();
    r.hits = st.hits;
    r.misses = st.misses;
    r.loaded = st.loaded;
    r.path = st.path;
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string port = GTA6RT_PORT_ROOT;
    const std::filesystem::path dir = argc > 1 ? argv[1] : "pcache-archive-tmp";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    sc::Catalog catalog;
    if (!catalog.load(port + "/build/shaderc_msl/runs/main/manifest.tsv") || !catalog.find("VS_Blit_6e5520d1_Wrapped")) {
        std::printf("SKIP: no shader store\n");
        return 77;
    }
    const auto show = [](const char* what, const Run& r) {
        std::printf("%-34s ok %llu, hits %llu, misses %llu, %s | %s\n", what, (unsigned long long)r.ok,
                    (unsigned long long)r.hits, (unsigned long long)r.misses, r.loaded ? "loaded" : "new", r.log.c_str());
    };
    const Run cold = build(catalog, dir.string(), "version A");
    show("cold, version A", cold);
    CHECK(cold.ok == 4 && cold.hits == 0 && !cold.loaded);
    const Run warm = build(catalog, dir.string(), "version A");
    show("warm, version A", warm);
    CHECK(warm.ok == 4 && warm.hits == 4 && warm.loaded);
    const Run other = build(catalog, dir.string(), "version B");
    show("other translator version B", other);
    CHECK(other.ok == 4 && other.hits == 0 && !other.loaded && other.path != warm.path);

    {
        std::ofstream(warm.path + ".key") << "gta6rt_metal binary archive v1; translator version A; gpu Some Other GPU";
    }
    const Run stale = build(catalog, dir.string(), "version A");
    show("same file, key of another GPU/OS", stale);
    CHECK(stale.ok == 4 && stale.hits == 0 && !stale.loaded);

    {
        const auto size = std::filesystem::file_size(stale.path);
        std::filesystem::resize_file(stale.path, size / 3);
        std::fstream f(stale.path, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(64);
        const char junk[64] = {1, 2, 3, 4, 5, 6, 7, 8};
        f.write(junk, sizeof junk);
    }
    const Run torn = build(catalog, dir.string(), "version A");
    show("torn archive file, right key", torn);
    CHECK(torn.ok == 4 && torn.hits == 0);
    const Run after = build(catalog, dir.string(), "version A");
    show("after the torn one", after);
    CHECK(after.ok == 4 && after.hits == 4 && after.loaded);

    std::printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
