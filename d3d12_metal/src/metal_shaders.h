#pragma once

// Translated shaders as Metal libraries: port/shaderc_msl's store (manifest + content-addressed metallibs, the split
// recipe: one argument buffer per contract binding), and the same recipe run on demand for a shader the store does
// not have yet. Builds run on worker threads of this class (never a title thread), at most two tool processes at
// once, each `metal -c` under the recipe's own timeout. A pipeline whose shader is still building is created by the
// caller once on_ready runs; until then its draws and dispatches are skipped (counted), so no title thread waits for
// a compile and the title's 30 s hang watchdog is not fed by it.

#include "metal_context.h"

#include <gta6rt_shaderc_msl/catalog.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace leonida {

class MetalShaders {
public:
    struct Config {
        std::string manifest;     // port/build/shaderc_msl/runs/main/manifest.tsv
        std::string spirv_root;   // the corpus the store was built from (spike/s2-make-game/work/shaders/spirv)
        std::string spirv_fallback;  // a shader missing there is built from here (the layer's LEONIDA_SHADERS corpus)
        std::string store;        // where on-demand builds go (<store>/cas)
        std::string tool;         // port/shaderc_msl/tools/shaderc_msl.py
        unsigned builders = 2;
    };
    MetalShaders(gpu::Device& device, Config config);
    ~MetalShaders();
    MetalShaders(const MetalShaders&) = delete;
    MetalShaders& operator=(const MetalShaders&) = delete;

    struct Library {
        std::shared_ptr<gpu::ShaderLibrary> library;
        std::string entry;
    };
    // The library of shader `name` (index.json name, "library/..." for the pipeline library's), `stage` vs/ps/cs/ms.
    // A shader the store lacks is queued for a build and `ready` runs on a builder thread when it ends (with or
    // without a library); null library then, with `pending` set. `error` says why there is none otherwise.
    // `variant` "dual": a pixel shader built with the recipe's --dual-source pass (blend source 1 written at Location
    // 0 Index 1, for dual-source-blend PSOs); always an on-demand build, kept apart from the store's library.
    Library find(const std::string& name, const std::string& stage, std::function<void()> ready, bool* pending,
                 std::string* error, const std::string& variant = {});
    // A library the manifest names by its store-relative path (the split64 route's mesh and object libraries).
    Library load_relative(const std::string& relative, std::string* error);
    const gta6rt::shaderc_msl::ShaderEntry* manifest_entry(const std::string& name) const { return catalog_.find(name); }
    bool loaded() const { return catalog_loaded_; }

    struct Stats {
        std::uint64_t store_hits = 0, built = 0, build_failures = 0, queued = 0, libraries = 0;
        double build_seconds = 0, load_seconds = 0;
    };
    Stats stats() const;
    void stop(unsigned wait_ms);

private:
    struct Job {
        std::string name, stage, variant;
        std::vector<std::function<void()>> waiters;
    };
    Library load_locked(const std::string& path, std::string* error);
    void builder();
    bool build(const std::string& name, const std::string& stage, const std::string& variant, std::string& metallib,
               std::string& error);

    gpu::Device& device_;
    Config config_;
    gta6rt::shaderc_msl::Catalog catalog_;
    bool catalog_loaded_ = false;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::map<std::string, Library> by_path_;
    std::map<std::string, std::string> built_;   // name -> metallib path, on-demand builds
    std::map<std::string, std::string> failed_;  // name -> reason
    std::map<std::string, Job> jobs_;
    std::deque<std::string> order_;
    std::vector<std::thread> threads_;
    bool stopping_ = false;
    Stats stats_;
};

}  // namespace leonida
