// The in-process recorder and prebuild on a real Metal device with store metallibs: a manifest's pipelines are
// prebuilt in the background and adopted at first use, an unknown pipeline is created and its wait recorded, a
// bootstrap name match prebuilds at creation, and the manifest written at flush merges this run into the earlier one.
// With a second argument "cold" (GTA6RT_METAL_PCACHE_PREBUILD=cold): only the tier entry whose shader changed is
// prebuilt; the pipelines a manifest lists under their current keys are made at their first use.

#include "pcache.h"

#include <gta6rt_metal/metal_backend.h>
#include <gta6rt_shaderc_msl/catalog.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <thread>

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

int main(int argc, char** argv) {
    const std::string port = GTA6RT_PORT_ROOT;
    const bool cold = argc > 2 && std::string(argv[2]) == "cold";
    const std::filesystem::path dir = std::string(argc > 1 ? argv[1] : "pcache-runtime-tmp") + (cold ? "-cold" : "");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string catalog_path = port + "/build/shaderc_msl/runs/main/manifest.tsv";
    sc::Catalog catalog;
    if (!catalog.load(catalog_path)) {
        std::printf("SKIP: no shader store at %s\n", catalog_path.c_str());
        return 77;
    }
    const char* vs = "VS_Blit_6e5520d1_Wrapped";
    const char* ps = "PS_BlitColour_6e5520d1_Wrapped";
    const char* cs = "library/lib_cs_01726";
    if (!catalog.find(vs) || !catalog.find(ps) || !catalog.find(cs)) {
        std::printf("SKIP: test shaders not in the store\n");
        return 77;
    }
    const std::filesystem::path tier = dir.string() + "-tier";
    std::filesystem::remove_all(tier);
    std::filesystem::create_directories(tier);
    ::setenv("GTA6RT_METAL_ARCHIVE_TIERS", tier.c_str(), 1);
    ::setenv("GTA6RT_METAL_PCACHE_RELEASE_S", "3", 1);
    if (cold) ::setenv("GTA6RT_METAL_PCACHE_PREBUILD", "cold", 1);
    auto device = gta6rt::metal::create_device({});
    std::string log;
    CHECK(pc::open_archive(*device, dir.string(), "pcache runtime test", true, &log) == gpu::Status::ok);
    gpu::RootLayoutDesc rd;
    for (std::uint32_t b = 0; b < 29; ++b)
        rd.tables.push_back({b, b == 1 ? gpu::DescriptorKind::sampler : b == 2 ? gpu::DescriptorKind::buffer
                                                                                : gpu::DescriptorKind::texture});
    auto root = device->create_root_layout(rd);

    std::map<std::string, std::unique_ptr<gpu::ShaderLibrary>> libs;
    const pc::ContentKeyFn content = [&](const std::string& n) {
        const auto* e = catalog.find(n);
        return e ? e->key : std::string("-");
    };
    const pc::LibraryFn library = [&](const std::string& name, const char*, std::function<void()>) {
        pc::Library out;
        const auto* e = catalog.find(name);
        if (!e) {
            out.error = "not in the store";
            return out;
        }
        auto& lib = libs[name];
        if (!lib) {
            std::ifstream in(catalog.library_path(*e), std::ios::binary);
            std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            lib = device->create_library_metallib(bytes, &out.error);
        }
        out.library = lib.get();
        out.entry = "main0";
        return out;
    };

    gpu::RenderPipelineDesc blit;
    blit.rt_count = 1;
    blit.rt_formats[0] = 28;
    blit.inputs = {{0, 6, 0, 0, false, 1}, {1, 2, 0, 12, false, 1}, {2, 16, 0, 28, false, 1}};
    gpu::RenderPipelineDesc blit_bgra = blit;
    blit_bgra.rt_formats[0] = 87;
    gpu::RenderPipelineDesc blit_rgba16 = blit;
    blit_rgba16.rt_formats[0] = 10;
    gpu::RenderPipelineDesc blit_tier = blit;
    blit_tier.rt_formats[0] = 24;
    gpu::ComputePipelineDesc comp;
    comp.threads = {8, 8, 1};

    {
        pc::Manifest m;
        for (const auto* d : {&blit, &blit_bgra}) {
            pc::Entry e;
            e.text = pc::encode(pc::spec_of(*d, vs, ps, content));
            e.key = pc::key_of(e.text);
            e.phase = pc::Phase::boot;
            e.t = d == &blit ? 1.0 : 2.0;
            m.note(e);
        }
        CHECK(m.save((dir / "pcache-manifest.txt").string()));
        std::ofstream(dir / "pcache-bootstrap.txt") << "# test\nL\tlanding\t" << cs << "\n";
        pc::Manifest t;
        pc::Spec stale = pc::spec_of(blit_tier, vs, ps, content);
        stale.content[1] = "0123456789abcdef";
        pc::Entry e;
        e.text = pc::encode(stale);
        e.key = pc::key_of(e.text);
        e.phase = pc::Phase::boot;
        e.t = 1.5;
        t.note(e);
        gpu::RenderPipelineDesc blit_late = blit;
        blit_late.rt_formats[0] = 2;
        pc::Entry late;
        late.text = pc::encode(pc::spec_of(blit_late, vs, ps, content));
        late.key = pc::key_of(late.text);
        late.phase = pc::Phase::boot;
        late.t = 1000.0;
        t.note(late);
        CHECK(t.save((tier / "pcache-tier.txt").string()));
    }

    pc::StartConfig cfg;
    cfg.device = device.get();
    cfg.root = root.get();
    cfg.libraries = library;
    cfg.content_key = content;
    cfg.directory = dir.string();
    pc::start(cfg);

    const auto wait_for = [](auto pred, double seconds) {
        const auto t0 = std::chrono::steady_clock::now();
        while (!pred()) {
            if (std::chrono::steady_clock::now() - t0 > std::chrono::duration<double>(seconds)) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    };
    int owner = 0;
    const auto refs = [&](auto& d) {
        d.vs = {library(vs, "vs", {}).library, "main0"};
        d.ps = {library(ps, "ps", {}).library, "main0"};
        d.root = root.get();
    };
    if (cold) {
        CHECK(wait_for([] { return pc::counters().prebuilt == 1; }, 60));
        std::this_thread::sleep_for(std::chrono::seconds(2));
        CHECK(pc::counters().prebuilt == 1);
        CHECK(pc::report().find("3 at first use only") != std::string::npos);
        gpu::RenderPipelineDesc demand = blit;
        refs(demand);
        demand.mode = gpu::CompileMode::async;
        demand.not_ready = gpu::NotReady::skip;
        auto made = pc::make_render(*device, demand, &owner, vs, ps);
        CHECK(made != nullptr);
        CHECK(pc::counters().created == 1 && pc::counters().adopted_ready == 0);
        CHECK(wait_for([&] { return made->state() == gpu::PipelineState::ready; }, 60));
        gpu::RenderPipelineDesc from_tier = blit_tier;
        refs(from_tier);
        from_tier.mode = gpu::CompileMode::async;
        from_tier.not_ready = gpu::NotReady::skip;
        auto tiered = pc::make_render(*device, from_tier, &owner, vs, ps);
        CHECK(tiered && tiered->state() == gpu::PipelineState::ready);
        CHECK(pc::counters().adopted_ready == 1);
        pc::flush("test");
        pc::forget(&owner);
        std::printf("%s\n", pc::report().c_str());
        pc::stop();
        std::filesystem::remove_all(tier);
        std::printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
        return failures ? 1 : 0;
    }
    CHECK(wait_for([] { return pc::counters().prebuilt == 3; }, 60));

    gpu::RenderPipelineDesc demand = blit;
    refs(demand);
    demand.mode = gpu::CompileMode::async;
    demand.not_ready = gpu::NotReady::skip;
    demand.label = "first use";
    auto adopted = pc::make_render(*device, demand, &owner, vs, ps);
    CHECK(adopted && adopted->state() == gpu::PipelineState::ready);
    CHECK(pc::counters().adopted_ready == 1);

    gpu::RenderPipelineDesc from_tier = blit_tier;
    refs(from_tier);
    from_tier.mode = gpu::CompileMode::async;
    from_tier.not_ready = gpu::NotReady::skip;
    auto tiered = pc::make_render(*device, from_tier, &owner, vs, ps);
    CHECK(tiered && tiered->state() == gpu::PipelineState::ready);
    CHECK(pc::counters().adopted_ready == 2);

    gpu::RenderPipelineDesc demand2 = blit_rgba16;
    demand2.mode = gpu::CompileMode::async;
    demand2.not_ready = gpu::NotReady::skip;
    refs(demand2);
    auto fresh = pc::make_render(*device, demand2, &owner, vs, ps);
    CHECK(fresh != nullptr);
    CHECK(pc::counters().created == 1);
    CHECK(wait_for([] { return pc::counters().waits >= 1; }, 60));

    pc::created(&owner, "", cs, comp, cs);
    CHECK(pc::counters().bootstrap_matched == 1);
    CHECK(wait_for([] { return pc::counters().prebuilt == 4; }, 120));
    gpu::ComputePipelineDesc demand3 = comp;
    demand3.mode = gpu::CompileMode::async;
    demand3.cs = {library(cs, "cs", {}).library, "main0"};
    demand3.root = root.get();
    auto compute = pc::make_compute(*device, demand3, &owner, cs);
    CHECK(compute && compute->state() == gpu::PipelineState::ready);
    CHECK(pc::counters().adopted_ready == 3);
    CHECK(wait_for([] { return pc::report().find("1 released unused") != std::string::npos; }, 10));

    pc::flush("test");
    pc::forget(&owner);
    std::printf("%s\n", pc::report().c_str());
    CHECK(pc::report().find(", 1 queued,") != std::string::npos);
    pc::Manifest back;
    std::string why;
    CHECK(back.load((dir / "pcache-manifest.txt").string(), &why));
    CHECK(back.size() == 5);
    std::size_t twice = 0;
    for (const auto& e : back.ordered()) twice += e.runs == 2;
    CHECK(twice == 1);
    std::size_t run_files = 0;
    for (const auto& f : std::filesystem::directory_iterator(dir / "pcache-runs")) run_files += f.is_regular_file();
    CHECK(run_files == 1);
    pc::stop();
    std::filesystem::remove_all(tier);
    CHECK(device->flush_pipeline_cache(nullptr) == gpu::Status::ok);
    std::printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
