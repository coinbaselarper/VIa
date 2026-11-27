#pragma once
// port/d3d12_vulkan: an A8_UNORM render target stored as an R8_UNORM image (no VK_FORMAT_A8_UNORM_KHR attachment,
// as on KosmicKrisp 26.3.1). The pixel shader's colour output at such a location is written as (a, g, b, a): the
// attachment's one channel takes the alpha the A8 target would, and SRC_ALPHA blend factors still read alpha. A
// component store through an access chain sends alpha to both red and alpha and red to green. False for a shape
// this does not handle (the pipeline is then refused).

#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace leonida::a8_red {

// The resources stored this way, by ResourceId: a clear of one writes the clear colour's alpha to red.
class Targets {
public:
    void add(std::uint64_t id) { std::lock_guard l(m_); ids_.insert(id); }
    void remove(std::uint64_t id) { std::lock_guard l(m_); ids_.erase(id); }
    bool contains(std::uint64_t id) const { std::lock_guard l(m_); return ids_.count(id) != 0; }
private:
    mutable std::mutex m_;
    std::set<std::uint64_t> ids_;
};
inline Targets& targets() {
    static Targets t;
    return t;
}

using Inst = std::vector<std::uint32_t>;

inline Inst instruction(std::uint32_t op, std::initializer_list<std::uint32_t> args) {
    Inst w{(std::uint32_t(args.size() + 1) << 16) | op};
    w.insert(w.end(), args);
    return w;
}

inline bool remap(std::vector<std::uint32_t>& words, unsigned mask) {
    if (!mask) return true;
    if (words.size() < 5 || words[0] != 0x07230203) return false;
    std::vector<Inst> code;
    std::map<unsigned, unsigned> locations, pointer_type, width, scalar, variables, constants;
    std::set<unsigned> float_types, int_types;
    unsigned next = words[3];
    for (std::size_t at = 5; at < words.size();) {
        const unsigned n = words[at] >> 16, op = words[at] & 65535u;
        if (!n || n > words.size() - at) return false;
        Inst i(words.begin() + at, words.begin() + at + n);
        at += n;
        if (op == 71 && n == 4 && i[2] == 30) locations[i[1]] = i[3];
        else if (op == 22 && n == 3) float_types.insert(i[1]);
        else if (op == 21 && n == 4) int_types.insert(i[1]);
        else if (op == 23 && n == 4) { width[i[1]] = i[3]; scalar[i[1]] = i[2]; }
        else if (op == 32 && n == 4 && i[2] == 3) pointer_type[i[1]] = i[3];
        else if (op == 59 && n >= 4 && i[3] == 3) variables[i[2]] = i[1];
        else if (op == 43 && n == 4 && int_types.count(i[1])) constants[i[2]] = i[3];
        code.push_back(std::move(i));
    }
    std::map<unsigned, unsigned> outputs;  // variable -> its vec4 type
    for (auto [id, ptr] : variables)
        if (locations.count(id) && locations[id] < 8 && (mask & (1u << locations[id])) && pointer_type.count(ptr)) {
            const unsigned type = pointer_type[ptr];
            if (width[type] != 4 || !float_types.count(scalar[type])) return false;
            outputs[id] = type;
        }
    if (outputs.empty()) return true;
    std::map<unsigned, unsigned> alpha_ptrs;   // access chain to .a -> its output variable
    std::map<unsigned, unsigned> chain_types;  // pointer type of each such chain
    unsigned index_type = 0, zero = 0;
    for (auto& [id, value] : constants)
        if (value == 0) zero = id;
    std::vector<Inst> declarations;
    std::vector<Inst> out;
    bool changed = false;
    for (auto& i : code) {
        const unsigned op = i[0] & 65535u;
        if ((op == 65 || op == 66) && i.size() >= 4 && outputs.count(i[3])) {
            if (i.size() != 5 || !constants.count(i[4])) return false;
            const unsigned c = constants[i[4]];
            if (c == 0) {
                bool found = false;
                for (auto& [id, value] : constants)
                    if (value == 1 && !found) { i[4] = id; found = true; }
                if (!found) return false;
                changed = true;
            } else if (c == 3) {
                alpha_ptrs[i[2]] = i[3];
                chain_types[i[2]] = i[1];
            }
            out.push_back(std::move(i));
            continue;
        }
        if (op == 63 && i.size() >= 3 && outputs.count(i[1])) return false;  // OpCopyMemory into the output
        if (op == 62 && i.size() >= 3 && outputs.count(i[1])) {
            const unsigned type = outputs[i[1]];
            const unsigned shuffled = next++;
            out.push_back(instruction(79, {type, shuffled, i[2], i[2], 3, 1, 2, 3}));  // OpVectorShuffle a g b a
            i[2] = shuffled;
            out.push_back(std::move(i));
            changed = true;
            continue;
        }
        if (op == 62 && i.size() >= 3 && alpha_ptrs.count(i[1])) {
            if (!zero) {
                zero = next++;
                index_type = *int_types.begin();
                declarations.push_back(instruction(43, {index_type, zero, 0}));
            }
            const unsigned red = next++;
            const unsigned value = i[2];
            const unsigned var = alpha_ptrs[i[1]], ptr = chain_types[i[1]];
            out.push_back(std::move(i));
            out.push_back(instruction(65, {ptr, red, var, zero}));
            out.push_back(instruction(62, {red, value}));
            changed = true;
            continue;
        }
        out.push_back(std::move(i));
    }
    if (!changed) return false;
    if (!declarations.empty() && int_types.empty()) return false;
    Inst result(words.begin(), words.begin() + 5);
    result[3] = next;
    bool declared = declarations.empty();
    for (const auto& i : out) {
        if ((i[0] & 65535u) == 54 && !declared) {
            for (const auto& d : declarations) result.insert(result.end(), d.begin(), d.end());
            declared = true;
        }
        result.insert(result.end(), i.begin(), i.end());
    }
    if (!declared) return false;
    words = std::move(result);
    return true;
}

}  // namespace leonida::a8_red
