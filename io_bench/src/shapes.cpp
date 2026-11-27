#include "shapes.h"

#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>

#include "common.h"

namespace iob {

namespace {
constexpr char kMagic[8] = {'I', 'O', 'B', 'S', 'H', 'P', '0', '1'};
}

bool shape_save(const std::string& path, const Shape& s) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    uint32_t nf = static_cast<uint32_t>(s.files.size());
    uint64_t nr = s.reqs.size();
    std::fwrite(kMagic, 1, 8, f);
    std::fwrite(&nf, 4, 1, f);
    std::fwrite(&nr, 8, 1, f);
    for (const auto& p : s.files) {
        uint32_t l = static_cast<uint32_t>(p.size());
        std::fwrite(&l, 4, 1, f);
        std::fwrite(p.data(), 1, l, f);
    }
    std::fwrite(s.reqs.data(), sizeof(Req), s.reqs.size(), f);
    return std::fclose(f) == 0;
}

bool shape_load(const std::string& path, Shape& s) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char m[8];
    uint32_t nf = 0;
    uint64_t nr = 0;
    bool ok = std::fread(m, 1, 8, f) == 8 && !std::memcmp(m, kMagic, 8) && std::fread(&nf, 4, 1, f) == 1 && std::fread(&nr, 8, 1, f) == 1;
    s = Shape{};
    for (uint32_t i = 0; ok && i < nf; ++i) {
        uint32_t l = 0;
        ok = std::fread(&l, 4, 1, f) == 1 && l < 4096;
        std::string p(ok ? l : 0, '\0');
        ok = ok && std::fread(p.data(), 1, l, f) == l;
        s.files.push_back(p);
    }
    if (ok) {
        s.reqs.resize(nr);
        ok = std::fread(s.reqs.data(), sizeof(Req), nr, f) == nr;
    }
    std::fclose(f);
    return ok;
}

static bool probe_stream(const uint8_t* p, size_t avail, size_t& used, size_t& produced) {
    static thread_local std::vector<uint8_t> sink(1 << 20);
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) return false;
    zs.next_in = const_cast<uint8_t*>(p);
    zs.avail_in = static_cast<uInt>(std::min<size_t>(avail, 0x7fffffff));
    int rc;
    do {
        zs.next_out = sink.data();
        zs.avail_out = static_cast<uInt>(sink.size());
        rc = inflate(&zs, Z_NO_FLUSH);
    } while (rc == Z_OK && zs.total_out < (1ull << 30));
    used = zs.total_in;
    produced = zs.total_out;
    inflateEnd(&zs);
    return rc == Z_STREAM_END;
}

void scan_rpf_window(const std::string& path, uint32_t file_index, uint64_t start, uint64_t len, uint32_t group, Shape& out) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return;
    fcntl(fd, F_NOCACHE, 1);
    const uint64_t fsz = file_size(fd);
    const uint64_t tail = 64ull << 20;
    uint64_t total = std::min<uint64_t>(len + tail, fsz > start ? fsz - start : 0);
    std::vector<uint8_t> buf(total);
    size_t got = 0;
    while (got < total) {
        ssize_t n = pread(fd, buf.data() + got, total - got, static_cast<off_t>(start + got));
        if (n <= 0) break;
        got += static_cast<size_t>(n);
    }
    close(fd);
    size_t pos = 0;
    const size_t limit = std::min<uint64_t>(len, got);
    while (pos < limit) {
        const uint8_t* q = static_cast<const uint8_t*>(std::memchr(buf.data() + pos, 0x78, limit - pos));
        if (!q) break;
        pos = static_cast<size_t>(q - buf.data());
        const uint8_t b1 = pos + 1 < got ? buf[pos + 1] : 0;
        if (b1 == 0x01 || b1 == 0x5e || b1 == 0x9c || b1 == 0xda) {
            size_t used = 0, produced = 0;
            if (probe_stream(buf.data() + pos, got - pos, used, produced) && used >= 6 && produced > 0) {
                Req r;
                r.off = start + pos;
                r.src = static_cast<uint32_t>(used);
                r.dst = static_cast<uint32_t>(std::min<size_t>(produced, 0xffffffffu));
                r.file = file_index;
                r.codec = kZlib;
                r.group = group;
                out.reqs.push_back(r);
                pos += used;
                continue;
            }
        }
        ++pos;
    }
}

