#pragma once

// Backend-neutral host GPU interface that the D3D12 semantics layer drives (DESIGN.md section 2).
// Split after victory-gpu's host modules: device (queues, fences, memory, present), descriptor (host
// bindless tables), root (the translated-shader contract), pipeline (state + translated shaders).
// No Metal, Vulkan or Windows types: the Metal implementation lives behind gta6rt::metal::create_device.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gta6rt::gpu {

inline constexpr std::uint32_t api_version = 4;  // 3: texture views; 4: texture copies, 32-bit fills (port/d3d12_metal)

enum class Status : std::int32_t {
    ok = 0,
    invalid_argument = -1,
    unsupported = -2,
    out_of_memory = -3,
    compile_failed = -4,
    pipeline_failed = -5,
    device_lost = -6,
    timeout = -7,
    not_ready = -8,
    not_implemented = -9,
    limit_exceeded = -10,  // the shader exceeds a Metal limit; Pipeline::limit() says which (fall back, e.g. mesh-as-compute)
};
const char* status_name(Status s);

// DXGI_FORMAT numbering (dxgiformat.h), so the D3D12 layer passes its values through.
namespace format {
inline constexpr std::uint32_t unknown = 0;
inline constexpr std::uint32_t r32g32b32a32_float = 2;
inline constexpr std::uint32_t r32g32b32a32_uint = 3;
inline constexpr std::uint32_t r32g32b32_float = 6;
inline constexpr std::uint32_t r16g16b16a16_float = 10;
inline constexpr std::uint32_t r16g16b16a16_unorm = 11;
inline constexpr std::uint32_t r32g32_float = 16;
inline constexpr std::uint32_t d32_float_s8x24_uint = 20;
inline constexpr std::uint32_t r10g10b10a2_unorm = 24;
inline constexpr std::uint32_t r11g11b10_float = 26;
inline constexpr std::uint32_t r8g8b8a8_unorm = 28;
inline constexpr std::uint32_t r8g8b8a8_unorm_srgb = 29;
inline constexpr std::uint32_t r8g8b8a8_uint = 30;
inline constexpr std::uint32_t r16g16_float = 34;
inline constexpr std::uint32_t r16g16_unorm = 35;
inline constexpr std::uint32_t r16g16_uint = 36;
inline constexpr std::uint32_t r32_typeless = 39;
inline constexpr std::uint32_t d32_float = 40;
inline constexpr std::uint32_t r32_float = 41;
inline constexpr std::uint32_t r32_uint = 42;
inline constexpr std::uint32_t d24_unorm_s8_uint = 45;
inline constexpr std::uint32_t r8g8_unorm = 49;
inline constexpr std::uint32_t r16_float = 54;
inline constexpr std::uint32_t d16_unorm = 55;
inline constexpr std::uint32_t r16_unorm = 56;
inline constexpr std::uint32_t r16_uint = 57;
inline constexpr std::uint32_t r8_unorm = 61;
inline constexpr std::uint32_t r8_uint = 62;
inline constexpr std::uint32_t a8_unorm = 65;
inline constexpr std::uint32_t r9g9b9e5_sharedexp = 67;
inline constexpr std::uint32_t bc1_unorm = 71;
inline constexpr std::uint32_t bc2_unorm = 74;
inline constexpr std::uint32_t bc3_unorm = 77;
inline constexpr std::uint32_t bc4_unorm = 80;
inline constexpr std::uint32_t bc5_unorm = 83;
inline constexpr std::uint32_t b5g6r5_unorm = 85;
inline constexpr std::uint32_t b8g8r8a8_unorm = 87;
inline constexpr std::uint32_t b8g8r8a8_unorm_srgb = 91;
inline constexpr std::uint32_t bc6h_uf16 = 95;
inline constexpr std::uint32_t bc7_unorm = 98;
inline constexpr std::uint32_t b4g4r4a4_unorm = 115;
}  // namespace format

struct FormatInfo {
    std::uint32_t dxgi = 0;
    std::uint32_t native = 0;      // backend pixel format value (MTLPixelFormat on Metal)
    std::uint32_t block_bytes = 0; // bytes per texel, or per block for compressed formats
    std::uint32_t block_dim = 1;   // 4 for BC
    bool depth = false;
    bool stencil = false;
    bool emulated = false;         // stored in a different native format than the DXGI one names
    const char* name = "";
    const char* note = "";
};

// D3D_PRIMITIVE_TOPOLOGY plus the console-only list types.
namespace topology {
inline constexpr std::uint32_t point_list = 1;
inline constexpr std::uint32_t line_list = 2;
inline constexpr std::uint32_t line_strip = 3;
inline constexpr std::uint32_t triangle_list = 4;
inline constexpr std::uint32_t triangle_strip = 5;
inline constexpr std::uint32_t rect_list = 17;
inline constexpr std::uint32_t quad_list = 19;
}  // namespace topology

// D3D12_PRIMITIVE_TOPOLOGY_TYPE plus the console's rectangle (19) and quad (32) pipeline types.
enum class TopologyType : std::uint32_t { undefined = 0, point = 1, line = 2, triangle = 3, patch = 4, rectangle = 19, quad = 32 };

enum class QueueType : std::uint32_t { direct, compute, copy };

// D3D12 heap types: DEFAULT, UPLOAD, READBACK; shared = CPU and GPU visible (console unified memory).
enum class MemoryKind : std::uint32_t { device_local, upload, readback, shared };

namespace usage {
inline constexpr std::uint32_t shader_read = 1u << 0;
inline constexpr std::uint32_t render_target = 1u << 1;
inline constexpr std::uint32_t depth_stencil = 1u << 2;
inline constexpr std::uint32_t unordered_access = 1u << 3;
inline constexpr std::uint32_t vertex = 1u << 4;
inline constexpr std::uint32_t index = 1u << 5;
inline constexpr std::uint32_t constant = 1u << 6;
inline constexpr std::uint32_t indirect = 1u << 7;
inline constexpr std::uint32_t copy = 1u << 8;
inline constexpr std::uint32_t pixel_format_view = 1u << 9;  // v3: views in another format of the same size
}  // namespace usage

