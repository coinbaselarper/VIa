#include "test_util.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace gta6rt::gpu;

namespace {

struct Input {
    std::uint32_t reg = 0, count = 0;
    std::string type, semantic;
};

struct Shader {
    std::string stem, stage, base, split, s52, moved, metallib;
    std::uint32_t push = 0;
    std::set<std::uint32_t> bindings;
    std::vector<Input> inputs;
    std::vector<std::pair<std::uint32_t, std::string>> colors;
    bool depth = false;
};

struct Pair {
    std::string label, vs, ps;
};

std::uint32_t input_format(const Input& in) {
    if (in.type == "uint" || in.type == "int") return in.count == 1 ? format::r32_uint : format::r32g32b32a32_uint;
    switch (in.count) {
    case 1: return format::r32_float;
    case 2: return format::r32g32_float;
    case 3: return format::r32g32b32_float;
    default: return format::r32g32b32a32_float;
    }
}

std::uint32_t color_format(const std::string& t) {
    if (t.rfind("uint", 0) == 0) return format::r8g8b8a8_uint;
    return format::r8g8b8a8_unorm;
}

const char* what_name(BindingPlanEntry::What w) {
    switch (w) {
    case BindingPlanEntry::What::push_block: return "push_block";
    case BindingPlanEntry::What::table: return "table";
    case BindingPlanEntry::What::vertex_buffer: return "vertex_buffer";
    case BindingPlanEntry::What::rect_params: return "rect_params";
    default: return "UNMAPPED";
    }
}

void report_reflection(std::ostream& o, const PipelineReflection& r) {
    auto stage = [&](const char* name, const std::vector<ReflectedBinding>& bs) {
        for (const auto& b : bs) {
            o << "    " << name << " [[buffer/texture(" << b.index << ")]] " << b.name << ": "
              << (b.kind == BindingKind::buffer ? "buffer" : b.kind == BindingKind::texture ? "texture" : "other")
              << ", " << (b.data_size == ~0ull ? std::string("dynamic stride") : std::to_string(b.data_size) + " B") << (b.argument_buffer ? ", argument buffer" : "") << ", " << b.access
              << (b.used ? "" : ", unused") << "\n";
            for (const auto& m : b.members)
                o << "        member " << m.name << " @" << m.offset << " " << m.type
                  << (m.array_length ? "[" + std::to_string(m.array_length) + "]" : "") << "\n";
        }
    };
    stage("vertex  ", r.vertex);
    stage("fragment", r.fragment);
    for (const auto& a : r.attributes)
        o << "    attribute " << a.location << " " << a.name << " " << a.type << (a.active ? "" : " (inactive)") << "\n";
    for (const auto& e : r.plan)
        o << "    plan: " << (e.stage ? "fragment" : "vertex") << " buffer(" << e.index << ") -> " << what_name(e.what)
          << (e.what == BindingPlanEntry::What::table ? " binding " + std::to_string(e.binding) : "") << "  [" << e.note
          << "]\n";
    for (const auto& m : r.mismatches) o << "    MISMATCH: " << m << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: metal_test_game_shaders <route_a/manifest.txt>\n");
        return 2;
    }
    std::ifstream mf(argv[1]);
    if (!mf) {
        std::printf("SKIP: no manifest at %s (GTA6RT_METAL_SPIRV_DIR or spirv-cross missing at configure time)\n", argv[1]);
        return 77;
    }
    std::vector<Pair> pairs;
    std::map<std::string, Shader> shaders;
    std::string line;
    while (std::getline(mf, line)) {
        std::istringstream s(line);
        std::string kind;
        s >> kind;
        if (kind == "pair") {
            Pair p;
            s >> p.label >> p.vs >> p.ps;
            pairs.push_back(p);
        } else if (kind == "shader") {
            Shader sh;
            std::string bindings;
            s >> sh.stem >> sh.stage >> sh.base >> sh.split >> sh.push >> bindings >> sh.s52 >> sh.moved >> sh.metallib;
            if (bindings != "-") {
                std::istringstream b(bindings);
                std::string x;
                while (std::getline(b, x, ',')) sh.bindings.insert(std::uint32_t(std::stoul(x)));
            }
            shaders[sh.stem] = sh;
        } else if (kind == "input") {
            std::string stem;
            Input in;
            s >> stem >> in.reg >> in.type >> in.count >> in.semantic;
            shaders[stem].inputs.push_back(in);
        } else if (kind == "color") {
            std::string stem, type;
            std::uint32_t idx;
            s >> stem >> idx >> type;
            shaders[stem].colors.emplace_back(idx, type);
        } else if (kind == "depth") {
            std::string stem;
            s >> stem;
            shaders[stem].depth = true;
        }
    }
    auto dev = gta6rt::metal::create_device();
    if (!dev) return 1;
    test::print_caps(dev->caps());