bool scan_ttpf(const std::string& path, uint32_t file_index, Shape& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t h[0x18];
    if (std::fread(h, 1, sizeof h, f) != sizeof h || std::memcmp(h, "RDS5", 4)) {
        std::fclose(f);
        return false;
    }
    uint32_t count;
    std::memcpy(&count, h + 4, 4);
    if (count < 3 || count > (1u << 24)) {
        std::fclose(f);
        return false;
    }
    std::vector<uint64_t> e(count - 1);
    bool ok = std::fread(e.data(), 8, e.size(), f) == e.size();
    std::fclose(f);
    if (!ok) return false;
    for (size_t i = 0; i + 1 < e.size(); ++i) {
        const uint64_t a = e[i], b = e[i + 1];
        const uint64_t po = (a & 0x3ffffff) * 256, pn = (b & 0x3ffffff) * 256;
        const unsigned codec = static_cast<unsigned>((a >> 26) & 15);
        const uint64_t vo = ((a >> 30) & 0x3fffff) * 4096, vn = ((b >> 30) & 0x3fffff) * 4096;
        const uint64_t pad = a >> 52;
        if (pn <= po + pad) continue;
        Req r;
        r.off = po;
        r.src = static_cast<uint32_t>(pn - po - pad);
        r.dst = static_cast<uint32_t>(vn > vo ? vn - vo : r.src);
        r.file = file_index;
        r.codec = codec == 8 ? kZlib : (codec == 0 ? kRaw : static_cast<uint8_t>(kBcpackBase + codec));
        r.group = static_cast<uint32_t>(i);
        out.reqs.push_back(r);
    }
    return true;
}

std::vector<Req> make_order(const std::vector<Req>& in, Order o, uint64_t seed, double mean_run) {
    std::vector<Req> v = in;
    if (o == Order::Seq) return v;
    std::mt19937_64 rng(seed);
    if (o == Order::Rand) {
        std::shuffle(v.begin(), v.end(), rng);
        return v;
    }
    std::vector<Req> out;
    out.reserve(v.size());
    std::vector<size_t> starts;
    std::geometric_distribution<size_t> run(1.0 / std::max(1.0, mean_run));
    std::vector<std::pair<size_t, size_t>> runs;
    for (size_t i = 0; i < v.size();) {
        size_t l = 1 + run(rng);
        l = std::min(l, v.size() - i);
        runs.push_back({i, l});
        i += l;
    }
    std::shuffle(runs.begin(), runs.end(), rng);
    for (auto [s, l] : runs)
        for (size_t k = 0; k < l; ++k) out.push_back(v[s + k]);
    return out;
}

std::vector<Req> uniform_requests(uint32_t file, uint64_t base, uint64_t span, uint32_t size, bool random, uint64_t seed, uint64_t total_bytes) {
    std::vector<Req> v;
    const uint64_t slots = span / size;
    const uint64_t want = std::min<uint64_t>(slots, total_bytes / size);
    std::vector<uint64_t> idx;
    std::mt19937_64 rng(seed);
    if (!random) {
        for (uint64_t i = 0; i < want; ++i) idx.push_back(i);
    } else if (slots <= 2 * want) {
        for (uint64_t i = 0; i < slots; ++i) idx.push_back(i);
        std::shuffle(idx.begin(), idx.end(), rng);
        idx.resize(want);
    } else {
        std::uniform_int_distribution<uint64_t> d(0, slots - 1);
        for (uint64_t i = 0; i < want; ++i) idx.push_back(d(rng));
        std::sort(idx.begin(), idx.end());
        idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
        std::shuffle(idx.begin(), idx.end(), rng);
    }
    v.reserve(idx.size());
    for (uint64_t i : idx) {
        Req r;
        r.off = base + i * size;
        r.src = r.dst = size;
        r.file = file;
        v.push_back(r);
    }
    return v;
}

}  // namespace iob
