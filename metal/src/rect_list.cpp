#include "gta6rt_metal/rect_list.h"
#include "msl_rewrite.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace gta6rt::rect {

int right_angle(const std::array<std::array<float, 2>, 3>& xy) {
    auto side = [&](int i, int j) {
        const float dx = xy[i][0] - xy[j][0], dy = xy[i][1] - xy[j][1];
        return dx * dx + dy * dy;
    };
    const float d12 = side(1, 2), d20 = side(2, 0), d01 = side(0, 1);
    if (d12 > d20 && d12 > d01) return 0;
    if (d01 > d12 && d01 > d20) return 2;
    return 1;
}

std::array<std::uint8_t, 6> triangle_corners(int corner) {
    switch (corner) {
    case 0: return {0, 1, 3, 0, 3, 2};
    case 2: return {0, 1, 2, 0, 3, 1};
    default: return {0, 1, 2, 0, 2, 3};
    }
}

namespace {

enum class Kind { f32, f16, unorm8, unorm16, snorm8, snorm16 };

bool components(std::uint32_t format, int& n, Kind& kind) {
    switch (format) {
    case 2: n = 4; kind = Kind::f32; return true;
    case 6: n = 3; kind = Kind::f32; return true;
    case 16: n = 2; kind = Kind::f32; return true;
    case 41: n = 1; kind = Kind::f32; return true;
    case 10: n = 4; kind = Kind::f16; return true;
    case 34: n = 2; kind = Kind::f16; return true;
    case 54: n = 1; kind = Kind::f16; return true;
    case 11: n = 4; kind = Kind::unorm16; return true;
    case 35: n = 2; kind = Kind::unorm16; return true;
    case 56: n = 1; kind = Kind::unorm16; return true;
    case 28: case 29: case 87: case 91: n = 4; kind = Kind::unorm8; return true;
    case 49: n = 2; kind = Kind::unorm8; return true;
    case 61: n = 1; kind = Kind::unorm8; return true;
    case 13: n = 4; kind = Kind::snorm16; return true;
    case 37: n = 2; kind = Kind::snorm16; return true;
    case 58: n = 1; kind = Kind::snorm16; return true;
    case 31: n = 4; kind = Kind::snorm8; return true;
    case 51: n = 2; kind = Kind::snorm8; return true;
    case 63: n = 1; kind = Kind::snorm8; return true;
    default: return false;
    }
}

int bytes_of(Kind k) {
    switch (k) {
    case Kind::f32: return 4;
    case Kind::f16: case Kind::unorm16: case Kind::snorm16: return 2;
    default: return 1;
    }
}

float half_to_float(std::uint16_t h) {
    const std::uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1f, man = h & 0x3ff;
    std::uint32_t bits;
    if (exp == 0) {
        if (!man) {
            bits = sign;
        } else {
            int e = -1;
            std::uint32_t m = man;
            do {
                ++e;
                m <<= 1;
            } while (!(m & 0x400));
            bits = sign | ((127 - 15 - e) << 23) | ((m & 0x3ff) << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7f800000u | (man << 13);
    } else {
        bits = sign | ((exp + 112) << 23) | (man << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

std::uint16_t float_to_half(float f) {
    std::uint32_t x;
    std::memcpy(&x, &f, 4);
    const std::uint32_t sign = (x >> 16) & 0x8000u;
    const int exp = int((x >> 23) & 0xff) - 127 + 15;
    std::uint32_t man = x & 0x7fffff;
    if (((x >> 23) & 0xff) == 0xff) return std::uint16_t(sign | 0x7c00u | (man ? 0x200u : 0));
    if (exp >= 31) return std::uint16_t(sign | 0x7c00u);
    if (exp <= 0) {
        if (exp < -10) return std::uint16_t(sign);
        man |= 0x800000;
        const int shift = 14 - exp;
        std::uint32_t h = man >> shift;
        if ((man >> (shift - 1)) & 1) ++h;
        return std::uint16_t(sign | h);
    }
    std::uint32_t h = sign | (std::uint32_t(exp) << 10) | (man >> 13);
    if (man & 0x1000) ++h;
    return std::uint16_t(h);
}

float read_comp(const std::uint8_t* p, Kind k, int c) {
    const std::uint8_t* b = p + c * bytes_of(k);
    switch (k) {
    case Kind::f32: {
        float f;
        std::memcpy(&f, b, 4);
        return f;
    }
    case Kind::f16: {
        std::uint16_t h;
        std::memcpy(&h, b, 2);
        return half_to_float(h);
    }
    case Kind::unorm16: {
        std::uint16_t v;
        std::memcpy(&v, b, 2);
        return float(v) / 65535.0f;
    }
    case Kind::unorm8: return float(b[0]) / 255.0f;
    case Kind::snorm16: {
        std::int16_t v;
        std::memcpy(&v, b, 2);
        return std::max(-1.0f, float(v) / 32767.0f);
    }
    case Kind::snorm8: return std::max(-1.0f, float(std::int8_t(b[0])) / 127.0f);
    }
    return 0;
}

void write_comp(std::uint8_t* p, Kind k, int c, float v) {
    std::uint8_t* b = p + c * bytes_of(k);
    switch (k) {
    case Kind::f32: std::memcpy(b, &v, 4); break;
    case Kind::f16: {
        const std::uint16_t h = float_to_half(v);
        std::memcpy(b, &h, 2);
        break;
    }
    case Kind::unorm16: {
        const std::uint16_t u = std::uint16_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535.0f));
        std::memcpy(b, &u, 2);
        break;
    }
    case Kind::unorm8: b[0] = std::uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); break;
    case Kind::snorm16: {
        const std::int16_t s = std::int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
        std::memcpy(b, &s, 2);
        break;
    }
    case Kind::snorm8: b[0] = std::uint8_t(std::int8_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 127.0f))); break;
    }
}

}  // namespace

