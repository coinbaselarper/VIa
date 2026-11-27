#include "pcache.h"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace gta6rt::pcache {

namespace {

constexpr const char* kPhaseNames[] = {"boot", "landing", "movie", "first-run", "story-load", "world"};

std::string escape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c <= ' ' || c == '~' || c == '=' || c == '@' || c == ',' || c >= 0x7f) {
            char buf[4];
            std::snprintf(buf, sizeof buf, "~%02x", c);
            out += buf;
        } else {
            out += char(c);
        }
    }
    return out;
}

std::string unescape(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '~' && i + 2 < s.size()) {
            out += char(std::strtoul(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string fbits(float f) {
    char buf[12];
    std::snprintf(buf, sizeof buf, "%x", std::bit_cast<std::uint32_t>(f));
    return buf;
}
float unfbits(const std::string& s) { return std::bit_cast<float>(std::uint32_t(std::strtoul(s.c_str(), nullptr, 16))); }

template <class E> std::uint32_t u(E e) { return static_cast<std::uint32_t>(e); }

std::string join(const std::vector<std::uint32_t>& v, char sep) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += std::to_string(v[i]);
    }
    return out;
}

std::vector<std::uint32_t> split_u(const std::string& s, char sep) {
    std::vector<std::uint32_t> out;
    std::size_t at = 0;
    while (at <= s.size()) {
        const std::size_t end = s.find(sep, at);
        const std::string part = s.substr(at, end == std::string::npos ? std::string::npos : end - at);
        out.push_back(std::uint32_t(std::strtoul(part.c_str(), nullptr, 10)));
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return out;
}

std::vector<std::string> split_s(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (true) {
        const std::size_t end = s.find(sep, at);
        out.push_back(s.substr(at, end == std::string::npos ? std::string::npos : end - at));
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return out;
}

std::string targets(const std::array<std::uint32_t, 8>& f, std::uint32_t n) {
    return std::to_string(n) + ":" + join(std::vector<std::uint32_t>(f.begin(), f.end()), '.');
}

std::string blends(const std::array<gpu::TargetBlend, 8>& b) {
    std::string out;
    for (std::size_t i = 0; i < b.size(); ++i) {
        if (i) out += ',';
        const auto& t = b[i];
        out += join({t.enable ? 1u : 0u, u(t.src), u(t.dst), u(t.op), u(t.src_alpha), u(t.dst_alpha), u(t.op_alpha),
                     t.write_mask},
                    '.');
    }
    return out;
}

std::string raster(const gpu::RasterDesc& r) {
    return std::to_string(u(r.cull)) + "." + (r.front_ccw ? "1" : "0") + "." + (r.wireframe ? "1" : "0") + "." +
           std::to_string(r.depth_bias) + "." + fbits(r.depth_bias_clamp) + "." + fbits(r.slope_scaled_depth_bias) + "." +
           (r.depth_clip ? "1" : "0");
}

std::string face(const gpu::StencilFace& f) { return join({u(f.fail), u(f.depth_fail), u(f.pass), u(f.func)}, '.'); }

std::string depth_stencil(const gpu::DepthStencilDesc& d) {
    return join({d.depth_test ? 1u : 0u, d.depth_write ? 1u : 0u, u(d.depth_func), d.stencil ? 1u : 0u,
                 d.stencil_read_mask, d.stencil_write_mask},
                '.') +
           "." + face(d.front) + "." + face(d.back);
}

std::string inputs(const std::vector<gpu::InputElement>& in) {
    if (in.empty()) return "-";
    std::string out;
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (i) out += ',';
        const auto& e = in[i];
        out += join({e.location, e.format, e.slot, e.offset, e.per_instance ? 1u : 0u, e.step_rate}, '.');
    }
    return out;
}

std::string threads(const std::array<std::uint32_t, 3>& t) { return join({t[0], t[1], t[2]}, '.'); }

std::string stage(const std::string& name, const std::string& content) {
    if (name.empty()) return "-";
    return escape(name) + "@" + (content.empty() ? "-" : escape(content.substr(0, 16)));
}

bool parse_targets(const std::string& v, std::array<std::uint32_t, 8>& f, std::uint32_t& n) {
    const auto c = v.find(':');
    if (c == std::string::npos) return false;
    n = std::uint32_t(std::strtoul(v.substr(0, c).c_str(), nullptr, 10));
    const auto parts = split_u(v.substr(c + 1), '.');
    if (parts.size() != 8) return false;
    std::copy(parts.begin(), parts.end(), f.begin());
    return true;
}

bool parse_blends(const std::string& v, std::array<gpu::TargetBlend, 8>& b) {
    const auto parts = split_s(v, ',');
    if (parts.size() != 8) return false;
    for (std::size_t i = 0; i < 8; ++i) {
        const auto x = split_u(parts[i], '.');
        if (x.size() != 8) return false;
        auto& t = b[i];
        t.enable = x[0] != 0;
        t.src = gpu::Blend(x[1]);
        t.dst = gpu::Blend(x[2]);
        t.op = gpu::BlendOp(x[3]);
        t.src_alpha = gpu::Blend(x[4]);
        t.dst_alpha = gpu::Blend(x[5]);
        t.op_alpha = gpu::BlendOp(x[6]);
        t.write_mask = std::uint8_t(x[7]);
    }
    return true;
}

bool parse_raster(const std::string& v, gpu::RasterDesc& r) {
    const auto x = split_s(v, '.');
    if (x.size() != 7) return false;
    r.cull = gpu::CullMode(std::strtoul(x[0].c_str(), nullptr, 10));
    r.front_ccw = x[1] == "1";
    r.wireframe = x[2] == "1";
    r.depth_bias = std::int32_t(std::strtol(x[3].c_str(), nullptr, 10));
    r.depth_bias_clamp = unfbits(x[4]);
    r.slope_scaled_depth_bias = unfbits(x[5]);
    r.depth_clip = x[6] == "1";
    return true;
}

bool parse_depth_stencil(const std::string& v, gpu::DepthStencilDesc& d) {
    const auto x = split_u(v, '.');
    if (x.size() != 14) return false;
    d.depth_test = x[0] != 0;
    d.depth_write = x[1] != 0;
    d.depth_func = gpu::CompareFunc(x[2]);
    d.stencil = x[3] != 0;
    d.stencil_read_mask = std::uint8_t(x[4]);
    d.stencil_write_mask = std::uint8_t(x[5]);
    d.front = {gpu::StencilOp(x[6]), gpu::StencilOp(x[7]), gpu::StencilOp(x[8]), gpu::CompareFunc(x[9])};
    d.back = {gpu::StencilOp(x[10]), gpu::StencilOp(x[11]), gpu::StencilOp(x[12]), gpu::CompareFunc(x[13])};
    return true;
}

bool parse_inputs(const std::string& v, std::vector<gpu::InputElement>& in) {
    in.clear();
    if (v == "-") return true;
    for (const auto& p : split_s(v, ',')) {
        const auto x = split_u(p, '.');
        if (x.size() != 6) return false;
        gpu::InputElement e;
        e.location = x[0];
        e.format = x[1];
        e.slot = x[2];
        e.offset = x[3];
        e.per_instance = x[4] != 0;
        e.step_rate = x[5];
        in.push_back(e);
    }
    return true;
}

bool parse_threads(const std::string& v, std::array<std::uint32_t, 3>& t) {
    const auto x = split_u(v, '.');
    if (x.size() != 3) return false;
    t = {x[0], x[1], x[2]};
    return true;
}

void parse_stage(const std::string& v, std::string& name, std::string& content) {
    name.clear();
    content.clear();
    if (v == "-") return;
    const auto at = v.rfind('@');
    name = unescape(v.substr(0, at));
    content = at == std::string::npos ? "-" : unescape(v.substr(at + 1));
}

std::string content_of(const ContentKeyFn& ck, const std::string& name) {
    if (name.empty()) return {};
    std::string k = ck ? ck(name) : std::string("-");
    return k.empty() ? "-" : k;
}

}  // namespace

const char* phase_name(Phase p) {
    const auto i = std::size_t(p);
    return i < std::size(kPhaseNames) ? kPhaseNames[i] : "?";
}

std::optional<Phase> phase_from_name(const std::string& s) {
    for (std::size_t i = 0; i < std::size(kPhaseNames); ++i)
        if (s == kPhaseNames[i]) return Phase(i);
    return std::nullopt;
}

std::string encode(const Spec& s) {
    std::string out(1, char(s.kind));
    const auto add = [&](const char* k, const std::string& v) {
        out += ' ';
        out += k;
        out += '=';
        out += v;
    };
    switch (s.kind) {
    case Kind::render: {
        const auto& r = s.render;
        add("vs", stage(s.names[0], s.content[0]));
        add("ps", stage(s.names[1], s.content[1]));
        add("in", inputs(r.inputs));
        add("rt", targets(r.rt_formats, r.rt_count));
        add("bl", blends(r.blend));
        add("ds", std::to_string(r.depth_format));
        add("s", std::to_string(r.samples));
        add("ra", raster(r.raster));
        add("dss", depth_stencil(r.depth_stencil));
        add("tp", std::to_string(u(r.topology)));
        add("rm", std::to_string(u(r.rect)));
        add("d24", std::to_string(u(r.d24)));
        break;
    }
    case Kind::mesh: {
        const auto& m = s.mesh;
        add("ms", stage(s.names[0], s.content[0]));
        add("ps", stage(s.names[1], s.content[1]));
        add("ot", threads(m.object_threads));
        add("mt", threads(m.mesh_threads));
        add("pl", std::to_string(m.payload_bytes));
        add("rt", targets(m.rt_formats, m.rt_count));
        add("bl", blends(m.blend));
        add("ds", std::to_string(m.depth_format));
        add("s", std::to_string(m.samples));
        add("ra", raster(m.raster));
        add("dss", depth_stencil(m.depth_stencil));
        add("pc", m.precheck ? "1" : "0");
        break;
    }
    case Kind::compute:
        add("cs", stage(s.names[0], s.content[0]));
        add("th", threads(s.compute.threads));
        break;
    }
    return out;
}

bool decode(const std::string& text, Spec& out, std::string* error) {
    const auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (text.size() < 2 || (text[0] != 'R' && text[0] != 'M' && text[0] != 'C') || text[1] != ' ')
        return fail("not a pipeline text");
    out = Spec{};
    out.kind = Kind(text[0]);
    std::istringstream in(text.substr(2));
    std::string token;
    unsigned seen = 0;
    while (in >> token) {
        const auto eq = token.find('=');
        if (eq == std::string::npos) return fail("token without '=': " + token);
        const std::string k = token.substr(0, eq), v = token.substr(eq + 1);
        bool ok = true;
        auto& r = out.render;
        auto& m = out.mesh;
        if (k == "vs" || k == "ms" || k == "cs") parse_stage(v, out.names[0], out.content[0]);
        else if (k == "ps") parse_stage(v, out.names[1], out.content[1]);
        else if (k == "in") ok = parse_inputs(v, r.inputs);
        else if (k == "rt") ok = out.kind == Kind::mesh ? parse_targets(v, m.rt_formats, m.rt_count) : parse_targets(v, r.rt_formats, r.rt_count);
        else if (k == "bl") ok = out.kind == Kind::mesh ? parse_blends(v, m.blend) : parse_blends(v, r.blend);
        else if (k == "ds") (out.kind == Kind::mesh ? m.depth_format : r.depth_format) = std::uint32_t(std::strtoul(v.c_str(), nullptr, 10));
        else if (k == "s") (out.kind == Kind::mesh ? m.samples : r.samples) = std::uint32_t(std::strtoul(v.c_str(), nullptr, 10));
        else if (k == "ra") ok = parse_raster(v, out.kind == Kind::mesh ? m.raster : r.raster);
        else if (k == "dss") ok = parse_depth_stencil(v, out.kind == Kind::mesh ? m.depth_stencil : r.depth_stencil);
        else if (k == "tp") r.topology = gpu::TopologyType(std::strtoul(v.c_str(), nullptr, 10));
        else if (k == "rm") r.rect = gpu::RectMode(std::strtoul(v.c_str(), nullptr, 10));
        else if (k == "d24") r.d24 = gpu::D24Mode(std::strtoul(v.c_str(), nullptr, 10));
        else if (k == "ot") ok = parse_threads(v, m.object_threads);
        else if (k == "mt") ok = parse_threads(v, m.mesh_threads);
        else if (k == "pl") m.payload_bytes = std::uint32_t(std::strtoul(v.c_str(), nullptr, 10));
        else if (k == "pc") m.precheck = v == "1";
        else if (k == "th") ok = parse_threads(v, out.compute.threads);
        else return fail("unknown field " + k);
        if (!ok) return fail("bad value for " + k + ": " + v);
        ++seen;
    }
    const unsigned need = out.kind == Kind::render ? 12 : out.kind == Kind::mesh ? 12 : 2;
    if (seen != need) return fail("expected " + std::to_string(need) + " fields, got " + std::to_string(seen));
    if (out.names[0].empty()) return fail("no first-stage shader");
    return true;
}

std::string key_of(const std::string& text) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : text) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

