#include "gta6rt_shaderc_msl/catalog.h"

#include <fstream>
#include <sstream>

namespace gta6rt::shaderc_msl {

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

std::vector<std::pair<int, int>> tabs_semicolon(const std::string &s) {
    std::vector<std::pair<int, int>> v;
    if (s != "-")
        for (auto &t : split(s, ';')) {
            auto kv = split(t, ':');
            if (kv.size() == 2) v.push_back({std::stoi(kv[0]), std::stoi(kv[1])});
        }
    return v;
}

Entry entry_of(const std::string &s) {
    if (s == "vertex") return Entry::vertex;
    if (s == "fragment") return Entry::fragment;
    if (s == "kernel") return Entry::kernel;
    if (s == "mesh") return Entry::mesh;
    return Entry::unknown;
}

}

bool Catalog::load(const std::string &path, std::string *error) {
    std::ifstream f(path);
    if (!f) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    entries_.clear();
    by_name_.clear();
    auto slash = path.find_last_of('/');
    std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);
    root_ = dir;
    std::string line;
    size_t lineno = 0;
    while (std::getline(f, line)) {
        lineno++;
        if (line.empty()) continue;
        if (line[0] == '#' && line.find('\t') == std::string::npos) {
            auto p = line.find("root=");
            if (p != std::string::npos) {
                std::string r = line.substr(p + 5);
                root_ = (!r.empty() && r[0] == '/') ? r : dir + "/" + r;
            }
            continue;
        }
        auto c = split(line, '\t');
        if (c.size() < 9) {
            if (error) *error = "line " + std::to_string(lineno) + ": " + std::to_string(c.size()) + " columns";
            return false;
        }
        ShaderEntry e;
        e.name = c[0];
        e.key = c[1];
        e.stage = c[2];
        e.entry = entry_of(c[3]);
        e.metallib = c[4];
        e.push_buffer = c[5] == "-" ? -1 : std::stoi(c[5]);
        if (c[6] != "-") {
            for (auto &t : split(c[6], ',')) {
                auto kv = split(t, ':');
                if (kv.size() == 2) e.tables.push_back({std::stoi(kv[0]), std::stoi(kv[1])});
            }
        }
        e.vertex_as_kernel = c[7] == "1";
        if (c[8] != "-") e.passes = split(c[8], ',');
        if (c.size() > 9 && c[9] != "-") {
            for (auto &t : split(c[9], ',')) {
                auto kv = split(t, ':');
                if (kv.size() == 2) e.descriptor_arrays.push_back({std::stoi(kv[0]), std::stoi(kv[1])});
            }
        }
        if (c.size() > 10 && c[10] != "-") e.mesh_route = c[10];
        if (c.size() >= 29 && c[26] != "-") {
            e.cs_route = c[26];
            e.x_buf_words = std::stoi(c[27]);
            e.x_buf_buffer = std::stoi(c[28]);
        }
        if (c.size() >= 26) {
            auto tabs = [&](const std::string &s) {
                std::vector<std::pair<int, int>> v;
                if (s != "-")
                    for (auto &t : split(s, ',')) {
                        auto kv = split(t, ':');
                        if (kv.size() == 2) v.push_back({std::stoi(kv[0]), std::stoi(kv[1])});
                    }
                return v;
            };
            if (c[11] != "-") {
                e.split.present = true;
                e.split.mesh_metallib = c[11];
                e.split.object_metallib = c[12];
                e.split.chunks = std::stoi(c[13]);
                e.split.vmax = std::stoi(c[14]);
                e.split.pmax = std::stoi(c[15]);
                e.split.push_buffer = c[16] == "-" ? -1 : std::stoi(c[16]);
                e.split.tables = tabs(c[17]);
            }
            if (c[18] != "-") {
                e.msc.present = true;
                e.msc.cs_metallib = c[18];
                e.msc.vs_metallib = c[19];
                e.msc.cs_push = c[20] == "-" ? -1 : std::stoi(c[20]);
                e.msc.vs_push = c[21] == "-" ? -1 : std::stoi(c[21]);
                e.msc.cs_tables = tabs(c[22]);
                e.msc.vs_tables = tabs(c[23]);
                auto l = split(c[24], ',');
                if (l.size() != 10) {
                    if (error) *error = "line " + std::to_string(lineno) + ": bad msc_layout";
                    return false;
                }
                e.msc.slot_bytes = std::stoi(l[0]);
                e.msc.vertices = std::stoi(l[1]);
                e.msc.vertex_stride = std::stoi(l[2]);
                e.msc.primitives = std::stoi(l[3]);
                e.msc.primitive_stride = std::stoi(l[4]);
                e.msc.max_vertices = std::stoi(l[5]);
                e.msc.max_primitives = std::stoi(l[6]);
                e.msc.address_dword = std::stoi(l[7]);
                e.msc.push_bytes = std::stoi(l[8]);
                e.msc.local_x = std::stoi(l[9]);
                e.msc.params = tabs_semicolon(c[25]);
            }
        }
        by_name_[e.name] = entries_.size();
        entries_.push_back(std::move(e));
    }
    return true;
}

const ShaderEntry *Catalog::find(const std::string &name) const {
    auto it = by_name_.find(name);
    return it == by_name_.end() ? nullptr : &entries_[it->second];
}

std::string Catalog::library_path(const ShaderEntry &e) const { return root_ + "/" + e.metallib; }

}