bool expand_rect_vertices(std::span<const std::uint8_t> src, std::uint32_t stride, std::span<const CpuElement> elements,
                          std::uint32_t rects, std::span<const std::uint32_t> src_indices, CpuExpansion& out) {
    if (!stride || !rects) return false;
    if (!src_indices.empty() && src_indices.size() < std::size_t(rects) * 3) return false;
    out.vertices.assign(std::size_t(rects) * 4 * stride, 0);
    out.indices.clear();
    out.corners.clear();
    int position = -1;
    for (std::size_t i = 0; i < elements.size(); ++i) {
        int n;
        Kind k;
        if (components(elements[i].format, n, k) && (k == Kind::f32 || k == Kind::f16) && n >= 2 &&
            elements[i].offset + 2 * bytes_of(k) <= stride) {
            position = int(i);
            break;
        }
    }
    for (std::uint32_t r = 0; r < rects; ++r) {
        const std::uint8_t* v[3];
        for (int j = 0; j < 3; ++j) {
            const std::uint64_t idx = src_indices.empty() ? std::uint64_t(r) * 3 + j : src_indices[std::size_t(r) * 3 + j];
            if ((idx + 1) * stride > src.size()) return false;
            v[j] = src.data() + idx * stride;
        }
        int corner = 1;
        if (position >= 0) {
            int n;
            Kind k;
            components(elements[position].format, n, k);
            std::array<std::array<float, 2>, 3> xy{};
            for (int j = 0; j < 3; ++j)
                for (int c = 0; c < 2; ++c) xy[j][c] = read_comp(v[j] + elements[position].offset, k, c);
            corner = right_angle(xy);
        }
        const int a = (corner + 2) % 3, b = (corner + 1) % 3;
        std::uint8_t* dst = out.vertices.data() + std::size_t(r) * 4 * stride;
        for (int j = 0; j < 3; ++j) std::memcpy(dst + j * stride, v[j], stride);
        std::uint8_t* fourth = dst + 3 * stride;
        std::memcpy(fourth, v[a], stride);
        for (const CpuElement& e : elements) {
            int n;
            Kind k;
            if (!components(e.format, n, k)) continue;
            const int inside = int((stride > e.offset ? stride - e.offset : 0) / bytes_of(k));
            for (int c = 0; c < std::min(n, inside); ++c) {
                const float value = read_comp(v[a] + e.offset, k, c) + read_comp(v[b] + e.offset, k, c) -
                                    read_comp(v[corner] + e.offset, k, c);
                write_comp(fourth + e.offset, k, c, value);
            }
        }
        for (std::uint8_t t : triangle_corners(corner)) out.indices.push_back(r * 4 + t);
        out.corners.push_back(corner);
    }
    return true;
}

