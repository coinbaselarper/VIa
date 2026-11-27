#include "msl_rewrite.h"

#include <cctype>
#include <regex>
#include <set>
#include <sstream>

namespace gta6rt::msl {

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\n\r");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\n\r");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> split_top_level(const std::string& s) {
    std::vector<std::string> out;
    int depth = 0;
    std::string cur;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char ch = s[i];
        if (ch == '(' || ch == '<' || ch == '[') ++depth;
        if (ch == ')' || ch == '>' || ch == ']') --depth;
        if (ch == ',' && depth == 0) {
            out.push_back(trim(cur));
            cur.clear();
            continue;
        }
        cur += ch;
    }
    if (!trim(cur).empty()) out.push_back(trim(cur));
    return out;
}

bool parse_param(const std::string& p, Param& out) {
    const auto at = p.find("[[");
    std::string decl = trim(at == std::string::npos ? p : p.substr(0, at));
    if (at != std::string::npos) {
        const auto end = p.find("]]", at);
        if (end == std::string::npos) return false;
        out.attr = trim(p.substr(at + 2, end - at - 2));
    }
    std::size_t e = decl.size();
    while (e > 0 && (std::isalnum(static_cast<unsigned char>(decl[e - 1])) || decl[e - 1] == '_')) --e;
    out.name = decl.substr(e);
    out.decl = decl;
    return !out.name.empty();
}

bool struct_members(const std::string& src, const std::string& name, std::vector<Member>& out, std::string& err,
                    bool allow_arrays) {
    const std::regex head("(^|\\n)struct " + name + "\\s*\\n\\{\\n");
    std::smatch m;
    if (!std::regex_search(src, m, head)) {
        err = "struct " + name + " not found";
        return false;
    }
    const std::size_t begin = std::size_t(m.position(0) + m.length(0));
    const std::size_t end = src.find("\n};", begin);
    if (end == std::string::npos) {
        err = "struct " + name + " not closed";
        return false;
    }
    std::istringstream body(src.substr(begin, end - begin));
    std::string line;
    const std::regex row("^\\s*([A-Za-z_][\\w:<>, ]*?)\\s+(\\w+)\\s*(\\[\\[([^\\]]*)\\]\\])?\\s*(\\[[^\\]]*\\])?\\s*;\\s*$");
    while (std::getline(body, line)) {
        if (trim(line).empty() || trim(line).rfind("//", 0) == 0) continue;
        std::smatch r;
        if (!std::regex_match(line, r, row)) {
            err = "struct " + name + ": cannot parse '" + trim(line) + "'";
            return false;
        }
        if (r[5].matched && !allow_arrays) {
            err = "struct " + name + ": array member '" + r[2].str() + "' not supported";
            return false;
        }
        out.push_back({trim(r[1].str()), r[2].str(), r[4].matched ? trim(r[4].str()) : "", r[5].matched ? r[5].str() : ""});
    }
    return true;
}

int vector_size(const std::string& type) {
    if (type.empty()) return 0;
    const char last = type.back();
    if (last >= '2' && last <= '4') return last - '0';
    return 1;
}

std::string base_type(const std::string& type) {
    std::string t = type;
    while (!t.empty() && t.back() >= '2' && t.back() <= '4') t.pop_back();
    return t;
}

bool find_entry(const std::string& msl, const std::string& qualifier, const std::string& entry, Entry& out,
                std::string& err) {
    std::string q;
    for (char c : qualifier) {
        if (c == '[' || c == ']') q += '\\';
        q += c;
    }
    const std::regex sig("(^|\\n)([^\\n]*?)" + q + "\\s+([\\w:<>]+)\\s+" + entry + "\\s*\\(");
    std::smatch m;
    if (!std::regex_search(msl, m, sig)) {
        err = qualifier + " entry '" + entry + "' not found";
        return false;
    }
    out.out_type = m[3].str();
    out.sig_begin = std::size_t(m.position(2));
    out.open = std::size_t(m.position(0) + m.length(0)) - 1;
    int depth = 0;
    std::size_t close = out.open;
    for (; close < msl.size(); ++close) {
        if (msl[close] == '(') ++depth;
        if (msl[close] == ')' && --depth == 0) break;
    }
    if (close >= msl.size()) {
        err = "unbalanced entry signature";
        return false;
    }
    out.close = close;
    for (const std::string& p : split_top_level(msl.substr(out.open + 1, close - out.open - 1))) {
        Param prm;
        if (!parse_param(p, prm)) {
            err = "cannot parse parameter '" + p + "'";
            return false;
        }
        out.params.push_back(prm);
    }
    return true;
}

