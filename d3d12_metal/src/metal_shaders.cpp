#include "metal_shaders.h"

#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <regex>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace leonida {

namespace {
double seconds_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}
bool exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && st.st_size > 0;
}
}  // namespace

MetalShaders::MetalShaders(gpu::Device& device, Config config) : device_(device), config_(std::move(config)) {
    std::string error;
    catalog_loaded_ = !config_.manifest.empty() && catalog_.load(config_.manifest, &error);
    if (catalog_loaded_)
        std::fprintf(stderr, "[rt] d3d12-metal: shader store: %zu metallibs from %s\n", catalog_.size(),
                     config_.manifest.c_str());
    else
        std::fprintf(stderr, "[rt] d3d12-metal: shader store not loaded (%s): every shader is built on demand\n",
                     error.empty() ? config_.manifest.c_str() : error.c_str());
    for (const std::string& path : config_.overlays) {
        gta6rt::shaderc_msl::Catalog o;
        std::string why;
        char a[PATH_MAX], b[PATH_MAX];
        if (!catalog_loaded_ || !o.load(path, &why))
            std::fprintf(stderr, "[rt] d3d12-metal: shader overlay %s NOT loaded: %s\n", path.c_str(),
                         catalog_loaded_ ? why.c_str() : "no store");
        else if (!::realpath(o.root().c_str(), a) || !::realpath(catalog_.root().c_str(), b) || std::strcmp(a, b))
            std::fprintf(stderr, "[rt] d3d12-metal: shader overlay %s NOT loaded: its root %s is not the store's %s\n",
                         path.c_str(), o.root().c_str(), catalog_.root().c_str());
        else {
            std::fprintf(stderr, "[rt] d3d12-metal: shader overlay: %zu rows from %s replace the store's\n", o.size(),
                         path.c_str());
            overlays_.push_back(std::move(o));
        }
    }
    if (!config_.store.empty()) ::mkdir(config_.store.c_str(), 0755);
    for (unsigned i = 0; i < std::max(1u, config_.builders); ++i) threads_.emplace_back([this] { builder(); });
}

MetalShaders::~MetalShaders() { stop(0); }

void MetalShaders::stop(unsigned) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
    }
    wake_.notify_all();
    for (auto& t : threads_)
        if (t.joinable()) t.join();
}

MetalShaders::Library MetalShaders::load_locked(const std::string& path, std::string* error) {
    if (auto it = by_path_.find(path); it != by_path_.end()) return it->second;
    const auto start = std::chrono::steady_clock::now();
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        if (error) *error = "metallib missing or empty: " + path;
        return {};
    }
    std::string why;
    auto lib = device_.create_library_metallib(bytes, &why);
    if (!lib) {
        if (error) *error = "Metal refused " + path + ": " + why;
        return {};
    }
    Library out;
    const auto names = lib->function_names();
    out.entry = names.empty() ? "main0" : names.front();
    for (const auto& n : names)
        if (n == "main0") out.entry = n;
    out.library = std::shared_ptr<gpu::ShaderLibrary>(std::move(lib));
    by_path_[path] = out;
    ++stats_.libraries;
    stats_.load_seconds += seconds_since(start);
    return out;
}

MetalShaders::Library MetalShaders::load_relative(const std::string& relative, std::string* error) {
    std::lock_guard lock(mutex_);
    if (!catalog_loaded_) {
        if (error) *error = "no shader store manifest";
        return {};
    }
    return load_locked(catalog_.path(relative), error);
}

