#pragma once

// Pipeline cache for the native Metal path (port/d3d12_metal/cache, STATUS-cache.md).
//
// Recorder: every pipeline the title uses is written to a manifest in first-use order with its phase and time, keyed
// by a canonical text of everything the backend pipeline is made from (codec below). Manifests merge across runs.
// Prebuild: at start-up the manifest's pipelines are created on background compile slots (never a title thread) into
// the backend's MTLBinaryArchive; a pipeline the title then asks for is handed over (adopted) instead of compiled.
// prepare_shaders builds the same archive offline from the manifest (tools/prepare_shaders.mm).
//
// GTA6RT_METAL_PCACHE=0: no recorder, no prebuild; the make_* calls create exactly what they are given.

#include <gta6rt_metal/gpu.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gta6rt::pcache {

enum class Kind : std::uint8_t { render = 'R', mesh = 'M', compute = 'C' };

enum class Phase : std::uint8_t { boot = 0, landing, movie, first_run, story_load, world, count };
const char* phase_name(Phase p);
std::optional<Phase> phase_from_name(const std::string& s);

// Shader name -> the store's content key ("-" when the store does not have it).
using ContentKeyFn = std::function<std::string(const std::string& name)>;

// One pipeline as data: kind, shader names (stage 0: vs/ms/cs, stage 1: ps or empty) and the backend description
// without libraries, root layout, compile mode, priority or label.
struct Spec {
    Kind kind = Kind::render;
    std::string names[2];
    std::string content[2];
    gpu::RenderPipelineDesc render;
    gpu::MeshPipelineDesc mesh;
    gpu::ComputePipelineDesc compute;
};

std::string encode(const Spec& spec);
bool decode(const std::string& text, Spec& out, std::string* error = nullptr);
std::string key_of(const std::string& text);  // 16 hex digits, FNV-1a 64 of the text

Spec spec_of(const gpu::RenderPipelineDesc& d, const std::string& vs, const std::string& ps, const ContentKeyFn& ck);
Spec spec_of(const gpu::MeshPipelineDesc& d, const std::string& ms, const std::string& ps, const ContentKeyFn& ck);
Spec spec_of(const gpu::ComputePipelineDesc& d, const std::string& cs, const ContentKeyFn& ck);

// ---- manifest ----

struct Entry {
    std::string key;
    Phase phase = Phase::boot;
    double t = 0;            // seconds since process start at first use (in the run that gave `phase`)
    std::uint32_t runs = 1;  // runs that used it
    std::string text;        // encode(Spec)
};

class Manifest {
public:
    static constexpr const char* kHeader = "# gta6rt pcache manifest 1";
    bool load(const std::string& path, std::string* error = nullptr);
    bool save(const std::string& path, std::string* error = nullptr) const;  // tmp + fsync + rename
    // Union; an entry keeps its earliest phase (and that run's time); runs add up when `new_run`.
    void merge(const Manifest& other, bool new_run);
    void note(const Entry& e);  // this run: first use only
    std::vector<Entry> ordered() const;  // by phase, then time
    std::size_t size() const { return entries_.size(); }
    const Entry* find(const std::string& key) const;

private:
    std::vector<Entry> entries_;
    std::vector<std::pair<std::string, std::size_t>> index_;  // sorted by key
    std::size_t lookup(const std::string& key) const;
    void reindex();
};

// Archive key part this module adds: shader translator/recipe fingerprint and catalog format. The backend adds
// GPU name, OS version and build, and process arch (port/metal open_pipeline_cache).
std::string archive_version(const std::string& port_root, const std::string& catalog_manifest);

// Opens (or creates) the archive in `directory`. An archive file that exists with this key but that Metal cannot
// read (torn, from another OS build under the same name, edited) is moved aside to <file>.unreadable and a new one is
// started, so a bad file is never used and never stops caching. `log` gets one line saying what happened.
gpu::Status open_archive(gpu::Device& device, const std::string& directory, const std::string& version, bool load,
                         std::string* log);

// device.flush_pipeline_cache, once more only while no flush of this process has failed: after a failed serialize
// (MDB_MAP_FULL past about 180-250 MB, STATUS-cache.md) Metal's in-memory archive is damaged and a later serialize
// writes a fraction of it over the last good file.
// when_idle: skip (Status::not_ready) while the backend's compile queue has work, for at most 120 s since the last
// flush: a serialize holds the backend's archive lock for its whole duration (3.7-7.5 s for 100 MB in the title,
// up to 205 s for 58 MB offline next to running compiles), and every compile needs that lock.
gpu::Status flush_archive(gpu::Device& device, std::string* error, bool when_idle = false);

// ---- in-process recorder and prebuild (the D3D12 layer's hooks) ----

struct Library {
    gpu::ShaderLibrary* library = nullptr;  // owned by the caller's store, alive for the process
    std::string entry;
    bool pending = false;  // being built; `ready` runs later
    std::string error;
};
// Name + stage ("vs", "ps", "ms", "cs") -> library; ready() is called once a pending build ends.
using LibraryFn = std::function<Library(const std::string& name, const char* stage, std::function<void()> ready)>;

struct StartConfig {
    gpu::Device* device = nullptr;
    const gpu::RootLayout* root = nullptr;
    LibraryFn libraries;
    ContentKeyFn content_key;
    std::string directory;        // beside the archive: pcache-manifest.txt, pcache-bootstrap.txt
    unsigned max_in_flight = 2;   // background compiles at once (the backend has 3 compile threads)
};

bool enabled();
void start(const StartConfig& config);
void stop();
void flush(const char* why);  // writes the manifest now (also every 10 s and at every phase change)

// PSO creation (bootstrap triggers: the Vulkan boot manifest's PSO keys, census pipeline names). Cheap when unmatched.
void created(const void* owner, const std::string& pso_key, const std::string& label, const gpu::RenderPipelineDesc& d,
             const std::string& vs, const std::string& ps);
void created(const void* owner, const std::string& pso_key, const std::string& label, const gpu::MeshPipelineDesc& d,
             const std::string& ms, const std::string& ps);
void created(const void* owner, const std::string& pso_key, const std::string& label, const gpu::ComputePipelineDesc& d,
             const std::string& cs);

// First use (ensure_built / ensure_rect): a prebuilt pipeline when there is one (pushed to the front of the compile
// queue if it is not ready), else device.create_*(desc). Records the first use either way.
std::unique_ptr<gpu::RenderPipeline> make_render(gpu::Device& device, const gpu::RenderPipelineDesc& desc,
                                                 const void* owner, const std::string& vs, const std::string& ps);
std::unique_ptr<gpu::MeshPipeline> make_mesh(gpu::Device& device, const gpu::MeshPipelineDesc& desc, const void* owner,
                                             const std::string& ms, const std::string& ps);
std::unique_ptr<gpu::ComputePipeline> make_compute(gpu::Device& device, const gpu::ComputePipelineDesc& desc,
                                                   const void* owner, const std::string& cs);
// The owner (a PSO) is going away: its pipelines leave the stall watch.
void forget(const void* owner);

struct Counters {
    std::uint64_t first_uses = 0, adopted_ready = 0, adopted_compiling = 0, created = 0, prebuilt = 0,
                  prebuilt_from_archive = 0, prebuild_failed = 0, bootstrap_matched = 0, waits = 0, queued = 0;
    double wait_longest = 0, wait_sum = 0;
};
Counters counters();
std::string report();

}  // namespace gta6rt::pcache