std::optional<std::array<std::uint32_t, 3>> workgroup_size(const std::string& msl) {
    const std::regex re("gl_WorkGroupSize\\s*(\\[\\[maybe_unused\\]\\])?\\s*=\\s*uint3\\(\\s*(\\d+)u?\\s*,\\s*(\\d+)u?\\s*,\\s*(\\d+)u?\\s*\\)");
    std::smatch m;
    if (!std::regex_search(msl, m, re)) return std::nullopt;
    return std::array<std::uint32_t, 3>{std::uint32_t(std::stoul(m[2].str())), std::uint32_t(std::stoul(m[3].str())),
                                        std::uint32_t(std::stoul(m[4].str()))};
}

namespace {

std::uint32_t member_scalars(const Member& m) {
    if (m.attr == "primitive_culled") return 0;
    std::uint32_t n = std::uint32_t(vector_size(m.type));
    if (!m.array.empty()) n *= std::uint32_t(std::stoul(m.array.substr(1, m.array.size() - 2)));
    return n;
}

bool is_user_clip(const Member& m) { return m.attr.rfind("user(clip", 0) == 0; }

}  // namespace

std::uint32_t mesh_unique_scalars(const std::string& msl, const std::string& vertex_struct) {
    std::vector<Member> members;
    std::string err;
    if (!struct_members(msl, vertex_struct, members, err, true)) return 0;
    std::uint32_t n = 0, clips = 0;
    for (const Member& m : members) {
        if (is_user_clip(m)) {
            ++clips;
            continue;
        }
        n += member_scalars(m);
    }
    return n + (clips ? 1 : 0);
}

gpu::MeshEstimate estimate_mesh(const std::string& msl, const std::string& entry) {
    gpu::MeshEstimate e;
    const std::regex decl("mesh<\\s*(\\w+)\\s*,\\s*(\\w+)\\s*,\\s*(\\d+)\\s*,\\s*(\\d+)\\s*,\\s*topology::(\\w+)\\s*>");
    std::smatch m;
    if (!std::regex_search(msl, m, decl)) {
        e.error = "no mesh<V, P, nv, np, topology> declaration";
        return e;
    }
    e.max_vertices = std::uint32_t(std::stoul(m[3].str()));
    e.max_primitives = std::uint32_t(std::stoul(m[4].str()));
    std::vector<Member> vm, pm;
    if (!struct_members(msl, m[1].str(), vm, e.error, true)) return e;
    if (!struct_members(msl, m[2].str(), pm, e.error, true)) return e;
    for (const Member& x : vm) e.vertex_scalars += member_scalars(x);
    for (const Member& x : pm) e.primitive_scalars += member_scalars(x);
    e.mesh_bytes = std::uint64_t(e.max_vertices) * 16ull * ((e.vertex_scalars + 3) / 4);
    e.parsed = true;
    (void)entry;
    return e;
}

namespace {

const char* kD24Helpers = R"MSL(
struct gta6rt_D24Params
{
    float constant_bias;
    float slope;
    float clamp;
    uint flags;
};

static inline float gta6rt_d24_quantize(float v)
{
    if (!(v > 0.0f)) return 0.0f;
    if (v >= 1.0f) return 1.0f;
    const float c = rint(v * 16777215.0f);
    const float q = c * (1.0f / 16777215.0f);
    const float r = fma(-q, 16777215.0f, c);
    return fma(r, 1.0f / 16777215.0f, q);
}

static inline float gta6rt_d24_biased(float z, float slope, constant gta6rt_D24Params& p)
{
    if ((p.flags & 1u) == 0u) return z;
    float b = p.constant_bias + p.slope * slope;
    if (p.clamp > 0.0f) b = min(b, p.clamp);
    else if (p.clamp < 0.0f) b = max(b, p.clamp);
    return z + b;
}
)MSL";

}  // namespace

std::string d24_probe_source() {
    return std::string("#include <metal_stdlib>\nusing namespace metal;\n") + kD24Helpers +
           "\nstruct gta6rt_ProbePush\n{\n    uint user[32];\n    uint heap_lo;\n    uint heap_hi;\n    uint code_lo;\n"
           "    uint code_hi;\n};\n\n"
           "kernel void gta6rt_d24_probe(constant gta6rt_ProbePush& push [[buffer(0)]], uint i [[thread_position_in_grid]])\n"
           "{\n    const ulong base = (ulong(push.heap_hi) << 32) | ulong(push.heap_lo);\n"
           "    if (i >= push.user[2]) return;\n"
           "    device const float* in = (device const float*)(base + ulong(push.user[0]));\n"
           "    device float* out = (device float*)(base + ulong(push.user[1]));\n"
           "    out[i] = gta6rt_d24_quantize(in[i]);\n}\n";
}

