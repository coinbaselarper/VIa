#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "gta6rt_metal/gpu.h"
#include "gta6rt_metal/metal_backend.h"
#include "gta6rt_shaderc_msl/catalog.h"

using namespace gta6rt::gpu;
namespace sc = gta6rt::shaderc_msl;

namespace {

std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::string slurp(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

double since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

Blend blend_of(int v) { return static_cast<Blend>(v); }

std::uint32_t input_format(const std::string &type, int count) {
    if (type == "uint" || type == "int") return count == 1 ? format::r32_uint : format::r32g32b32a32_uint;
    switch (count) {
    case 1: return format::r32_float;
    case 2: return format::r32g32_float;
    case 3: return format::r32g32b32_float;
    default: return format::r32g32b32a32_float;
    }
}

}  // namespace

int main(int argc, char **argv) {
    std::string manifest, list, cache_dir, report, version;
    bool verify = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); } return argv[++i]; };
        if (a == "--manifest") manifest = next();
        else if (a == "--list") list = next();
        else if (a == "--cache-dir") cache_dir = next();
        else if (a == "--report") report = next();
        else if (a == "--translator-version") version = next();
        else if (a == "--verify") verify = true;
        else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    if (manifest.empty() || list.empty() || cache_dir.empty() || version.empty()) {
        std::fprintf(stderr, "usage: shaderc_msl_warmup --manifest M --list L --cache-dir D --translator-version V [--verify] [--report R]\n");
        return 2;
    }
    sc::Catalog cat;
    std::string err;
    if (!cat.load(manifest, &err)) { std::fprintf(stderr, "catalog: %s\n", err.c_str()); return 1; }
    auto dev = gta6rt::metal::create_device();
    if (!dev) { std::fprintf(stderr, "no device\n"); return 1; }
    PipelineCacheDesc cd;
    cd.directory = cache_dir.c_str();
    cd.translator_version = version;
    cd.load = true;
    if (dev->open_pipeline_cache(cd, &err) != Status::ok) { std::fprintf(stderr, "open cache: %s\n", err.c_str()); return 1; }
    const PipelineCacheStats st0 = dev->pipeline_cache_stats();
    std::printf("cache %s loaded=%d key=%s bytes=%llu\n", st0.path.c_str(), st0.loaded, st0.key.c_str(), (unsigned long long)st0.file_bytes);
    RootLayoutDesc rd;
    for (std::uint32_t b = 0; b <= 28; ++b)
        rd.tables.push_back({b, b == 1 ? DescriptorKind::sampler : b == 2 ? DescriptorKind::buffer : DescriptorKind::texture});
    auto root = dev->create_root_layout(rd);

    std::map<std::string, std::unique_ptr<ShaderLibrary>> libs;
    auto lib_of = [&](const std::string &name) -> ShaderLibrary * {
        const sc::ShaderEntry *e = cat.find(name);
        if (!e) return nullptr;
        auto it = libs.find(e->key);
        if (it != libs.end()) return it->second.get();
        const std::string bytes = slurp(cat.library_path(*e));
        auto l = dev->create_library_metallib({reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()}, &err);
        ShaderLibrary *raw = l.get();
        libs[e->key] = std::move(l);
        return raw;
    };

    std::ofstream rep;
    if (!report.empty()) rep.open(report);
    std::vector<std::unique_ptr<Pipeline>> keep;
    int n = 0, ok = 0, hits = 0, missing = 0, failed = 0;
    double total = 0;
    std::ifstream lf(list);
    for (std::string line; std::getline(lf, line);) {
        if (line.empty() || line[0] == '#') continue;
        auto c = split(line, '\t');
        const bool gfx = c[0] == "G";
        std::unique_ptr<Pipeline> pso;
        std::string label = gfx ? c[2] + " + " + c[3] : c[2];
        const auto t0 = std::chrono::steady_clock::now();
        if (gfx) {
            ShaderLibrary *vl = lib_of(c[2]), *pl = lib_of(c[3]);
            if (!vl || !pl) { missing++; n++; if (rep) rep << "missing\t" << label << "\n"; continue; }
            RenderPipelineDesc d;
            d.vs = {vl, "main0"};
            d.ps = {pl, "main0"};
            d.topology = static_cast<TopologyType>(std::stoi(c[4]));
            d.rt_count = std::uint32_t(std::stoi(c[5]));
            d.rt_formats[0] = std::uint32_t(std::stoi(c[6]));
            d.rt_formats[1] = std::uint32_t(std::stoi(c[7]));
            for (std::uint32_t r = 0; r < d.rt_count && r < 8; ++r)
                if (d.rt_formats[r] == 0) d.rt_formats[r] = d.rt_formats[0];
            d.depth_format = std::uint32_t(std::stoi(c[8]));
            d.samples = std::uint32_t(std::stoi(c[9]));
            auto bl = split(c[10], '/');
            if (bl.size() == 3) {
                TargetBlend tb;
                tb.enable = bl[0] == "1";
                tb.src = tb.src_alpha = blend_of(std::stoi(bl[1]));
                tb.dst = tb.dst_alpha = blend_of(std::stoi(bl[2]));
                for (std::uint32_t r = 0; r < d.rt_count && r < 8; ++r) d.blend[r] = tb;
            }
            if (c.size() > 11 && c[11] != "-") {
                std::uint32_t off = 0;
                for (auto &t : split(c[11], ';')) {
                    auto f = split(t, ':');
                    int count = std::stoi(f[1]);
                    d.inputs.push_back({std::uint32_t(std::stoi(f[0])), input_format(f[2], count), 0, off});
                    off += 4 * count;
                }
            }
            d.root = root.get();
            d.mode = CompileMode::eager;
            d.priority = Priority::first_scene;
            d.label = "warmup";
            pso = dev->create_render_pipeline(d);
        } else {
            ShaderLibrary *cl = lib_of(c[2]);
            if (!cl) { missing++; n++; if (rep) rep << "missing\t" << label << "\n"; continue; }
            ComputePipelineDesc d;
            d.cs = {cl, "main0"};
            if (c.size() >= 6) d.threads = {std::uint32_t(std::stoi(c[3])), std::uint32_t(std::stoi(c[4])), std::uint32_t(std::stoi(c[5]))};
            d.root = root.get();
            d.mode = CompileMode::eager;
            d.priority = Priority::first_scene;
            d.label = "warmup";
            pso = dev->create_compute_pipeline(d);
        }
        const double dt = since(t0);
        total += dt;
        n++;
        const bool good = pso->status() == Status::ok;
        ok += good;
        failed += !good;
        hits += good && pso->from_archive();
        if (rep) rep << (good ? "ok" : "fail") << "\t" << c[1] << "\t" << label << "\t" << dt << "\t" << (good && pso->from_archive()) << "\t" << (good ? "" : pso->error()) << "\n";
        keep.push_back(std::move(pso));
    }
    if (dev->flush_pipeline_cache(&err) != Status::ok) { std::fprintf(stderr, "flush: %s\n", err.c_str()); return 1; }
    const PipelineCacheStats st = dev->pipeline_cache_stats();
    std::printf("pipelines %d ok %d failed %d missing %d from_archive %d, %.2f s creating; archive hits %llu misses %llu added %llu add_failures %llu file %llu B serialize %.3f s load %.3f s\n",
                n, ok, failed, missing, hits, total, (unsigned long long)st.hits, (unsigned long long)st.misses, (unsigned long long)st.added,
                (unsigned long long)st.add_failures, (unsigned long long)st.file_bytes, st.serialize_seconds, st.load_seconds);
    if (verify && (hits != ok)) { std::fprintf(stderr, "verify: only %d of %d pipelines came from the archive\n", hits, ok); return 3; }
    return failed ? 4 : 0;
}