struct DeviceCaps {
    std::string name;
    std::string process_arch;           // "arm64" or "x86_64"
    std::uint32_t argument_buffers_tier = 0;  // 1 or 2
    bool residency_sets = false;
    bool family_apple7 = false, family_apple8 = false, family_apple9 = false;
    bool family_metal3 = false, family_metal4 = false;
    bool unified_memory = false;
    bool barycentrics = false;
    bool placement_sparse = false;
    bool d24s8 = false;
    bool mesh_shaders = false;
    std::uint64_t max_buffer_length = 0;
    std::uint32_t max_threadgroup_memory = 0;
    std::uint64_t recommended_working_set = 0;
    std::uint32_t simd_width = 0;       // measured from a builtin compute pipeline
    std::uint64_t timestamp_frequency = 0;
    // v2
    std::uint32_t max_threads_per_threadgroup = 0;
    std::uint64_t max_dispatch_groups = 0;      // per dimension; Metal grids are 32-bit (no 65,535 cap)
    std::uint32_t max_mesh_bytes = 32768;       // measured in S0.5 / S5.2 (Metal reports no API for these)
    std::uint32_t max_mesh_scalars = 124;
    bool counters_stage_boundary = false;       // timestamp sampling at encoder boundaries
    bool counters_draw_boundary = false;
    bool counters_dispatch_boundary = false;
    std::uint64_t host_page_size = 0;           // page size of this process (16 KiB arm64, 4 KiB under Rosetta)
    std::uint32_t linear_texture_alignment = 0; // row pitch alignment of buffer-backed textures (R32F)
    double display_refresh_hz = 0;
};

struct DeviceStats {
    std::uint64_t pipelines_created = 0;
    std::uint64_t pipelines_compiled = 0;
    std::uint64_t render_encoders = 0;
    std::uint64_t blit_encoders = 0;
    std::uint64_t compute_encoders = 0;
    std::uint64_t barriers_noop = 0;
    std::uint64_t barriers_fenced = 0;
    std::uint64_t clears_folded = 0;   // clears merged into a render pass load action
    std::uint64_t command_buffers = 0;
    std::uint64_t value_writes = 0;
    std::uint64_t value_waits = 0;
    std::uint64_t rect_draws_expanded = 0;
    std::uint64_t stores_discarded = 0;  // render targets stored with DontCare because a discard followed
    std::uint64_t fence_joins = 0;       // encoders that waited for all earlier work of their queue
    std::uint64_t target_joins = 0;      // of those, joins forced by an untracked render target used again
    std::uint64_t residency_commits = 0; // device-wide residency set commits
    // v2
    std::uint64_t dispatches = 0;
    std::uint64_t dispatches_thread_counts = 0;  // console DispatchX bit 0x20: counts are threads
    std::uint64_t indirect_commands = 0;         // records executed by execute_indirect (max_count each)
    std::uint64_t indirect_prepasses = 0;        // count-buffer and quad-index compute passes
    std::uint64_t prepasses_hoisted = 0;         // of those, run before the render pass that uses them
    std::uint64_t render_pass_splits = 0;        // render passes ended early to run a pre-pass
    std::uint64_t barriers_in_encoder = 0;       // barriers done as an in-encoder memory barrier
    std::uint64_t mesh_draws = 0;
    std::uint64_t quad_draws = 0;
    std::uint64_t queries_occlusion = 0;
    std::uint64_t timestamps = 0;
    std::uint64_t draws_skipped_not_ready = 0;   // NotReady::skip draws whose pipeline was still compiling
    std::uint64_t d24_fragment_pipelines = 0;
    std::uint64_t archive_hits = 0;
    std::uint64_t archive_misses = 0;
    std::uint64_t pipelines_refused_limit = 0;   // Status::limit_exceeded
    std::uint64_t presents = 0;
    std::uint64_t present_planes = 0;
    std::uint64_t command_buffer_errors = 0;     // completed with MTLCommandBufferStatusError (wait_idle: device_lost)
    std::uint64_t command_buffer_victims = 0;    // of those, discarded by Metal's recovery from another GPU error
    std::uint64_t event_signals_repaired = 0;    // event signals a failed command buffer skipped, done by the CPU
};

// ---- memory accounting and pressure (DESIGN.md 8) ----

enum class MemoryCategory : std::uint32_t {
    buffer, texture, heap, descriptor_table, host_window, internal, swapchain, count
};

enum class MemoryPressure : std::uint32_t { normal, warning, critical };

struct MemoryBudget {
    std::uint64_t recommended_working_set = 0;  // MTLDevice recommendedMaxWorkingSetSize
    std::uint64_t device_allocated = 0;         // MTLDevice currentAllocatedSize
    std::uint64_t process_footprint = 0;        // task_info TASK_VM_INFO phys_footprint
    std::uint64_t physical_memory = 0;
    std::array<std::uint64_t, std::size_t(MemoryCategory::count)> bytes{};    // allocatedSize of live objects
    std::array<std::uint64_t, std::size_t(MemoryCategory::count)> objects{};
    MemoryPressure pressure = MemoryPressure::normal;  // last level the dispatch source reported
    std::uint64_t pressure_events = 0;
};

const char* memory_category_name(MemoryCategory c);

// ---- memory ----

struct BufferDesc {
    std::uint64_t size = 0;
    MemoryKind memory = MemoryKind::device_local;
    std::uint32_t usage = 0;
    const char* label = nullptr;
};

enum class TextureDimension : std::uint32_t { tex1d, tex2d, tex2d_array, tex3d, cube };

struct TextureDesc {
    TextureDimension dimension = TextureDimension::tex2d;
    std::uint32_t format = format::unknown;
    std::uint32_t width = 1, height = 1, depth_or_layers = 1;
    std::uint32_t mip_levels = 1;
    std::uint32_t samples = 1;
    std::uint32_t usage = usage::shader_read;
    MemoryKind memory = MemoryKind::device_local;
    const char* label = nullptr;
};

struct AllocationInfo {
    std::uint64_t size = 0;
    std::uint64_t alignment = 0;
};

struct HeapDesc {
    std::uint64_t size = 0;
    MemoryKind memory = MemoryKind::device_local;
    bool console_arena = false;  // also map a buffer over the whole heap: console VA = va_base + offset
    const char* label = nullptr;
};