D24Wrapper d24_fragment_wrapper(const std::string& msl, const std::string& entry) {
    D24Wrapper w;
    if (msl.empty()) {
        std::ostringstream s;
        s << "#include <metal_stdlib>\nusing namespace metal;\n" << kD24Helpers
          << "\nstruct gta6rt_d24_only_out\n{\n    float gta6rt_depth [[depth(any)]];\n};\n\n"
             "fragment gta6rt_d24_only_out gta6rt_d24_only(float4 gta6rt_fc [[position]], "
             "constant gta6rt_D24Params& gta6rt_d24 [[buffer(0)]])\n{\n"
             "    const float gta6rt_slope = max(abs(dfdx(gta6rt_fc.z)), abs(dfdy(gta6rt_fc.z)));\n"
             "    gta6rt_d24_only_out r;\n"
             "    r.gta6rt_depth = gta6rt_d24_quantize(gta6rt_d24_biased(gta6rt_fc.z, gta6rt_slope, gta6rt_d24));\n"
             "    return r;\n}\n";
        w.source = s.str();
        w.entry = "gta6rt_d24_only";
        w.params_index = 0;
        w.ok = true;
        return w;
    }
    Entry e;
    if (!find_entry(msl, "fragment", entry, e, w.error)) return w;
    const std::string prefix = msl.substr(e.sig_begin, e.open - e.sig_begin);
    if (prefix.find("early_fragment_tests") != std::string::npos) {
        w.error = "early_fragment_tests: a depth write would be ignored";
        return w;
    }
    std::set<std::uint32_t> used;
    const std::regex buf("buffer\\((\\d+)\\)");
    std::string fc;
    for (const Param& p : e.params) {
        std::smatch b;
        if (std::regex_search(p.attr, b, buf)) used.insert(std::uint32_t(std::stoul(b[1].str())));
        if (p.attr == "position") fc = p.name;
    }
    std::uint32_t k = 0;
    while (k < 31 && used.count(k)) ++k;
    if (k > 30) {
        w.error = "no free buffer index for the D24 parameters";
        return w;
    }
    w.params_index = k;
    const bool has_out = e.out_type != "void";
    std::vector<Member> outs;
    std::string depth_member;
    if (has_out) {
        if (!struct_members(msl, e.out_type, outs, w.error, true)) return w;
        for (const Member& m : outs)
            if (m.attr.rfind("depth(", 0) == 0) depth_member = m.name;
    }
    w.shader_depth = !depth_member.empty();

    const std::string body = entry + "_d24_body";
    const std::string out_struct = entry + "_d24_out";
    std::string decls, args, sig;
    for (const Param& p : e.params) {
        if (!decls.empty()) decls += ", ";
        decls += p.decl;
        if (!args.empty()) args += ", ";
        args += p.name;
        if (!sig.empty()) sig += ", ";
        sig += p.decl + (p.attr.empty() ? "" : " [[" + p.attr + "]]");
    }
    if (fc.empty()) {
        fc = "gta6rt_fc";
        sig += std::string(sig.empty() ? "" : ", ") + "float4 gta6rt_fc [[position]]";
    }
    sig += ", constant gta6rt_D24Params& gta6rt_d24 [[buffer(" + std::to_string(k) + ")]]";

    std::ostringstream s;
    s << msl.substr(0, e.sig_begin);
    s << "static inline __attribute__((always_inline)) " << e.out_type << " " << body << "(" << decls << ")";
    s << msl.substr(e.close + 1);
    s << "\n" << kD24Helpers << "\nstruct " << out_struct << "\n{\n";
    for (const Member& m : outs) {
        if (m.name == depth_member) continue;
        s << "    " << m.type << " " << m.name << (m.attr.empty() ? "" : " [[" + m.attr + "]]") << m.array << ";\n";
    }
    s << "    float gta6rt_depth [[depth(any)]];\n};\n\n";
    s << "fragment " << out_struct << " " << entry << "_d24(" << sig << ")\n{\n";
    s << "    const float gta6rt_z = " << fc << ".z;\n";
    s << "    const float gta6rt_slope = max(abs(dfdx(gta6rt_z)), abs(dfdy(gta6rt_z)));\n";
    if (has_out) s << "    " << e.out_type << " gta6rt_o = " << body << "(" << args << ");\n";
    else s << "    " << body << "(" << args << ");\n";
    s << "    " << out_struct << " gta6rt_r;\n";
    for (const Member& m : outs) {
        if (m.name == depth_member) continue;
        if (m.array.empty()) {
            s << "    gta6rt_r." << m.name << " = gta6rt_o." << m.name << ";\n";
        } else {
            s << "    for (uint gta6rt_i = 0; gta6rt_i < " << m.array.substr(1, m.array.size() - 2)
              << "; ++gta6rt_i) gta6rt_r." << m.name << "[gta6rt_i] = gta6rt_o." << m.name << "[gta6rt_i];\n";
        }
    }
    if (w.shader_depth)
        s << "    gta6rt_r.gta6rt_depth = gta6rt_d24_quantize(gta6rt_o." << depth_member << ");\n";
    else
        s << "    gta6rt_r.gta6rt_depth = gta6rt_d24_quantize(gta6rt_d24_biased(gta6rt_z, gta6rt_slope, gta6rt_d24));\n";
    s << "    return gta6rt_r;\n}\n";
    w.source = s.str();
    w.entry = entry + "_d24";
    w.ok = true;
    return w;
}

}  // namespace gta6rt::msl
