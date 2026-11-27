#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace iob {

enum Codec : uint8_t { kRaw = 0, kZlib = 1, kBcpackBase = 16 };

struct Req {
    uint64_t off = 0;
    uint32_t src = 0;
    uint32_t dst = 0;
    uint32_t file = 0;
    uint8_t codec = 0;
    uint8_t pad[3] = {};
    uint32_t group = 0;
};

struct Shape {
    std::vector<std::string> files;
    std::vector<Req> reqs;
};

bool shape_save(const std::string& path, const Shape& s);
bool shape_load(const std::string& path, Shape& s);
void scan_rpf_window(const std::string& path, uint32_t file_index, uint64_t start, uint64_t len, uint32_t group, Shape& out);
bool scan_ttpf(const std::string& path, uint32_t file_index, Shape& out);

enum class Order { Seq, Rand, Runs };
std::vector<Req> make_order(const std::vector<Req>& in, Order o, uint64_t seed, double mean_run);
std::vector<Req> uniform_requests(uint32_t file, uint64_t base, uint64_t span, uint32_t size, bool random, uint64_t seed, uint64_t total_bytes);

}  // namespace iob