    RootLayoutDesc base_rd, split_rd;
    for (std::uint32_t b = 0; b <= 28; ++b) {
        const DescriptorKind k = b == 1 ? DescriptorKind::sampler : b == 2 ? DescriptorKind::buffer : DescriptorKind::texture;
        base_rd.tables.push_back({b, k});
        split_rd.tables.push_back({b, k});
    }
    base_rd.split_sets = false;
    auto base_root = dev->create_root_layout(base_rd);
    auto split_root = dev->create_root_layout(split_rd);

    const std::string report_path = std::string(argv[1]).substr(0, std::string(argv[1]).rfind('/')) + "/report.txt";
    std::ostringstream rep;
    int built = 0, attempted = 0;
    for (const Pair& p : pairs) {
        const Shader& vs = shaders[p.vs];
        const Shader& ps = shaders[p.ps];
        rep << "== pair " << p.label << ": " << p.vs << " + " << p.ps << "\n";
        rep << "   MSL identical to spike/s5.2 step 1 output: vs " << vs.s52 << ", ps " << ps.s52 << "\n";
        std::string ps_bind;
        for (auto b : ps.bindings) ps_bind += std::to_string(b) + " ";
        rep << "   contract bindings the PS reads (index.json): " << ps_bind << "\n";
        for (const char* variant : {"base", "split", "split_metallib"}) {
            const bool split = std::string(variant) != "base";
            const bool offline = std::string(variant) == "split_metallib";
            if (offline && (vs.metallib.empty() || vs.metallib == "-")) continue;
            std::string e1, e2;
            MslOptions mo;
            std::unique_ptr<ShaderLibrary> vl, pl;
            if (offline) {
                const std::string vb = test::read_file(vs.metallib), pb = test::read_file(ps.metallib);
                vl = dev->create_library_metallib({reinterpret_cast<const std::uint8_t*>(vb.data()), vb.size()}, &e1);
                pl = dev->create_library_metallib({reinterpret_cast<const std::uint8_t*>(pb.data()), pb.size()}, &e2);
            } else {
                vl = dev->create_library_msl(test::read_file(split ? vs.split : vs.base), mo, &e1);
                pl = dev->create_library_msl(test::read_file(split ? ps.split : ps.base), mo, &e2);
            }
            CHECK(vl && pl, "%s %s MSL compile: %s %s", p.label.c_str(), variant, e1.c_str(), e2.c_str());
            if (!vl || !pl) {
                rep << "  [" << variant << "] MSL compile failed: " << e1 << " " << e2 << "\n";
                continue;
            }
            RenderPipelineDesc pd;
            pd.vs = {vl.get(), "main0"};
            pd.ps = {pl.get(), "main0"};
            std::uint32_t offset = 0;
            for (const Input& in : vs.inputs) {
                pd.inputs.push_back({in.reg, input_format(in), 0, offset});
                offset += 4 * in.count;
            }
            for (const auto& [idx, type] : ps.colors) {
                pd.rt_formats[idx] = color_format(type);
                pd.rt_count = std::max(pd.rt_count, idx + 1);
            }
            if (ps.depth) pd.depth_format = format::d32_float;
            pd.root = split ? split_root.get() : base_root.get();
            pd.mode = CompileMode::async;
            auto pso = dev->create_render_pipeline(pd);
            ++attempted;
            const Status st = pso->wait();
            rep << "  [" << variant << "] " << (offline ? "metallib load" : "MSL compile") << " vs " << vl->compile_seconds() << " s, ps " << pl->compile_seconds()
                << " s; pipeline " << (st == Status::ok ? "READY" : "FAILED") << " in " << pso->compile_seconds() << " s"
                << (st == Status::ok ? "" : ": " + pso->error()) << "\n";
            CHECK(st == Status::ok, "%s %s pipeline: %s", p.label.c_str(), variant, pso->error().c_str());
            if (st != Status::ok) continue;
            ++built;
            const PipelineReflection& r = pso->reflection();
            report_reflection(rep, r);
            std::set<std::uint32_t> tables;
            bool push_vs = false, push_ps = false;
            for (const auto& e : r.plan) {
                if (e.what == BindingPlanEntry::What::table) tables.insert(e.binding);
                if (e.what == BindingPlanEntry::What::push_block) (e.stage ? push_ps : push_vs) = true;
            }
            CHECK(push_vs && push_ps, "%s %s: push block found in both stages", p.label.c_str(), variant);
            bool attrs_ok = true;
            for (const auto& a : r.attributes) {
                bool found = false;
                for (const auto& in : pd.inputs) found |= in.location == a.location;
                attrs_ok &= found || !a.active;
            }
            CHECK(attrs_ok, "%s %s: every vertex attribute has an input element", p.label.c_str(), variant);
            if (split) {
                CHECK(tables == ps.bindings, "%s split: reflected tables match index.json bindings", p.label.c_str());
                CHECK(r.mismatches.empty(), "%s split: no mismatches (%zu)", p.label.c_str(), r.mismatches.size());
                rep << "   => split: tables " << (tables == ps.bindings ? "MATCH" : "DIFFER") << " the shader's contract "
                    << "bindings; " << r.mismatches.size() << " mismatches\n";
            } else {
                const bool collapsed = std::any_of(r.mismatches.begin(), r.mismatches.end(), [](const std::string& m) {
                    return m.find("set 0 holds") != std::string::npos;
                });
                if (ps.bindings.size() > 1) CHECK(collapsed, "%s base: collapsed set 0 detected", p.label.c_str());
                rep << "   => base: " << (collapsed ? "set 0 collapsed (not bindable as separate tables)" : "no set 0")
                    << "; " << r.mismatches.size() << " mismatches\n";
            }
        }
        if (p.label == "blit") {
            std::string e;
            auto vl = dev->create_library_msl(test::read_file(vs.split), MslOptions{}, &e);
            auto pl = dev->create_library_msl(test::read_file(ps.split), MslOptions{}, &e);
            RenderPipelineDesc pd;
            pd.vs = {vl.get(), "main0"};
            pd.ps = {pl.get(), "main0"};
            std::uint32_t offset = 0;
            for (const Input& in : vs.inputs) {
                pd.inputs.push_back({in.reg, input_format(in), 0, offset});
                offset += 4 * in.count;
            }
            pd.rt_formats[0] = format::r8g8b8a8_unorm;
            pd.rt_count = 1;
            pd.root = split_root.get();
            pd.topology = TopologyType::rectangle;
            pd.rect = RectMode::vertex_expand;
            auto pso = dev->create_render_pipeline(pd);
            const Status st = pso->wait();
            rep << "  [rect] " << p.vs << " as a rectangle-list pipeline: " << (st == Status::ok ? "READY" : "FAILED")
                << ", mode " << (pso->rect_mode() == RectMode::vertex_expand ? "vertex_expand" : "cpu_rewrite") << "\n";
            CHECK(st == Status::ok && pso->rect_mode() == RectMode::vertex_expand,
                  "rect wrapper pipeline for the real VS_Blit (%s)", pso->error().c_str());
        }
    }
    rep << "pipelines built " << built << " of " << attempted << "\n";
    std::ofstream(report_path) << rep.str();
    std::fputs(rep.str().c_str(), stdout);
    std::printf("report: %s\n", report_path.c_str());
    test::print_stats(dev->stats());
    return test::finish("metal_test_game_shaders");
}