// Host-owned pages the GPU reads and writes in place (title memory outside the arena, DESIGN.md F08): a Metal
// bytesNoCopy buffer over `cpu`, which must be host_page_size aligned. The pages stay owned by the caller and must
// outlive the buffer and every command buffer that used it. Resident from creation, untracked (barriers order it).
struct HostBufferDesc {
    void* cpu = nullptr;
    std::uint64_t size = 0;
    std::uint64_t va = 0;  // console VA to register for resolve_va; 0 = allocate one
    const char* label = nullptr;
};

enum class ResourceKind : std::uint32_t { buffer, texture };

class Resource {
public:
    virtual ~Resource() = default;
    virtual ResourceKind kind() const = 0;
    // Address in the backend's console-style VA space (D3D12 GetGPUVirtualAddress). Never the host GPU
    // address and never the CPU address (DESIGN.md F08).
    virtual std::uint64_t gpu_va() const = 0;
    virtual std::uint64_t host_gpu_address() const = 0;  // Metal gpuAddress, 0 for textures
    virtual void* cpu_pointer() = 0;                     // null unless CPU visible
    virtual std::uint64_t size() const = 0;
    virtual bool placed() const = 0;
};

class Buffer : public Resource {
public:
    ResourceKind kind() const override { return ResourceKind::buffer; }
};

// v3: a view of a texture (D3D12 SRV/UAV over a range, another dimension, format or channel order).
enum class Swizzle : std::uint8_t { identity, zero, one, r, g, b, a };

struct TextureViewDesc {
    std::uint32_t format = format::unknown;  // DXGI; unknown = the texture's
    TextureDimension dimension = TextureDimension::tex2d;
    std::uint32_t first_mip = 0, mip_count = 0;      // 0 = to the last mip
    std::uint32_t first_layer = 0, layer_count = 0;  // 0 = to the last layer (or slice of a cube)
    std::array<Swizzle, 4> swizzle{};                // identity channels unless set
};

class Texture : public Resource {
public:
    ResourceKind kind() const override { return ResourceKind::texture; }
    virtual const TextureDesc& desc() const = 0;
    virtual std::uint32_t native_format() const = 0;
    // Only for CPU-visible textures (MemoryKind::shared); others take a copy command.
    // v3: for a 3D texture `layer` is the z slice written (one slice per call).
    virtual Status write_region(std::uint32_t mip, std::uint32_t layer, std::uint32_t x, std::uint32_t y,
                                std::uint32_t width, std::uint32_t height, const void* data, std::uint32_t row_pitch) = 0;
};

class Heap {
public:
    virtual ~Heap() = default;
    virtual std::uint64_t size() const = 0;
    virtual std::uint64_t va_base() const = 0;
    virtual std::uint64_t host_gpu_address() const = 0;  // of the arena buffer when console_arena
    virtual Buffer* arena() = 0;                         // null unless console_arena
};

// ---- samplers and descriptors ----

enum class Filter : std::uint32_t { point, linear };
enum class AddressMode : std::uint32_t { wrap = 1, mirror = 2, clamp = 3, border = 4, mirror_once = 5 };
enum class CompareFunc : std::uint32_t { none = 0, never = 1, less = 2, equal = 3, less_equal = 4, greater = 5, not_equal = 6, greater_equal = 7, always = 8 };

struct SamplerDesc {
    Filter min = Filter::linear, mag = Filter::linear, mip = Filter::linear;
    AddressMode u = AddressMode::wrap, v = AddressMode::wrap, w = AddressMode::wrap;
    std::uint32_t max_anisotropy = 1;
    CompareFunc compare = CompareFunc::none;
    float mip_lod_bias = 0.0f;  // not a Metal sampler field: applied in the shader (DESIGN.md F12)
    float min_lod = 0.0f, max_lod = 1000.0f;
    std::array<float, 4> border{0, 0, 0, 0};
};

class Sampler {
public:
    virtual ~Sampler() = default;
    virtual bool border_exact() const = 0;  // false when the border colour was rounded to a fixed Metal one
};

// A host bindless table: one array of 8-byte entries (texture or sampler handles, buffer addresses) in GPU
// memory. The translated-shader contract has one table per set-0 binding (DESIGN.md F10/F11).
enum class DescriptorKind : std::uint32_t { texture, sampler, buffer };

struct DescriptorHeapDesc {
    DescriptorKind kind = DescriptorKind::texture;
    std::uint32_t count = 0;
    const char* label = nullptr;
};

class DescriptorHeap {
public:
    virtual ~DescriptorHeap() = default;
    virtual std::uint32_t count() const = 0;
    virtual std::uint32_t stride() const = 0;
    virtual DescriptorKind kind() const = 0;
    virtual Status write_texture(std::uint32_t slot, Texture* texture) = 0;
    virtual Status write_sampler(std::uint32_t slot, Sampler* sampler) = 0;
    virtual Status write_buffer(std::uint32_t slot, Buffer* buffer, std::uint64_t offset) = 0;
    virtual Status clear(std::uint32_t slot) = 0;
    virtual Status copy(std::uint32_t dst_slot, const DescriptorHeap& src, std::uint32_t src_slot, std::uint32_t n) = 0;
    virtual std::uint64_t read_raw(std::uint32_t slot) const = 0;  // the 8 bytes a shader reads there
};

// ---- root layout: the translated-shader contract ----

// leonida's contract (d3d12_vulkan.md section 5): a 144-byte push block of s0..s31 user data, heap_base and
// code_base, plus set-0 bindings that are bindless tables. A D3D12 root signature is lowered to user data by
// the D3D12 layer; the backend sees only this layout.
struct RootTable {
    std::uint32_t binding = 0;  // set-0 binding of the contract (0 = 2D images, 1 = samplers, 2 = hash table, ...)
    DescriptorKind kind = DescriptorKind::texture;
};

struct RootLayoutDesc {
    std::uint32_t push_bytes = 144;
    std::uint32_t user_dwords = 32;
    std::vector<RootTable> tables;
    // Set index SPIRV-Cross gives binding b: 1 + b with the split recipe (tools/route_a_msl.py --split),
    // 0 for every binding with the unmodified Route A recipe (reported as collapsed).
    bool split_sets = true;
};