Spec spec_of(const gpu::RenderPipelineDesc& d, const std::string& vs, const std::string& ps, const ContentKeyFn& ck) {
    Spec s;
    s.kind = Kind::render;
    s.names[0] = vs;
    s.names[1] = ps;
    s.content[0] = content_of(ck, vs);
    s.content[1] = content_of(ck, ps);
    s.render = d;
    return s;
}

Spec spec_of(const gpu::MeshPipelineDesc& d, const std::string& ms, const std::string& ps, const ContentKeyFn& ck) {
    Spec s;
    s.kind = Kind::mesh;
    s.names[0] = ms;
    s.names[1] = ps;
    s.content[0] = content_of(ck, ms);
    s.content[1] = content_of(ck, ps);
    s.mesh = d;
    return s;
}

Spec spec_of(const gpu::ComputePipelineDesc& d, const std::string& cs, const ContentKeyFn& ck) {
    Spec s;
    s.kind = Kind::compute;
    s.names[0] = cs;
    s.content[0] = content_of(ck, cs);
    s.compute = d;
    return s;
}

// ---- manifest ----

std::size_t Manifest::lookup(const std::string& key) const {
    auto it = std::lower_bound(index_.begin(), index_.end(), key,
                               [](const auto& a, const std::string& k) { return a.first < k; });
    return it != index_.end() && it->first == key ? it->second : std::size_t(-1);
}

