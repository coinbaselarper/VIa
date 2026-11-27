#pragma once
#include <string>
#include <vector>

#include "common.h"
#include "shapes.h"

namespace iob {

struct ReadCfg {
    std::string backend = "pread";
    int threads = 4;
    bool nocache = false;
    int rdahead = -1;
    int advise = 0;
    size_t split = 0;
    std::string madv = "none";
    size_t dio_hw = 0;
    bool evict_behind = false;
    bool useful_in_dst = false;
    bool evict_before = true;
    bool evict_after = true;
};

struct ReadResult {
    uint64_t bytes = 0, read_bytes = 0, n = 0, errors = 0;
    double wall = 0;
    std::vector<double> lat_ms;
    Cpu cpu;
    VmSnap vm0, vm1;
    double resident_before = 0;
    double evict_s = 0;
};

ReadResult run_reads(const std::vector<std::string>& files, const std::vector<Req>& reqs, const ReadCfg& cfg);
void evict_requests(const std::vector<std::string>& files, const std::vector<Req>& reqs);
void print_read_result(const char* tag, const ReadCfg& cfg, const ReadResult& r, const char* extra);

}  // namespace iob