class RootLayout {
public:
    virtual ~RootLayout() = default;
    virtual const RootLayoutDesc& desc() const = 0;
};

// ---- shaders and pipelines ----

struct MslOptions {
    std::uint32_t language_major = 3, language_minor = 0;
    bool fast_math = false;
    const char* label = nullptr;
    bool deferred = false;  // v2: compile when the first pipeline using it compiles (on a compile thread)
};

class ShaderLibrary {
public:
    virtual ~ShaderLibrary() = default;
    virtual std::vector<std::string> function_names() const = 0;
    virtual const std::string& source() const = 0;  // empty for a metallib
    virtual double compile_seconds() const = 0;
};

struct ShaderRef {
    ShaderLibrary* library = nullptr;
    std::string entry = "main0";
};

struct InputElement {
    std::uint32_t location = 0;  // D3D input register = MSL [[attribute(n)]]
    std::uint32_t format = format::unknown;
    std::uint32_t slot = 0;
    std::uint32_t offset = 0;
    bool per_instance = false;
    std::uint32_t step_rate = 1;
};

enum class Blend : std::uint32_t {
    zero = 1, one = 2, src_color = 3, inv_src_color = 4, src_alpha = 5, inv_src_alpha = 6, dest_alpha = 7,
    inv_dest_alpha = 8, dest_color = 9, inv_dest_color = 10, src_alpha_sat = 11, blend_factor = 14,
    inv_blend_factor = 15, src1_color = 16, inv_src1_color = 17, src1_alpha = 18, inv_src1_alpha = 19
};
enum class BlendOp : std::uint32_t { add = 1, subtract = 2, rev_subtract = 3, min = 4, max = 5 };

struct TargetBlend {
    bool enable = false;
    Blend src = Blend::one, dst = Blend::zero;
    BlendOp op = BlendOp::add;
    Blend src_alpha = Blend::one, dst_alpha = Blend::zero;
    BlendOp op_alpha = BlendOp::add;
    std::uint8_t write_mask = 0xf;
};

enum class CullMode : std::uint32_t { none = 1, front = 2, back = 3 };

struct RasterDesc {
    CullMode cull = CullMode::back;
    bool front_ccw = false;
    bool wireframe = false;
    std::int32_t depth_bias = 0;
    float depth_bias_clamp = 0.0f;
    float slope_scaled_depth_bias = 0.0f;
    bool depth_clip = true;
};

// D3D12_STENCIL_OP numbering.
enum class StencilOp : std::uint32_t { keep = 1, zero = 2, replace = 3, incr_sat = 4, decr_sat = 5, invert = 6, incr = 7, decr = 8 };

struct StencilFace {
    StencilOp fail = StencilOp::keep, depth_fail = StencilOp::keep, pass = StencilOp::keep;
    CompareFunc func = CompareFunc::always;
};

struct DepthStencilDesc {
    bool depth_test = false;
    bool depth_write = false;
    CompareFunc depth_func = CompareFunc::less;
    bool stencil = false;
    // v2: used when stencil is true
    std::uint8_t stencil_read_mask = 0xff, stencil_write_mask = 0xff;
    StencilFace front, back;
};

enum class CompileMode : std::uint32_t {
    eager,  // compile now, block
    lazy,   // compile at the first draw that binds it (the 54k-creation storm, DESIGN.md F13)
    async,  // queue on the backend's compile threads now (by Priority); never compiles on the caller's thread
};

// What a draw or dispatch does when its pipeline is not ready yet. skip never blocks the submitting thread: the
// command is dropped (counted, logged once per pipeline) and a lazy pipeline is queued at Priority::demand.
enum class NotReady : std::uint32_t { wait, skip };

// Order of the compile queue: first_scene before demand (draws that found it not ready) before background.
enum class Priority : std::uint32_t { first_scene = 0, demand = 1, background = 2 };

// D24_UNORM_S8_UINT targets (stored as D32F_S8): fragment = the pixel stage writes depth = quantise(z + D24 bias)
// as D3D12 D24 hardware does (DESIGN.md 5.3), and the rasterizer bias is off; rasterizer = Metal's float bias and
// unquantised depth (the fallback when the pixel shader cannot be rewritten).
enum class D24Mode : std::uint32_t { fragment, rasterizer };

// Why a pipeline was refused with Status::limit_exceeded.
struct PipelineLimit {
    enum class Kind : std::uint32_t { none, threadgroup_memory, mesh_size, mesh_scalars, threads_per_group };
    Kind kind = Kind::none;
    std::uint64_t value = 0, limit = 0;
    bool estimated = false;  // found from the MSL before compiling, not reported by Metal
    std::string reason;
};
const char* limit_kind_name(PipelineLimit::Kind k);

enum class RectMode : std::uint32_t {
    none,
    vertex_expand,  // generated wrapper vertex function, exact after the vertex stage
    cpu_rewrite,    // fourth vertex from vertex inputs on the CPU, exact only for shaders linear in inputs
};

struct RenderPipelineDesc {
    ShaderRef vs, ps;
    std::vector<InputElement> inputs;
    std::array<std::uint32_t, 8> rt_formats{};
    std::uint32_t rt_count = 0;
    std::array<TargetBlend, 8> blend{};
    std::uint32_t depth_format = format::unknown;
    std::uint32_t samples = 1;
    RasterDesc raster;
    DepthStencilDesc depth_stencil;
    TopologyType topology = TopologyType::triangle;
    const RootLayout* root = nullptr;
    CompileMode mode = CompileMode::eager;
    RectMode rect = RectMode::vertex_expand;
    const char* label = nullptr;
    // v2
    NotReady not_ready = NotReady::wait;
    Priority priority = Priority::demand;
    D24Mode d24 = D24Mode::fragment;
};

enum class PipelineState : std::uint32_t { deferred, compiling, ready, failed, queued };  // queued: v2, waiting for a compile thread
enum class PipelineKind : std::uint32_t { render, compute, mesh };

enum class BindingKind : std::uint32_t { buffer, texture, sampler, other };

struct ReflectedMember {
    std::string name;
    std::uint64_t offset = 0;
    std::string type;
    std::uint64_t array_length = 0;
};