void Manifest::reindex() {
    index_.clear();
    index_.reserve(entries_.size());
    for (std::size_t i = 0; i < entries_.size(); ++i) index_.emplace_back(entries_[i].key, i);
    std::sort(index_.begin(), index_.end());
}

const Entry* Manifest::find(const std::string& key) const {
    const std::size_t i = lookup(key);
    return i == std::size_t(-1) ? nullptr : &entries_[i];
}

bool Manifest::load(const std::string& path, std::string* error) {
    std::ifstream in(path);
    if (!in) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    std::string line;
    if (!std::getline(in, line) || line != kHeader) {
        if (error) *error = path + ": not a pcache manifest (header '" + line.substr(0, 60) + "')";
        return false;
    }
    std::vector<Entry> got;
    std::size_t bad = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto f = split_s(line, '\t');
        if (f.size() != 5) {
            ++bad;
            continue;
        }
        Entry e;
        e.key = f[0];
        const auto p = phase_from_name(f[1]);
        e.text = f[4];
        if (!p || key_of(e.text) != e.key) {
            ++bad;
            continue;
        }
        e.phase = *p;
        e.t = std::strtod(f[2].c_str(), nullptr);
        e.runs = std::uint32_t(std::strtoul(f[3].c_str(), nullptr, 10));
        got.push_back(std::move(e));
    }
    if (bad) {
        if (error) *error = path + ": " + std::to_string(bad) + " malformed lines (a torn or edited file); not used";
        return false;
    }
    entries_ = std::move(got);
    reindex();
    return true;
}

