#pragma once
// Links one x86-64 COFF object (clang -target x86_64-pc-windows-msvc) into an executable in-memory guest image:
// sections laid out at RVAs, ADDR64/ADDR32NB/REL32 relocations applied, .pdata merged into one sorted table.
// External functions referenced by REL32/ADDR32NB go through in-image jmp [rip] thunks (so handler RVAs in
// .xdata stay inside the image); __imp_X symbols become IAT slots; ADDR64 data references point at the target.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <gta6rt/seh.h>

namespace coff {

struct image {
    uint8_t* mem = nullptr;
    uint64_t base = 0;
    uint32_t size = 0;
    const gta6rt_runtime_function* pdata = nullptr;
    uint32_t pdata_count = 0;
    std::map<std::string, uint64_t> defined;
    uint64_t sym(const std::string& n) const {
        auto it = defined.find(n);
        return it == defined.end() ? 0 : it->second;
    }
};

inline bool read_file(const char* path, std::vector<uint8_t>* out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    out->resize(static_cast<size_t>(ftell(f)));
    fseek(f, 0, SEEK_SET);
    const bool ok = fread(out->data(), 1, out->size(), f) == out->size();
    fclose(f);
    return ok;
}

template <class T>
T rd(const uint8_t* p) {
    T v;
    memcpy(&v, p, sizeof v);
    return v;
}

// resolver(name) returns the address of an external symbol, or 0 (link error).
inline bool link(const char* path, const std::function<uint64_t(const std::string&)>& resolver, image* out,
                 std::string* err) {
    std::vector<uint8_t> f;
    if (!read_file(path, &f)) {
        *err = std::string("cannot read ") + path;
        return false;
    }
    const uint8_t* h = f.data();
    if (rd<uint16_t>(h) != 0x8664) {
        *err = "not an x86-64 COFF object";
        return false;
    }
    const uint16_t nsec = rd<uint16_t>(h + 2);
    const uint32_t symtab = rd<uint32_t>(h + 8), nsym = rd<uint32_t>(h + 12);
    const uint16_t optsz = rd<uint16_t>(h + 16);
    const uint8_t* strtab = h + symtab + nsym * 18;
    auto sec_name = [&](const uint8_t* s) {
        char n[9] = {};
        memcpy(n, s, 8);
        if (n[0] == '/') return std::string(reinterpret_cast<const char*>(strtab + atoi(n + 1)));
        return std::string(n);
    };
    auto sym_name = [&](const uint8_t* s) {
        if (rd<uint32_t>(s) == 0) return std::string(reinterpret_cast<const char*>(strtab + rd<uint32_t>(s + 4)));
        char n[9] = {};
        memcpy(n, s, 8);
        return std::string(n);
    };

    struct sec {
        std::string name;
        const uint8_t* hdr;
        uint32_t rva = 0, size = 0;
        bool keep = false;
    };
    std::vector<sec> secs(nsec);
    uint32_t cursor = 0x1000;
    for (uint16_t i = 0; i < nsec; ++i) {
        sec& s = secs[i];
        s.hdr = h + 20 + optsz + 40 * i;
        s.name = sec_name(s.hdr);
        const uint32_t ch = rd<uint32_t>(s.hdr + 36);
        s.size = rd<uint32_t>(s.hdr + 16);
        s.keep = !(ch & 0x00000a00u) && s.name.rfind(".debug", 0) != 0 && s.name != ".llvm_addrsig";
        if (!s.keep) continue;
        const uint32_t align_code = (ch >> 20) & 0xf;
        const uint32_t align = align_code ? 1u << (align_code - 1) : 16;
        cursor = (cursor + align - 1) & ~(align - 1);
        s.rva = cursor;
        cursor += s.size;
    }
    cursor = (cursor + 0xfff) & ~0xfffu;
    const uint32_t thunk_area = cursor, thunk_cap = 512;
    cursor += thunk_cap * 16;
    const uint32_t iat_area = cursor;
    cursor += thunk_cap * 8;
    cursor = (cursor + 0xfff) & ~0xfffu;
    const uint32_t pdata_area = cursor;
    uint32_t npdata = 0;
    for (const sec& s : secs)
        if (s.keep && s.name == ".pdata") npdata += s.size / 12;
    cursor += (npdata * 12 + 0xfff) & ~0xfffu;
    const uint32_t total = cursor + 0x1000;

    auto* mem = static_cast<uint8_t*>(mmap(nullptr, total, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0));
    if (mem == MAP_FAILED) {
        *err = "mmap failed";
        return false;
    }
    memset(mem, 0xcc, total);
    const uint64_t base = reinterpret_cast<uint64_t>(mem);
    for (const sec& s : secs) {
        if (!s.keep) continue;
        const uint32_t ch = rd<uint32_t>(s.hdr + 36);
        if (ch & 0x80) memset(mem + s.rva, 0, s.size);
        else memcpy(mem + s.rva, h + rd<uint32_t>(s.hdr + 20), s.size);
    }

    std::map<std::string, uint64_t> thunks, slots;
    uint32_t nthunk = 0, nslot = 0;
    auto thunk_for = [&](const std::string& n, uint64_t target) -> uint64_t {
        auto it = thunks.find(n);
        if (it != thunks.end()) return it->second;
        if (nthunk >= thunk_cap) return 0;
        uint8_t* t = mem + thunk_area + 16 * nthunk;
        t[0] = 0xff;
        t[1] = 0x25;
        memset(t + 2, 0, 4);
        memcpy(t + 6, &target, 8);
        const uint64_t a = base + thunk_area + 16 * nthunk++;
        thunks[n] = a;
        return a;
    };
    auto slot_for = [&](const std::string& n, uint64_t target) -> uint64_t {
        auto it = slots.find(n);
        if (it != slots.end()) return it->second;
        if (nslot >= thunk_cap) return 0;
        memcpy(mem + iat_area + 8 * nslot, &target, 8);
        const uint64_t a = base + iat_area + 8 * nslot++;
        slots[n] = a;
        return a;
    };

    struct symv {
        std::string name;
        int16_t section;
        uint32_t value;
        uint8_t storage;
    };
    std::vector<symv> syms(nsym);
    for (uint32_t i = 0; i < nsym; ++i) {
        const uint8_t* s = h + symtab + 18 * i;
        syms[i] = {sym_name(s), rd<int16_t>(s + 12), rd<uint32_t>(s + 8), s[16]};
        if (syms[i].section > 0 && secs[syms[i].section - 1].keep && (syms[i].storage == 2 || syms[i].storage == 3))
            out->defined[syms[i].name] = base + secs[syms[i].section - 1].rva + syms[i].value;
        const uint8_t aux = s[17];
        for (uint8_t k = 0; k < aux; ++k) syms[++i].name.clear();
    }

    for (const sec& s : secs) {
        if (!s.keep) continue;
        const uint32_t nrel = rd<uint16_t>(s.hdr + 32), prel = rd<uint32_t>(s.hdr + 24);
        for (uint32_t r = 0; r < nrel; ++r) {
            const uint8_t* rel = h + prel + 10 * r;
            const uint32_t off = rd<uint32_t>(rel), si = rd<uint32_t>(rel + 4);
            const uint16_t type = rd<uint16_t>(rel + 8);
            const symv& sy = syms[si];
            uint64_t S = 0;
            bool external = false;
            if (sy.section > 0) {
                if (!secs[sy.section - 1].keep) continue;
                S = base + secs[sy.section - 1].rva + sy.value;
            } else if (sy.section == -1) {
                S = sy.value;
            } else if (sy.name == "__ImageBase") {
                S = base;
            } else if (sy.name.rfind("__imp_", 0) == 0) {
                const uint64_t t = resolver(sy.name.substr(6));
                if (!t) {
                    *err = "unresolved " + sy.name;
                    return false;
                }
                S = slot_for(sy.name, t);
            } else {
                external = true;
                S = resolver(sy.name);
                if (!S) {
                    *err = "unresolved " + sy.name;
                    return false;
                }
            }
            uint8_t* P = mem + s.rva + off;
            const uint64_t pa = base + s.rva + off;
            switch (type) {
            case 1: {
                const uint64_t v = rd<uint64_t>(P) + S;
                memcpy(P, &v, 8);
                break;
            }
            case 3: {
                if (external) S = thunk_for(sy.name, S);
                const uint32_t v = rd<uint32_t>(P) + static_cast<uint32_t>(S - base);
                memcpy(P, &v, 4);
                break;
            }
            case 4:
            case 5:
            case 6:
            case 7:
            case 8:
            case 9: {
                if (external) S = thunk_for(sy.name, S);
                const int64_t v = static_cast<int64_t>(rd<int32_t>(P)) + static_cast<int64_t>(S) -
                                  static_cast<int64_t>(pa + 4 + (type - 4));
                if (v != static_cast<int32_t>(v)) {
                    *err = "REL32 out of range for " + sy.name;
                    return false;
                }
                const int32_t v32 = static_cast<int32_t>(v);
                memcpy(P, &v32, 4);
                break;
            }
            default: {
                char b[96];
                snprintf(b, sizeof b, "relocation type %u in %s not handled", type, s.name.c_str());
                *err = b;
                return false;
            }
            }
        }
    }

    std::vector<gta6rt_runtime_function> fns;
    for (const sec& s : secs)
        if (s.keep && s.name == ".pdata")
            for (uint32_t k = 0; k + 12 <= s.size; k += 12) fns.push_back(rd<gta6rt_runtime_function>(mem + s.rva + k));
    std::sort(fns.begin(), fns.end(), [](const auto& a, const auto& b) { return a.begin_rva < b.begin_rva; });
    memcpy(mem + pdata_area, fns.data(), fns.size() * 12);

    out->mem = mem;
    out->base = base;
    out->size = total;
    out->pdata = reinterpret_cast<const gta6rt_runtime_function*>(mem + pdata_area);
    out->pdata_count = static_cast<uint32_t>(fns.size());
    return true;
}

}  // namespace coff