struct ReflectedBinding {
    std::string name;
    std::uint32_t index = 0;
    BindingKind kind = BindingKind::other;
    std::string access;          // "read", "read_write", "write"
    std::uint64_t data_size = 0; // buffer: bytes of the pointee
    bool argument_buffer = false;
    bool used = true;
    std::vector<ReflectedMember> members;
};

struct ReflectedAttribute {
    std::string name;
    std::uint32_t location = 0;
    std::string type;
    bool active = true;
};

// How the pipeline binds the root layout, as the backend resolved it from reflection.
struct BindingPlanEntry {
    enum class What : std::uint32_t { push_block, table, vertex_buffer, rect_params, unmapped, d24_params };
    What what = What::unmapped;
    std::uint32_t stage = 0;   // 0 vertex, 1 fragment, 2 kernel, 3 object, 4 mesh
    std::uint32_t index = 0;   // [[buffer(n)]]
    std::uint32_t binding = 0; // table: contract binding; vertex_buffer: input slot
    std::uint32_t set = 0;     // SPIRV-Cross descriptor set
    std::string note;
};

struct PipelineReflection {
    std::vector<ReflectedBinding> vertex, fragment;
    std::vector<ReflectedAttribute> attributes;
    std::vector<BindingPlanEntry> plan;
    std::vector<std::string> mismatches;  // what does not line up with the root layout
    std::vector<ReflectedBinding> kernel, object, mesh;  // v2
};

class Pipeline {
public:
    virtual ~Pipeline() = default;
    virtual PipelineKind kind() const = 0;
    virtual PipelineState state() const = 0;
    virtual Status wait() = 0;  // compiles a deferred one on this thread, or takes a queued one off the queue
    virtual const std::string& error() const = 0;
    virtual const PipelineReflection& reflection() const = 0;  // valid once ready
    virtual double compile_seconds() const = 0;                 // library compile (deferred) + pipeline build
    virtual const PipelineLimit& limit() const = 0;             // set when refused with limit_exceeded
    virtual Status status() const = 0;                          // ok, not_ready, pipeline_failed, limit_exceeded
    virtual bool from_archive() const = 0;                      // the binary archive held it
};

class RenderPipeline : public Pipeline {
public:
    PipelineKind kind() const override { return PipelineKind::render; }
    virtual RectMode rect_mode() const = 0;
    virtual D24Mode d24_mode() const = 0;
};

struct ComputePipelineDesc {
    ShaderRef cs;
    std::array<std::uint32_t, 3> threads{0, 0, 0};  // threadgroup size; 0 = gl_WorkGroupSize of the SPIRV-Cross MSL
    const RootLayout* root = nullptr;
    CompileMode mode = CompileMode::eager;
    NotReady not_ready = NotReady::wait;
    Priority priority = Priority::demand;
    const char* label = nullptr;
};

class ComputePipeline : public Pipeline {
public:
    PipelineKind kind() const override { return PipelineKind::compute; }
    virtual std::array<std::uint32_t, 3> threads() const = 0;
    virtual std::uint32_t threadgroup_memory() const = 0;  // staticThreadgroupMemoryLength, once ready
    virtual std::uint32_t max_total_threads() const = 0;
    virtual std::uint32_t execution_width() const = 0;
};

// Mesh output use of a SPIRV-Cross mesh function, from its MSL (mesh<V, P, nv, np, topology> and the struct
// V): Metal refuses a pipeline whose mesh size nv * 16 * ceil(vertex scalars / 4) is above 32,768 B or whose unique
// scalar count is above 124 (S5.2: 4,631 of 9,282 corpus mesh shaders).
struct MeshEstimate {
    bool parsed = false;
    std::uint32_t max_vertices = 0, max_primitives = 0;
    std::uint32_t vertex_scalars = 0, primitive_scalars = 0;
    std::uint64_t mesh_bytes = 0;
    std::string error;
};

struct MeshPipelineDesc {
    ShaderRef object;  // optional (amplification); the title uses none
    ShaderRef mesh;
    ShaderRef ps;      // optional: no pixel shader = rasterization off
    std::array<std::uint32_t, 3> object_threads{0, 0, 0}, mesh_threads{0, 0, 0};  // 0 = from the MSL
    std::uint32_t payload_bytes = 0;
    std::array<std::uint32_t, 8> rt_formats{};
    std::uint32_t rt_count = 0;
    std::array<TargetBlend, 8> blend{};
    std::uint32_t depth_format = format::unknown;
    std::uint32_t samples = 1;
    RasterDesc raster;
    DepthStencilDesc depth_stencil;
    const RootLayout* root = nullptr;
    CompileMode mode = CompileMode::eager;
    NotReady not_ready = NotReady::wait;
    Priority priority = Priority::demand;
    bool precheck = true;  // refuse from the MSL estimate before compiling (saves the compile)
    const char* label = nullptr;
};

class MeshPipeline : public Pipeline {
public:
    PipelineKind kind() const override { return PipelineKind::mesh; }
    virtual const MeshEstimate& estimate() const = 0;
    virtual std::array<std::uint32_t, 3> mesh_threads() const = 0;
    virtual std::array<std::uint32_t, 3> object_threads() const = 0;
};

// ---- queries ----

enum class QueryType : std::uint32_t { occlusion, binary_occlusion, timestamp, pipeline_statistics };

struct QueryHeapDesc {
    QueryType type = QueryType::occlusion;
    std::uint32_t count = 0;
    const char* label = nullptr;
};

class QueryHeap {
public:
    virtual ~QueryHeap() = default;
    virtual QueryType type() const = 0;
    virtual std::uint32_t count() const = 0;
};

// ---- indirect execution (ExecuteIndirect with the signature shapes leonida's IR records) ----

enum class IndirectKind : std::uint32_t { draw, draw_indexed, dispatch, dispatch_mesh };

struct IndirectDesc {
    IndirectKind kind = IndirectKind::draw;
    std::uint32_t stride = 0;     // bytes between records (>= the packed size: 16, 20, 12, 12)
    std::uint32_t max_count = 1;
    Buffer* arguments = nullptr;
    std::uint64_t argument_offset = 0;
    Buffer* count_buffer = nullptr;  // null: max_count records; else min(*count, max_count), GPU-written uint32
    std::uint64_t count_offset = 0;
};