MetalShaders::Library MetalShaders::find(const std::string& name, const std::string& stage,
                                         std::function<void()> ready, bool* pending, std::string* error,
                                         const std::string& variant) {
    if (pending) *pending = false;
    const std::string job_key = variant.empty() ? name : name + "|" + variant;
    std::lock_guard lock(mutex_);
    if (catalog_loaded_ && variant.empty())
        if (const auto* e = manifest_entry(name)) {
            ++stats_.store_hits;
            return load_locked(catalog_.library_path(*e), error);
        }
    if (auto it = built_.find(job_key); it != built_.end()) return load_locked(it->second, error);
    if (auto it = failed_.find(job_key); it != failed_.end()) {
        if (error) *error = it->second;
        return {};
    }
    if (stopping_ || config_.tool.empty()) {
        if (error) *error = "not in the shader store, and no builder";
        return {};
    }
    auto [it, fresh] = jobs_.try_emplace(job_key);
    if (fresh) {
        it->second.name = name;
        it->second.stage = stage;
        it->second.variant = variant;
        order_.push_back(job_key);
        ++stats_.queued;
        if (stats_.queued <= 8 || (stats_.queued & (stats_.queued - 1)) == 0)
            std::fprintf(stderr, "[rt] d3d12-metal: %s%s%s is not in the shader store: built on demand (%llu queued so far)\n",
                         name.c_str(), variant.empty() ? "" : " variant ", variant.c_str(),
                         static_cast<unsigned long long>(stats_.queued));
    }
    if (ready) it->second.waiters.push_back(std::move(ready));
    if (pending) *pending = true;
    wake_.notify_one();
    return {};
}

void MetalShaders::builder() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !order_.empty(); });
            if (stopping_) return;
            const std::string key = order_.front();
            order_.pop_front();
            auto it = jobs_.find(key);
            if (it == jobs_.end()) continue;
            job.name = it->second.name;
            job.stage = it->second.stage;
            job.variant = it->second.variant;
        }
        const std::string job_key = job.variant.empty() ? job.name : job.name + "|" + job.variant;
        std::string metallib, error;
        const auto start = std::chrono::steady_clock::now();
        const bool ok = build(job.name, job.stage, job.variant, metallib, error);
        std::vector<std::function<void()>> waiters;
        {
            std::lock_guard lock(mutex_);
            stats_.build_seconds += seconds_since(start);
            if (ok) {
                built_[job_key] = metallib;
                ++stats_.built;
            } else {
                failed_[job_key] = error;
                ++stats_.build_failures;
                std::fprintf(stderr, "[rt] d3d12-metal: on-demand build of %s failed: %s\n", job_key.c_str(),
                             error.c_str());
            }
            auto it = jobs_.find(job_key);
            if (it != jobs_.end()) {
                waiters = std::move(it->second.waiters);
                jobs_.erase(it);
            }
        }
        for (auto& w : waiters) w();
    }
}

bool MetalShaders::build(const std::string& name, const std::string& stage, const std::string& variant,
                         std::string& metallib, std::string& error) {
    std::string root = config_.spirv_root;
    std::string spv = root + "/" + name + ".spv";
    if (!exists(spv) && !config_.spirv_fallback.empty() && exists(config_.spirv_fallback + "/" + name + ".spv")) {
        root = config_.spirv_fallback;
        spv = root + "/" + name + ".spv";
    }
    if (!exists(spv)) {
        error = "no SPIR-V at " + spv;
        return false;
    }
    const std::string cas = config_.store + "/cas";
    const std::string out = config_.store + "/build-" + std::to_string(::getpid()) + "-" +
                            std::to_string(std::hash<std::string>{}(name + "|" + variant)) + ".json";
    const char* python = "/usr/bin/env";
    std::vector<std::string> args = {"env", "PYTHONDONTWRITEBYTECODE=1", "python3", "-I", config_.tool, spv,
                                     "--stage", stage, "--cas", cas, "--spirv-dir", root};
    if (variant == "dual") args.push_back("--dual-source");
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 1, out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, python, &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) {
        error = std::string("posix_spawn: ") + std::strerror(rc);
        return false;
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    std::ifstream in(out);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ::unlink(out.c_str());
    std::smatch m;
    static const std::regex key("\"key\": \"([0-9a-f]{64})\"");
    if (!std::regex_search(text, m, key)) {
        error = "the recipe printed no key (exit " + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) + ")";
        return false;
    }
    const std::string k = m[1].str();
    metallib = cas + "/" + k.substr(0, 2) + "/" + k + ".metallib";
    if (!exists(metallib)) {
        static const std::regex reason("\"error\": \"([^\"]{0,200})");
        error = std::regex_search(text, m, reason) ? m[1].str() : "no metallib after the recipe ran";
        return false;
    }
    return true;
}

MetalShaders::Stats MetalShaders::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

}  // namespace leonida