namespace {

using msl::base_type;
using msl::Member;
using msl::Param;
using msl::parse_param;
using msl::split_top_level;
using msl::trim;
using msl::vector_size;

bool members_of(const std::string& src, const std::string& name, std::vector<Member>& out, std::string& err) {
    return msl::struct_members(src, name, out, err, false);
}

bool fetch_expr(std::uint32_t format, const std::string& member_type, const std::string& ptr, std::string& out) {
    const std::string bt = base_type(member_type);
    const int n = vector_size(member_type);
    std::string v4;
    if (bt == "float" || bt == "half") {
        switch (format) {
        case 2: v4 = "float4(*(const device packed_float4*)(" + ptr + "))"; break;
        case 6: v4 = "float4(*(const device packed_float3*)(" + ptr + "), 1.0)"; break;
        case 16: v4 = "float4(*(const device packed_float2*)(" + ptr + "), 0.0, 1.0)"; break;
        case 41: v4 = "float4(*(const device float*)(" + ptr + "), 0.0, 0.0, 1.0)"; break;
        case 10: v4 = "float4(*(const device packed_half4*)(" + ptr + "))"; break;
        case 34: v4 = "float4(float2(*(const device packed_half2*)(" + ptr + ")), 0.0, 1.0)"; break;
        case 28: case 29: v4 = "unpack_unorm4x8_to_float(*(const device uint*)(" + ptr + "))"; break;
        case 87: case 91: v4 = "unpack_unorm4x8_to_float(*(const device uint*)(" + ptr + ")).zyxw"; break;
        case 11: v4 = "float4(*(const device packed_ushort4*)(" + ptr + ")) / 65535.0"; break;
        default: return false;
        }
    } else if (bt == "uint" || bt == "int") {
        const std::string vt = bt + "4";
        switch (format) {
        case 3: v4 = vt + "(*(const device packed_uint4*)(" + ptr + "))"; break;
        case 42: v4 = vt + "(*(const device uint*)(" + ptr + "), 0, 0, 1)"; break;
        case 30: v4 = vt + "(*(const device packed_uchar4*)(" + ptr + "))"; break;
        default: return false;
        }
    } else {
        return false;
    }
    static const char* swz[] = {"", ".x", ".xy", ".xyz", ""};
    out = member_type + "(" + v4 + swz[n] + ")";
    if (bt == "half") out = member_type + "(" + v4 + swz[n] + ")";
    return true;
}

}  // namespace