namespace dispatch_flag {
inline constexpr std::uint32_t thread_counts = 0x20;  // console DispatchX initiator bit: x, y, z are threads
}

// ---- command lists ----

struct TargetView {
    Texture* texture = nullptr;
    std::uint32_t format = format::unknown;  // 0 = the texture's
    std::uint32_t mip = 0, layer = 0;
};

enum class LoadOp : std::uint32_t { preserve, clear, discard, no_access };
enum class StoreOp : std::uint32_t { preserve, discard, resolve, no_access };

struct RenderPassTarget {
    TargetView view;
    LoadOp load = LoadOp::preserve;
    StoreOp store = StoreOp::preserve;
    std::array<float, 4> clear{0, 0, 0, 0};
};

struct RenderPassDesc {
    std::array<RenderPassTarget, 8> targets{};
    std::uint32_t target_count = 0;
    TargetView depth;
    LoadOp depth_load = LoadOp::preserve;
    StoreOp depth_store = StoreOp::preserve;
    float clear_depth = 1.0f;
    std::uint8_t clear_stencil = 0;
};

struct Viewport { float x = 0, y = 0, width = 0, height = 0, min_depth = 0, max_depth = 1; };
struct Rect { std::int32_t left = 0, top = 0, right = 0, bottom = 0; };

struct VertexBufferView {
    std::uint64_t va = 0;  // console VA (Resource::gpu_va)
    std::uint32_t size = 0;
    std::uint32_t stride = 0;
};

struct IndexBufferView {
    std::uint64_t va = 0;
    std::uint32_t size = 0;
    std::uint32_t format = format::r16_uint;  // r16_uint or r32_uint
};

enum class BarrierType : std::uint32_t { transition, uav, aliasing };

// Every barrier orders all earlier work of the queue before all later work, whatever resource it names, because
// any resource may be read through a descriptor table (DESIGN.md 3.3). A transition between two read-only states
// is the only no-op.
struct Barrier {
    BarrierType type = BarrierType::transition;
    Resource* resource = nullptr;  // null: all (UAV / aliasing)
    std::uint32_t before = 0, after = 0;  // D3D12_RESOURCE_STATES
};

enum class WaitCompare : std::uint32_t { greater_equal, equal, not_equal };

// One 2D region of one subresource; v3: for a 3D texture `layer` is the z slice.
struct BufferTextureCopy {
    Buffer* buffer = nullptr;
    std::uint64_t offset = 0;
    std::uint32_t row_pitch = 0;
    Texture* texture = nullptr;
    std::uint32_t mip = 0, layer = 0;
    std::uint32_t x = 0, y = 0, width = 0, height = 0;
    bool depth_plane = false;
};

// v4: one region of one subresource into another texture of the same texel size (CopyTextureRegion between textures).
// For a 3D texture `layer` is the first z slice and `depth` the slice count; for others depth is 1.
struct TextureCopy {
    Texture* src = nullptr;
    std::uint32_t src_mip = 0, src_layer = 0, src_x = 0, src_y = 0;
    Texture* dst = nullptr;
    std::uint32_t dst_mip = 0, dst_layer = 0, dst_x = 0, dst_y = 0;
    std::uint32_t width = 0, height = 0, depth = 1;
};

class CommandList {
public:
    virtual ~CommandList() = default;
    virtual QueueType type() const = 0;
    virtual void reset() = 0;
    virtual Status close() = 0;

    // Explicit render passes (D3D12 BeginRenderPass) ...
    virtual void begin_render_pass(const RenderPassDesc& desc) = 0;
    virtual void end_render_pass() = 0;
    // ... and the OMSetRenderTargets model, which the backend turns into passes when it encodes.
    virtual void set_render_targets(std::span<const TargetView> targets, const TargetView* depth) = 0;
    virtual void clear_render_target(const TargetView& target, const std::array<float, 4>& color) = 0;
    virtual void clear_depth_stencil(const TargetView& target, bool depth, float depth_value, bool stencil,
                                     std::uint8_t stencil_value) = 0;
    virtual void discard(Resource* resource) = 0;

    virtual void set_pipeline(RenderPipeline* pipeline) = 0;
    virtual void set_root_layout(const RootLayout* layout) = 0;
    virtual void set_user_data(std::uint32_t first, std::span<const std::uint32_t> dwords) = 0;
    virtual void set_heap_base(std::uint64_t value) = 0;
    virtual void set_code_base(std::uint64_t value) = 0;
    virtual void set_descriptor_table(std::uint32_t binding, DescriptorHeap* heap) = 0;
    virtual void set_vertex_buffers(std::uint32_t first_slot, std::span<const VertexBufferView> views) = 0;
    virtual void set_index_buffer(const IndexBufferView& view) = 0;
    virtual void set_topology(std::uint32_t d3d_topology) = 0;
    virtual void set_viewport(const Viewport& viewport) = 0;
    virtual void set_scissor(const Rect& rect) = 0;
    virtual void set_blend_factor(const std::array<float, 4>& factor) = 0;
    virtual void set_stencil_ref(std::uint32_t ref) = 0;

    virtual void draw(std::uint32_t vertex_count, std::uint32_t instance_count, std::uint32_t first_vertex,
                      std::uint32_t first_instance) = 0;
    virtual void draw_indexed(std::uint32_t index_count, std::uint32_t instance_count, std::uint32_t first_index,
                              std::int32_t base_vertex, std::uint32_t first_instance) = 0;

    virtual void copy_buffer(Buffer* dst, std::uint64_t dst_offset, Buffer* src, std::uint64_t src_offset,
                             std::uint64_t size) = 0;
    virtual void copy_buffer_to_texture(const BufferTextureCopy& copy) = 0;
    virtual void copy_texture_to_buffer(const BufferTextureCopy& copy) = 0;
    // v4. Ordered like the other copies (a blit encoder).
    virtual void copy_texture(const TextureCopy& copy) = 0;
    // v4. Every 32-bit word of [offset, offset + size) becomes `value` (size a multiple of 4): a UAV clear of a raw,
    // structured or 32-bit typed buffer view. A compute dispatch, or a blit fill when the four bytes are equal.
    virtual void fill_buffer_u32(Buffer* buffer, std::uint64_t offset, std::uint64_t size, std::uint32_t value) = 0;