bool Manifest::save(const std::string& path, std::string* error) const {
    const std::string tmp = path + ".tmp." + std::to_string(::getpid());
    std::string body = std::string(kHeader) + "\n# key\tphase\tt_s\truns\tpipeline\n";
    for (const Entry& e : ordered()) {
        char t[32];
        std::snprintf(t, sizeof t, "%.3f", e.t);
        body += e.key + "\t" + phase_name(e.phase) + "\t" + t + "\t" + std::to_string(e.runs) + "\t" + e.text + "\n";
    }
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        if (error) *error = "cannot write " + tmp + ": " + std::strerror(errno);
        return false;
    }
    std::size_t done = 0;
    while (done < body.size()) {
        const ssize_t n = ::write(fd, body.data() + done, body.size() - done);
        if (n <= 0) {
            ::close(fd);
            ::unlink(tmp.c_str());
            if (error) *error = "short write to " + tmp;
            return false;
        }
        done += std::size_t(n);
    }
    ::fsync(fd);
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        if (error) *error = "rename to " + path + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

void Manifest::merge(const Manifest& other, bool new_run) {
    std::size_t added = 0;
    for (const Entry& e : other.entries_) {
        const std::size_t i = lookup(e.key);
        if (i == std::size_t(-1)) {
            entries_.push_back(e);
            ++added;
            continue;
        }
        Entry& mine = entries_[i];
        if (e.phase < mine.phase || (e.phase == mine.phase && e.t < mine.t)) {
            mine.phase = e.phase;
            mine.t = e.t;
        }
        mine.runs = new_run ? mine.runs + e.runs : std::max(mine.runs, e.runs);
    }
    if (added) reindex();
}

