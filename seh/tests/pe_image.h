#pragma once
// Minimal PE32+ mapper for the seh tests (sections, base relocations, imports through a callback, exports).

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pe {

inline uint32_t u32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
inline uint16_t u16(const uint8_t* p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

struct image {
    uint8_t* mem = nullptr;
    uint64_t base = 0, preferred = 0;
    uint32_t size = 0;
    const uint8_t* opt = nullptr;
    uint32_t dir_rva(int i) const { return u32(opt + 112 + 8 * i); }
    uint32_t dir_size(int i) const { return u32(opt + 112 + 8 * i + 4); }
    uint32_t entry_rva() const { return u32(opt + 16); }
};

// Maps at want (0 = the preferred base, ~0 = anywhere). Returns false with a message on failure.
inline bool map(const char* path, uint64_t want, image* out) {
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        return false;
    }
    struct stat st;
    fstat(fd, &st);
    auto* raw = static_cast<uint8_t*>(mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0));
    close(fd);
    if (raw == MAP_FAILED) return false;
    const uint8_t* nt = raw + u32(raw + 0x3c);
    const uint8_t* oh = nt + 24;
    uint64_t pref;
    memcpy(&pref, oh + 24, 8);
    const uint32_t size = u32(oh + 56), hdr = u32(oh + 60);
    const uint16_t nsec = u16(nt + 6), ohsz = u16(nt + 20);
    void* hint = want == ~0ull ? nullptr : reinterpret_cast<void*>(want ? want : pref);
    auto* mem = static_cast<uint8_t*>(mmap(hint, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0));
    if (mem == MAP_FAILED || (hint && mem != hint)) {
        fprintf(stderr, "%s: could not map %#x bytes at %p (got %p)\n", path, size, hint, mem);
        if (mem != MAP_FAILED) munmap(mem, size);
        munmap(raw, static_cast<size_t>(st.st_size));
        return false;
    }
    memcpy(mem, raw, hdr);
    const uint8_t* sh = oh + ohsz;
    for (int i = 0; i < nsec; ++i, sh += 40) {
        const uint32_t vsz = u32(sh + 8), va = u32(sh + 12), rsz = u32(sh + 16), rp = u32(sh + 20);
        memcpy(mem + va, raw + rp, rsz < vsz ? rsz : vsz);
    }
    munmap(raw, static_cast<size_t>(st.st_size));
    out->mem = mem;
    out->base = reinterpret_cast<uint64_t>(mem);
    out->preferred = pref;
    out->size = size;
    const uint8_t* mnt = mem + u32(mem + 0x3c);
    out->opt = mnt + 24;
    return true;
}

inline void relocate(image& im) {
    const int64_t delta = static_cast<int64_t>(im.base - im.preferred);
    if (!delta) return;
    uint32_t off = im.dir_rva(5);
    const uint32_t end = off + im.dir_size(5);
    while (off + 8 <= end) {
        const uint32_t page = u32(im.mem + off), bsz = u32(im.mem + off + 4);
        if (!bsz) break;
        for (uint32_t i = 8; i + 2 <= bsz; i += 2) {
            const uint16_t e = u16(im.mem + off + i);
            if ((e >> 12) == 10) {
                uint64_t v;
                memcpy(&v, im.mem + page + (e & 0xfff), 8);
                v += static_cast<uint64_t>(delta);
                memcpy(im.mem + page + (e & 0xfff), &v, 8);
            }
        }
        off += bsz;
    }
    uint64_t b = im.base;
    memcpy(const_cast<uint8_t*>(im.opt) + 24, &b, 8);
}

// resolver(dll, name, ordinal) -> address; 0 leaves the slot pointing at trap(dll, name).
template <class R, class T>
inline void bind_imports(image& im, R resolver, T trap) {
    uint32_t d = im.dir_rva(1);
    if (!d) return;
    for (;; d += 20) {
        const uint32_t ilt = u32(im.mem + d), name = u32(im.mem + d + 12), iat = u32(im.mem + d + 16);
        if (!name) break;
        const char* dll = reinterpret_cast<const char*>(im.mem + name);
        const uint32_t lookup = ilt ? ilt : iat;
        for (uint32_t k = 0;; ++k) {
            uint64_t ent;
            memcpy(&ent, im.mem + lookup + 8 * k, 8);
            if (!ent) break;
            const char* fn = nullptr;
            uint32_t ord = 0;
            if (ent >> 63) ord = static_cast<uint32_t>(ent & 0xffff);
            else fn = reinterpret_cast<const char*>(im.mem + static_cast<uint32_t>(ent) + 2);
            uint64_t a = resolver(dll, fn, ord);
            if (!a) a = trap(dll, fn);
            memcpy(im.mem + iat + 8 * k, &a, 8);
        }
    }
}

inline uint64_t export_addr(const image& im, const char* want) {
    const uint32_t d = im.dir_rva(0);
    if (!d) return 0;
    const uint8_t* e = im.mem + d;
    const uint32_t nnames = u32(e + 24), funcs = u32(e + 28), names = u32(e + 32), ords = u32(e + 36);
    for (uint32_t i = 0; i < nnames; ++i) {
        const char* n = reinterpret_cast<const char*>(im.mem + u32(im.mem + names + 4 * i));
        if (strcmp(n, want)) continue;
        const uint16_t o = u16(im.mem + ords + 2 * i);
        return im.base + u32(im.mem + funcs + 4 * o);
    }
    return 0;
}

}  // namespace pe