    virtual void barrier(std::span<const Barrier> barriers) = 0;

    // Console command-stream values (list write32/write64/wait32/wait64; DESIGN.md F27).
    virtual void write_value(Buffer* buffer, std::uint64_t offset, std::uint64_t value, std::uint32_t bytes) = 0;
    virtual void wait_value(Buffer* buffer, std::uint64_t offset, std::uint64_t value, std::uint32_t bytes,
                            WaitCompare compare) = 0;

    virtual void begin_event(const char* label) = 0;
    virtual void end_event() = 0;

    // v2. Compute and mesh share the root state above (user data, heap_base, code_base, tables): leonida's IR
    // captures the 32 user dwords per draw and per dispatch, so the caller sets them before each command.
    virtual void set_compute_pipeline(ComputePipeline* pipeline) = 0;
    virtual void dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint32_t flags = 0) = 0;
    virtual void set_mesh_pipeline(MeshPipeline* pipeline) = 0;
    virtual void draw_mesh(std::uint32_t x, std::uint32_t y, std::uint32_t z) = 0;
    // draw / draw_indexed / dispatch_mesh use the graphics or mesh state, dispatch the compute state.
    virtual void execute_indirect(const IndirectDesc& desc) = 0;
    virtual void set_stencil_refs(std::uint32_t front, std::uint32_t back) = 0;

    virtual void begin_query(QueryHeap* heap, std::uint32_t index) = 0;  // occlusion kinds
    virtual void end_query(QueryHeap* heap, std::uint32_t index) = 0;    // timestamp: when earlier work is done
    virtual void resolve_queries(QueryHeap* heap, std::uint32_t first, std::uint32_t count, Buffer* dst,
                                 std::uint64_t offset) = 0;  // 8 bytes per query (pipeline statistics: 88, zeros)
    // Console write64 with the timestamp flag: the GPU time (1 GHz ticks) once every earlier command finished.
    virtual void write_timestamp(Buffer* buffer, std::uint64_t offset) = 0;
};

// ---- queues, fences, present ----

class Fence {
public:
    virtual ~Fence() = default;
    virtual std::uint64_t completed_value() const = 0;
    virtual Status signal_cpu(std::uint64_t value) = 0;
    virtual Status wait_cpu(std::uint64_t value, std::uint64_t timeout_ns) = 0;
    // SetEventOnCompletion: the callback runs on a backend thread once the value is reached.
    virtual Status on_completion(std::uint64_t value, std::function<void()> callback) = 0;
    // Console fence slot 12: a CPU-visible copy of the value the title may poll as memory.
    virtual const volatile std::uint64_t* value_address() const = 0;
};

class Swapchain;

// One PresentX plane (the caller decodes the console's 72-byte plane parameters into this). Plane 0 is drawn
// opaque; later planes are composed over it: premultiplied o.rgb + s.rgb * (1 - o.a), or straight alpha.
enum class PlaneBlend : std::uint32_t { opaque, premultiplied, straight };

struct PresentPlane {
    Texture* source = nullptr;
    Rect src{};  // texels; all zero = the whole texture
    Rect dst{};  // drawable pixels; all zero = the whole drawable
    Filter filter = Filter::linear;
    PlaneBlend blend = PlaneBlend::premultiplied;
};

struct PresentDesc {
    std::array<PresentPlane, 8> planes{};
    std::uint32_t plane_count = 0;
    std::uint32_t sync_interval = 1;  // vblanks per frame: presentDrawable afterMinimumDuration interval / refresh
};

struct ClockCalibration {
    std::uint64_t gpu_ticks = 0;  // timestamp_frequency ticks (1 GHz measured)
    std::uint64_t cpu_ns = 0;     // CLOCK_UPTIME_RAW in ns (mach_absolute_time scaled)
};

class Queue {
public:
    virtual ~Queue() = default;
    virtual QueueType type() const = 0;
    virtual Status execute(std::span<CommandList* const> lists) = 0;
    virtual Status signal(Fence* fence, std::uint64_t value) = 0;
    virtual Status wait(Fence* fence, std::uint64_t value) = 0;
    virtual Status present(Swapchain* swapchain, std::uint32_t sync_interval) = 0;
    // device_lost when a command buffer of this queue failed on the GPU since the previous device_lost answer.
    virtual Status wait_idle(std::uint64_t timeout_ns) = 0;
    virtual std::uint64_t timestamp_frequency() const = 0;
    // v2
    virtual Status present_planes(Swapchain* swapchain, const PresentDesc& desc) = 0;
    virtual ClockCalibration clock_calibration() const = 0;
};

struct SwapchainDesc {
    void* layer = nullptr;  // CAMetalLayer*
    std::uint32_t width = 0, height = 0;
    std::uint32_t buffer_count = 2;
    std::uint32_t format = format::r8g8b8a8_unorm;  // the title's back buffers; the layer stays BGRA8
    bool keep_last_frame = false;  // copy each presented frame for read_last_frame (tests, screenshots)
    bool frame_events = false;     // v2: run a display link that ticks wait_frame_event (console frame events)
};

struct FrameStats {
    std::uint64_t presented = 0;          // present calls
    std::uint64_t displayed = 0;          // drawables the presented handler reported on screen
    double last_presented_time = 0;       // CFTimeInterval of the last on-screen drawable (0 if never)
    std::vector<double> recent_intervals; // seconds between consecutive on-screen drawables, newest last (<= 128)
    std::uint64_t frame_events = 0;       // display-link ticks
    double refresh_hz = 0;
};

class Swapchain {
public:
    virtual ~Swapchain() = default;
    virtual std::uint32_t buffer_count() const = 0;
    virtual Texture* buffer(std::uint32_t index) = 0;
    virtual std::uint32_t current_index() const = 0;
    virtual std::uint64_t presented() const = 0;
    // BGRA8 pixels of the last presented drawable (keep_last_frame), after the queue went idle.
    virtual Status read_last_frame(std::vector<std::uint8_t>& bgra, std::uint32_t& width, std::uint32_t& height) = 0;
    // v2
    virtual FrameStats frame_stats() const = 0;
    // Blocks until the next display-link tick after the call (frame_events), or times out.
    virtual Status wait_frame_event(std::uint64_t timeout_ns) = 0;
};