void Manifest::note(const Entry& e) {
    if (lookup(e.key) != std::size_t(-1)) return;
    entries_.push_back(e);
    auto it = std::lower_bound(index_.begin(), index_.end(), e.key,
                               [](const auto& a, const std::string& k) { return a.first < k; });
    index_.insert(it, {e.key, entries_.size() - 1});
}

std::vector<Entry> Manifest::ordered() const {
    std::vector<Entry> out = entries_;
    std::stable_sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        return a.phase != b.phase ? a.phase < b.phase : a.t < b.t;
    });
    return out;
}

std::string archive_version(const std::string& port_root, const std::string& catalog_manifest) {
    std::string translator = "shaderc_msl recipe 1";
    {
        std::ifstream in(port_root + "/shaderc_msl/warmup/translator_version.txt");
        std::string line;
        if (in && std::getline(in, line) && !line.empty()) translator = line;
    }
    std::string catalog = "catalog ?";
    {
        std::ifstream in(catalog_manifest);
        std::string line;
        if (in && std::getline(in, line)) {
            const auto v = line.find(" v");
            if (line.rfind("# shaderc_msl manifest", 0) == 0 && v != std::string::npos)
                catalog = "catalog " + line.substr(v + 1, line.find(' ', v + 1) - v - 1);
        }
    }
    return translator + "; " + catalog + "; pcache 1";
}

}  // namespace gta6rt::pcache
