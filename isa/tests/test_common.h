#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <condition_variable>
#include <mutex>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_checks = 0, g_fails = 0;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        ++g_checks;                                            \
        if (!(cond)) {                                         \
            ++g_fails;                                         \
            if (g_fails <= 60) {                               \
                std::printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
                std::printf(__VA_ARGS__);                      \
                std::printf("\n");                             \
            }                                                  \
        }                                                      \
    } while (0)

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
};

// Maps the PE sections of `path` at their RVAs into fresh anonymous memory (RW). Returns base or nullptr.
struct MappedImage {
    uint8_t* base = nullptr;
    size_t size = 0;
    uint32_t text_rva = 0, text_size = 0;
};

inline bool map_pe(const char* path, MappedImage* out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    fstat(fd, &st);
    std::vector<uint8_t> f(static_cast<size_t>(st.st_size));
    size_t got = 0;
    while (got < f.size()) {
        ssize_t r = read(fd, f.data() + got, f.size() - got);
        if (r <= 0) break;
        got += static_cast<size_t>(r);
    }
    close(fd);
    if (got != f.size() || f.size() < 0x400) return false;
    uint32_t lfa;
    memcpy(&lfa, &f[0x3c], 4);
    uint16_t nsec, optsz;
    memcpy(&nsec, &f[lfa + 6], 2);
    memcpy(&optsz, &f[lfa + 20], 2);
    const size_t opt = lfa + 24;
    uint32_t soi, soh;
    memcpy(&soi, &f[opt + 56], 4);
    memcpy(&soh, &f[opt + 60], 4);
    void* m = mmap(nullptr, soi, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) return false;
    auto* b = static_cast<uint8_t*>(m);
    memcpy(b, f.data(), soh);
    const size_t sec = opt + optsz;
    for (int i = 0; i < nsec; ++i) {
        const uint8_t* s = &f[sec + 40 * i];
        uint32_t vsz, va, rsz, rptr;
        memcpy(&vsz, s + 8, 4);
        memcpy(&va, s + 12, 4);
        memcpy(&rsz, s + 16, 4);
        memcpy(&rptr, s + 20, 4);
        const size_t n = rsz < vsz ? rsz : vsz;
        if (rptr + n <= f.size()) memcpy(b + va, &f[rptr], n);
        if (!memcmp(s, ".text", 5)) {
            out->text_rva = va;
            out->text_size = vsz;
        }
    }
    out->base = b;
    out->size = soi;
    return true;
}

inline const char* exe_path(int argc, char** argv) {
    if (argc > 1) return argv[1];
    if (const char* e = getenv("GTA6RT_EXE")) return e;
    return GTA6RT_ISA_TEST_EXE;
}

inline int finish(const char* name) {
    if (g_fails) {
        std::printf("%s: FAIL (%d of %d checks failed)\n", name, g_fails, g_checks);
        return 1;
    }
    std::printf("%s: PASS (%d checks)\n", name, g_checks);
    return 0;
}

struct Barrier {
    std::mutex m;
    std::condition_variable cv;
    int n = 0, waiting = 0, gen = 0;
    void init(int count) { n = count; }
    void wait() {
        std::unique_lock<std::mutex> lk(m);
        const int g = gen;
        if (++waiting == n) {
            waiting = 0;
            ++gen;
            cv.notify_all();
            return;
        }
        cv.wait(lk, [&] { return gen != g; });
    }
};