WrapperResult generate_rect_wrapper(const std::string& msl, const std::string& entry,
                                    std::span<const WrapperInput> inputs) {
    WrapperResult res;
    const std::regex sig("\\bvertex\\s+(\\w+)\\s+" + entry + "\\s*\\(");
    std::smatch m;
    if (!std::regex_search(msl, m, sig)) {
        res.error = "vertex entry '" + entry + "' not found";
        return res;
    }
    const std::string out_type = m[1].str();
    const std::size_t sig_begin = std::size_t(m.position(0));
    const std::size_t open = std::size_t(m.position(0) + m.length(0)) - 1;
    int depth = 0;
    std::size_t close = open;
    for (; close < msl.size(); ++close) {
        if (msl[close] == '(') ++depth;
        if (msl[close] == ')' && --depth == 0) break;
    }
    if (close >= msl.size()) {
        res.error = "unbalanced entry signature";
        return res;
    }
    std::vector<Param> params;
    for (const std::string& p : split_top_level(msl.substr(open + 1, close - open - 1))) {
        Param q;
        if (!parse_param(p, q)) {
            res.error = "cannot parse parameter '" + p + "'";
            return res;
        }
        params.push_back(q);
    }
    std::string in_type, in_name;
    std::set<std::uint32_t> used_buffers;
    const std::regex buf("buffer\\((\\d+)\\)");
    for (const Param& p : params) {
        if (p.attr == "stage_in") {
            in_name = p.name;
            in_type = trim(p.decl.substr(0, p.decl.size() - p.name.size()));
        }
        std::smatch b;
        if (std::regex_search(p.attr, b, buf)) used_buffers.insert(std::uint32_t(std::stoul(b[1].str())));
        const bool known = p.attr.empty() || p.attr == "stage_in" || p.attr == "vertex_id" || p.attr == "instance_id" ||
                           p.attr == "base_vertex" || p.attr == "base_instance" ||
                           p.attr.rfind("buffer(", 0) == 0 || p.attr.rfind("texture(", 0) == 0 ||
                           p.attr.rfind("sampler(", 0) == 0;
        if (!known) {
            res.error = "parameter attribute [[" + p.attr + "]] not handled";
            return res;
        }
    }
    std::vector<Member> in_members, out_members;
    if (!in_type.empty() && !members_of(msl, in_type, in_members, res.error)) return res;
    if (!members_of(msl, out_type, out_members, res.error)) return res;
    std::string position;
    for (const Member& mm : out_members)
        if (mm.attr.find("position") != std::string::npos) position = mm.name;
    if (position.empty()) {
        res.error = "no [[position]] output";
        return res;
    }

    std::set<std::uint32_t> slots;
    std::map<std::uint32_t, const WrapperInput*> by_location;
    for (const WrapperInput& in : inputs) {
        slots.insert(in.slot);
        by_location[in.location] = &in;
    }
    std::uint32_t max_slot = slots.empty() ? 0 : *slots.rbegin();
    res.vertex_buffer_index.assign(max_slot + 1, 0);
    for (std::uint32_t s = 0; s <= max_slot; ++s) {
        res.vertex_buffer_index[s] = 30 - s;
        if (used_buffers.count(30 - s)) {
            res.error = "vertex buffer index " + std::to_string(30 - s) + " is used by the shader";
            return res;
        }
        used_buffers.insert(30 - s);
    }
    auto free_index = [&] {
        for (std::uint32_t i = 0; i < 31; ++i)
            if (!used_buffers.count(i)) {
                used_buffers.insert(i);
                return i;
            }
        return 31u;
    };
    res.params_index = free_index();
    res.index_buffer_index = free_index();
    if (res.params_index > 30 || res.index_buffer_index > 30) {
        res.error = "no free buffer index for the rect parameters";
        return res;
    }

    std::ostringstream fetch;
    for (const Member& mm : in_members) {
        const std::regex attr_re("attribute\\((\\d+)\\)");
        std::smatch am;
        if (!std::regex_search(mm.attr, am, attr_re)) {
            res.error = "stage_in member " + mm.name + " has no attribute index";
            return res;
        }
        const std::uint32_t loc = std::uint32_t(std::stoul(am[1].str()));
        auto it = by_location.find(loc);
        if (it == by_location.end()) {
            fetch << "        gta6rt_in." << mm.name << " = " << mm.type << "(0);\n";
            continue;
        }
        const WrapperInput& in = *it->second;
        const std::string ptr = "gta6rt_vb" + std::to_string(in.slot) + " + gta6rt_v * gta6rt_rp.strides[" +
                                std::to_string(in.slot) + "] + " + std::to_string(in.offset) + "u";
        std::string expr;
        if (!fetch_expr(in.format, mm.type, ptr, expr)) {
            res.error = "attribute " + std::to_string(loc) + ": DXGI format " + std::to_string(in.format) + " into " +
                        mm.type + " not generated";
            return res;
        }
        fetch << "        gta6rt_in." << mm.name << " = " << expr << ";\n";
    }

    const std::string body_name = entry + "_rect_body";
    const std::string wrapper_name = entry + "_rect";
    std::string decls, args, extra;
    for (const Param& p : params) {
        if (!decls.empty()) decls += ", ";
        decls += p.decl;
        if (!args.empty()) args += ", ";
        if (p.attr == "stage_in") {
            args += "gta6rt_in";
        } else if (p.attr == "vertex_id") {
            args += "gta6rt_v";
        } else if (p.attr == "instance_id") {
            args += "gta6rt_iid";
        } else if (p.attr == "base_vertex") {
            args += "uint(gta6rt_rp.indexed ? gta6rt_rp.base_vertex : int(gta6rt_rp.first))";
        } else if (p.attr == "base_instance") {
            args += "gta6rt_rp.first_instance";
        } else {
            args += p.name;
            extra += ", " + p.decl + (p.attr.empty() ? "" : " [[" + p.attr + "]]");
        }
    }

    std::ostringstream w;
    w << msl.substr(0, sig_begin);
    w << "static inline __attribute__((always_inline)) " << out_type << " " << body_name << "(" << decls << ")";
    w << msl.substr(close + 1);
    w << "\n\nstruct gta6rt_RectParams\n{\n    uint first;\n    int base_vertex;\n    uint indexed;\n"
         "    uint first_instance;\n    uint strides[8];\n};\n\n";
    w << "vertex " << out_type << " " << wrapper_name << "(uint gta6rt_vid [[vertex_id]], uint gta6rt_iid [[instance_id]]"
      << extra << ", constant gta6rt_RectParams& gta6rt_rp [[buffer(" << res.params_index << ")]]";
    for (std::uint32_t s = 0; s <= max_slot; ++s)
        if (slots.count(s))
            w << ", const device uchar* gta6rt_vb" << s << " [[buffer(" << res.vertex_buffer_index[s] << ")]]";
    w << ", const device uchar* gta6rt_ib [[buffer(" << res.index_buffer_index << ")]])\n{\n";
    w << "    const uint gta6rt_r = gta6rt_vid / 6u;\n    const uint gta6rt_k = gta6rt_vid % 6u;\n";
    w << "    " << out_type << " gta6rt_c[3];\n";
    w << "    for (uint gta6rt_j = 0; gta6rt_j < 3u; ++gta6rt_j)\n    {\n";
    w << "        const uint gta6rt_n = gta6rt_rp.first + gta6rt_r * 3u + gta6rt_j;\n";
    w << "        uint gta6rt_v = gta6rt_n;\n";
    w << "        if (gta6rt_rp.indexed == 4u) gta6rt_v = uint(int(((const device uint*)gta6rt_ib)[gta6rt_n]) + gta6rt_rp.base_vertex);\n";
    w << "        else if (gta6rt_rp.indexed == 2u) gta6rt_v = uint(int(((const device ushort*)gta6rt_ib)[gta6rt_n]) + gta6rt_rp.base_vertex);\n";
    if (!in_type.empty()) w << "        " << in_type << " gta6rt_in = {};\n" << fetch.str();
    w << "        gta6rt_c[gta6rt_j] = " << body_name << "(" << args << ");\n    }\n";
    w << "    const float2 gta6rt_p0 = gta6rt_c[0]." << position << ".xy;\n";
    w << "    const float2 gta6rt_p1 = gta6rt_c[1]." << position << ".xy;\n";
    w << "    const float2 gta6rt_p2 = gta6rt_c[2]." << position << ".xy;\n";
    w << "    const float gta6rt_d12 = distance_squared(gta6rt_p1, gta6rt_p2);\n";
    w << "    const float gta6rt_d20 = distance_squared(gta6rt_p2, gta6rt_p0);\n";
    w << "    const float gta6rt_d01 = distance_squared(gta6rt_p0, gta6rt_p1);\n";
    w << "    const uint gta6rt_corner = (gta6rt_d12 > gta6rt_d20 && gta6rt_d12 > gta6rt_d01) ? 0u : "
         "((gta6rt_d01 > gta6rt_d12 && gta6rt_d01 > gta6rt_d20) ? 2u : 1u);\n";
    w << "    const uint gta6rt_table[18] = { 0u, 1u, 3u, 0u, 3u, 2u,  0u, 1u, 2u, 0u, 2u, 3u,  0u, 1u, 2u, 0u, 3u, 1u };\n";
    w << "    const uint gta6rt_which = gta6rt_table[gta6rt_corner * 6u + gta6rt_k];\n";
    w << "    if (gta6rt_which < 3u) return gta6rt_c[gta6rt_which];\n";
    w << "    const uint gta6rt_a = (gta6rt_corner + 2u) % 3u;\n    const uint gta6rt_b = (gta6rt_corner + 1u) % 3u;\n";
    w << "    " << out_type << " gta6rt_o = gta6rt_c[gta6rt_a];\n";
    for (const Member& mm : out_members) {
        const std::string bt = base_type(mm.type);
        if (bt != "float" && bt != "half") continue;
        w << "    gta6rt_o." << mm.name << " = gta6rt_c[gta6rt_a]." << mm.name << " + gta6rt_c[gta6rt_b]." << mm.name
          << " - gta6rt_c[gta6rt_corner]." << mm.name << ";\n";
    }
    w << "    return gta6rt_o;\n}\n";
    res.source = w.str();
    res.entry = wrapper_name;
    res.ok = true;
    return res;
}

}  // namespace gta6rt::rect