// ---- pipeline cache (MTLBinaryArchive) and compile queue ----

struct PipelineCacheDesc {
    const char* directory = nullptr;      // the archive and its key file live here
    std::string translator_version;       // part of the key: a new translator never reads an old archive
    bool load = true;                     // read an existing archive with the same key
};

struct PipelineCacheStats {
    bool open = false;
    bool loaded = false;                  // an archive with this key existed and was read
    std::string path, key;
    std::uint64_t hits = 0, misses = 0, added = 0, add_failures = 0;
    double load_seconds = 0, serialize_seconds = 0;
    std::uint64_t file_bytes = 0;
};

struct CompileQueueStats {
    std::uint32_t threads = 0;
    std::array<std::uint64_t, 3> queued{};     // now, per Priority
    std::array<std::uint64_t, 3> completed{};  // since creation, per Priority they ran at
    std::uint64_t running = 0;
    std::uint64_t stolen = 0;                  // taken off the queue by a wait() on the caller's thread
};

// ---- device ----

class Device {
public:
    virtual ~Device() = default;
    virtual const DeviceCaps& caps() const = 0;
    virtual DeviceStats stats() const = 0;
    virtual bool describe_format(std::uint32_t dxgi, FormatInfo& out) const = 0;

    virtual std::unique_ptr<Queue> create_queue(QueueType type, const char* label = nullptr) = 0;
    virtual std::unique_ptr<Fence> create_fence(std::uint64_t initial) = 0;

    virtual AllocationInfo buffer_allocation_info(const BufferDesc& desc) const = 0;
    virtual AllocationInfo texture_allocation_info(const TextureDesc& desc) const = 0;
    virtual std::unique_ptr<Heap> create_heap(const HeapDesc& desc) = 0;
    virtual std::unique_ptr<Buffer> create_buffer(const BufferDesc& desc) = 0;
    virtual std::unique_ptr<Texture> create_texture(const TextureDesc& desc) = 0;
    virtual std::unique_ptr<Buffer> create_placed_buffer(Heap* heap, std::uint64_t offset, const BufferDesc& desc) = 0;
    virtual std::unique_ptr<Texture> create_placed_texture(Heap* heap, std::uint64_t offset, const TextureDesc& desc) = 0;
    // Console VA to the buffer holding it (vertex/index views, root CBVs): GPU VA is not CPU VA.
    virtual bool resolve_va(std::uint64_t va, Buffer** buffer, std::uint64_t* offset) const = 0;

    virtual std::unique_ptr<Sampler> create_sampler(const SamplerDesc& desc) = 0;
    virtual std::unique_ptr<DescriptorHeap> create_descriptor_heap(const DescriptorHeapDesc& desc) = 0;
    virtual std::unique_ptr<RootLayout> create_root_layout(const RootLayoutDesc& desc) = 0;

    virtual std::unique_ptr<ShaderLibrary> create_library_msl(std::string_view source, const MslOptions& options,
                                                              std::string* error) = 0;
    virtual std::unique_ptr<ShaderLibrary> create_library_metallib(std::span<const std::uint8_t> bytes,
                                                                   std::string* error) = 0;
    virtual std::unique_ptr<RenderPipeline> create_render_pipeline(const RenderPipelineDesc& desc) = 0;

    virtual std::unique_ptr<CommandList> create_command_list(QueueType type) = 0;
    virtual std::unique_ptr<Swapchain> create_swapchain(const SwapchainDesc& desc) = 0;

    // ---- v2 ----
    virtual std::unique_ptr<ComputePipeline> create_compute_pipeline(const ComputePipelineDesc& desc) = 0;
    virtual std::unique_ptr<MeshPipeline> create_mesh_pipeline(const MeshPipelineDesc& desc) = 0;
    virtual std::unique_ptr<QueryHeap> create_query_heap(const QueryHeapDesc& desc) = 0;

    virtual std::unique_ptr<Buffer> create_host_buffer(const HostBufferDesc& desc, std::string* error) = 0;
    // A linear texture over buffer bytes (CPU-visible when the buffer is): GPU-written textures the CPU reads
    // (ocean height/displacement) need no copy. row_pitch must be a multiple of caps().linear_texture_alignment.
    virtual std::unique_ptr<Texture> create_buffer_texture(Buffer* buffer, std::uint64_t offset, std::uint32_t row_pitch,
                                                           const TextureDesc& desc, std::string* error) = 0;

    virtual MemoryBudget memory_budget() const = 0;
    // Called on a backend dispatch queue when the system memory-pressure source fires (and by simulate).
    virtual std::uint64_t add_memory_pressure_handler(std::function<void(MemoryPressure)> handler) = 0;
    virtual void remove_memory_pressure_handler(std::uint64_t token) = 0;
    virtual void simulate_memory_pressure(MemoryPressure level) = 0;  // diagnostics and tests

    virtual Status open_pipeline_cache(const PipelineCacheDesc& desc, std::string* error) = 0;
    virtual Status flush_pipeline_cache(std::string* error) = 0;  // serialize the archive if it grew
    virtual PipelineCacheStats pipeline_cache_stats() const = 0;
    // Moves queued (or deferred) pipelines to the front of the compile queue at `priority` ("first scene first").
    virtual void precompile(std::span<Pipeline* const> pipelines, Priority priority) = 0;
    virtual CompileQueueStats compile_queue_stats() const = 0;
    virtual MeshEstimate estimate_mesh(ShaderLibrary* library, const std::string& entry) const = 0;

    // ---- v3 ----
    // A view sharing the texture's memory. A format other than the texture's needs usage::pixel_format_view on it
    // (depth, stencil and swizzled formats always have it). The view keeps the texture alive.
    virtual std::unique_ptr<Texture> create_texture_view(Texture* texture, const TextureViewDesc& desc,
                                                         std::string* error) = 0;
};

}  // namespace gta6rt::gpu
