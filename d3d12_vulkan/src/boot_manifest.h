#pragma once
// The pipelines a boot used, in the order of their first use, kept beside the pipeline cache (boot_manifest.txt in
// LEONIDA_PIPELINE_CACHE_DIR). The next boot queues each one's build as soon as the title creates its PSO, ahead of
// speculative warm-up and in that order, instead of at its first draw or dispatch. A line is "c <PSO key>" for a
// compute pipeline or "g <PSO key> <unbound mask, hex>" for a graphics variant; the key is the PSO description's
// sha256 (pipeline_key), stable across boots. The file written keeps the loaded entries in their order and appends
// this boot's new first uses, so a short run does not forget a longer one.
// LEONIDA_BOOT_MANIFEST=0: neither read nor written.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace leonida {

class BootManifest {
public:
    struct Plan {
        std::size_t order = 0;
        std::vector<std::uint32_t> graphics_variants;
        bool compute = false;
    };

    static BootManifest& get() {
        static BootManifest* m = new BootManifest;
        return *m;
    }

    static bool enabled() {
        static const bool on = [] {
            const char* v = std::getenv("LEONIDA_BOOT_MANIFEST");
            return !v || v[0] != '0';
        }();
        return on;
    }

    // Once, from the first device: reads `path` if it is there and starts the periodic writer.
    void open(const std::filesystem::path& path) {
        if (!enabled() || path.empty()) return;
        std::call_once(open_, [&] {
            std::lock_guard lock(mutex_);
            path_ = path;
            std::ifstream in(path_);
            std::string line;
            if (in && std::getline(in, line) && line == kHeader) {
                while (std::getline(in, line)) {
                    if (line.size() < 3 || (line[0] != 'c' && line[0] != 'g') || line[1] != ' ') continue;
                    const std::size_t space = line.find(' ', 2);
                    const std::string key = line.substr(2, space == std::string::npos ? std::string::npos : space - 2);
                    Plan& p = plans_[key];
                    if (!p.order) p.order = ++order_;
                    if (line[0] == 'c') {
                        p.compute = true;
                    } else if (space != std::string::npos) {
                        p.graphics_variants.push_back(std::uint32_t(std::strtoul(line.c_str() + space + 1, nullptr, 16)));
                    }
                    previous_.push_back(line);
                }
                loaded_ = true;
            } else if (in) {
                unreadable_ = true;
            }
            std::fprintf(stderr, "[rt] d3d12: boot manifest %s: %s\n", path_.string().c_str(),
                         loaded_       ? (std::to_string(previous_.size()) + " pipelines in first-use order").c_str()
                         : unreadable_ ? "not one of ours, left as it is (not overwritten)"
                                       : "none yet (this boot writes one)");
            std::thread([this] {
                for (;;) {
                    std::this_thread::sleep_for(std::chrono::seconds(20));
                    write();
                }
            }).detach();
        });
    }

    // The manifest's plan for a PSO key, or null.
    const Plan* plan(const std::string& key) {
        std::lock_guard lock(mutex_);
        const auto it = plans_.find(key);
        if (it == plans_.end()) return nullptr;
        ++planned_;
        return &it->second;
    }

    // A pipeline's first demanded use this boot.
    void note(bool compute, const std::string& key, std::uint32_t unbound) {
        if (!enabled() || key.empty()) return;
        char line[160];
        if (compute) std::snprintf(line, sizeof line, "c %s", key.c_str());
        else std::snprintf(line, sizeof line, "g %s %x", key.c_str(), unbound);
        std::lock_guard lock(mutex_);
        if (!seen_.insert(line).second) return;
        recorded_.push_back(line);
        dirty_ = true;
    }

    std::uint64_t planned() {
        std::lock_guard lock(mutex_);
        return planned_;
    }

    // Writes the file (atomically, through a temporary) if anything is new. False when nothing was written.
    bool write() {
        std::vector<std::string> lines;
        std::filesystem::path path;
        {
            std::lock_guard lock(mutex_);
            if (!dirty_ || path_.empty() || unreadable_) return false;
            // The loaded order first (the pipelines this boot found there were built before their first use, so
            // this boot did not note them), then this boot's new first uses in their order.
            lines = previous_;
            const std::unordered_set<std::string> known(previous_.begin(), previous_.end());
            for (const std::string& line : recorded_)
                if (!known.count(line)) lines.push_back(line);
            path = path_;
            dirty_ = false;
        }
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        const auto temporary = path.string() + ".tmp";
        {
            std::ofstream out(temporary, std::ios::trunc);
            if (!out) return false;
            out << kHeader << '\n';
            for (const std::string& line : lines) out << line << '\n';
            if (!out) return false;
        }
        std::filesystem::rename(temporary, path, ec);
        if (ec) return false;
        written_.store(lines.size(), std::memory_order_relaxed);
        return true;
    }
    std::size_t written() const { return written_.load(std::memory_order_relaxed); }

private:
    static constexpr const char* kHeader = "# leonida boot manifest 1";
    std::once_flag open_;
    std::mutex mutex_;
    std::filesystem::path path_;
    std::unordered_map<std::string, Plan> plans_;
    std::vector<std::string> previous_, recorded_;
    std::unordered_set<std::string> seen_;
    std::size_t order_ = 0;
    std::uint64_t planned_ = 0;
    bool loaded_ = false, unreadable_ = false, dirty_ = false;
    std::atomic<std::size_t> written_{0};
};

}  // namespace leonida
