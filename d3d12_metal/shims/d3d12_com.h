// port/d3d12_metal: copy of d3d12_vulkan/shims/d3d12_com.h over the Metal backend (src/metal_context.h) instead of
// the Vulkan one; ORIGIN.md lists the changes. D3D12 COM objects: command allocator, graphics command list,
// command queue and fence. Their vtables are the Windows SDK's (ID3D12GraphicsCommandList8, ID3D12CommandQueue,
// ID3D12CommandAllocator, ID3D12Fence1); every method not implemented here is a generated stub that reports its
// name once (tools/gen_d3d12_stubs.py). The Xbox-specific slots measured for the device do not apply to these
// interfaces as far as is known; docs/d3d12_vulkan.md section 3.1 lists what is still unverified.
//
// Contracts with the rest of the layer (the device proxy that will hand these objects out):
// - a resource's ID is its backing pointer value (identical Xbox depth placements share storage and layouts);
// - so is an ID3D12QueryHeap's leonida::QueryHeapId, registered with its VkQueryPool;
// - the CPU handle of an RTV or DSV points at a leonida::TargetView in the device's descriptor heap.
#pragma once

#include <cstdlib>

#include <windows.h>
#include <d3d12.h>
#include <array>
#include <map>
#include <memory>
#include <vector>

#include "d3d12_xs_device.h"
#include "../src/gfx10_descriptors.h"
#include "../src/metal_context.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../src/root_signature.h"
#include "../src/subgroup_size.h"

namespace leonida {
class MetalPresenter;
struct PresentPlanes;
}

namespace leonida::label { struct Write; }
namespace rt::d3d12 {
bool record_gpu_label(ID3D12GraphicsCommandList* list, const leonida::label::Write& write);


// `device` (optional) is what the object's GetDevice returns; the object holds a reference to it.
HRESULT create_command_queue(leonida::MetalContext& context, const D3D12_COMMAND_QUEUE_DESC& desc, REFIID riid,
                             void** out, IUnknown* device = nullptr);
// The Metal queue and fence behind this module's ID3D12CommandQueue / ID3D12Fence (the Xbox proxies unwrapped), or
// null for another object.
leonida::MetalQueue* metal_queue_of(ID3D12CommandQueue* queue);
leonida::gpu::Fence* metal_fence_of(ID3D12Fence* fence);
HRESULT create_command_allocator(D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** out, IUnknown* device = nullptr);
HRESULT create_command_list(D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* allocator, REFIID riid,
                            void** out, IUnknown* device = nullptr, ID3D12PipelineState* initial_state = nullptr);
HRESULT create_fence(leonida::MetalContext& context, UINT64 initial_value, D3D12_FENCE_FLAGS flags, REFIID riid,
                     void** out, IUnknown* device = nullptr);
HRESULT create_command_signature(leonida::IndirectKind kind, UINT byte_stride, UINT packed_size, REFIID riid, void** out,
                                 IUnknown* device, std::span<const D3D12_INDIRECT_ARGUMENT_DESC> prefix);
bool command_signature_info(ID3D12CommandSignature* signature, leonida::IndirectKind& kind, UINT& byte_stride,
                            bool& terminal_only);

// ID3D12Device1::SetEventOnMultipleFenceCompletion on fences from create_fence: sets `event` once every fence
// (D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL) or any fence (_ANY) has reached its value; a null event blocks until then.
HRESULT set_event_on_multiple_fence_completion(leonida::MetalContext& context, ID3D12Fence* const* fences,
                                               const UINT64* values, UINT count,
                                               D3D12_MULTIPLE_FENCE_WAIT_FLAGS flags, HANDLE event,
                                               IUnknown* device = nullptr);

bool is_sparse_image(ID3D12Resource* resource);

// Colour image used for unordered accesses to an Xbox single-plane depth surface.
struct XboxDepthStorage {
    leonida::ResourceId colour = 0;
    UINT mips = 0;
    UINT layers = 0;
};
XboxDepthStorage xbox_depth_storage(ID3D12Resource* resource);

// What the device of this layer was created with. The name is the cut point d3d12_xs_device.cpp tests for "a device
// of this layer" (vulkan_device_info); on Metal only the fields the front end reads are kept.
struct VulkanDeviceInfo {
    bool presentation = true;
    std::string shader_dir;
    std::uint32_t max_push_constants_size = 4096;
    bool push_constants_need_fallback = false;
    leonida::SubgroupSizeCaps subgroup{.subgroup_size_control = false, .compute_full_subgroups = false,
                                       .min_size = 32, .max_size = 32, .default_size = 32,
                                       .max_compute_workgroup_subgroups = 32, .required_stages = 0};
    bool bindless = true;
    bool bindless_partially_bound = true;
    bool sampler_anisotropy = true;
    bool sampler_mirror_clamp_to_edge = true;
    bool sampler_filter_minmax = false;
    bool calibrated_timestamps = false;
    bool conditional_rendering = false;
    bool storage_buffer_8bit = true;
    bool conservative_rasterization = false;
    bool host_image_copy = false;
    bool shader_fma = true;
    bool shader_subgroup_clock = false;
    bool shader_device_clock = false;
    bool texture_compression_bc = true;
    bool prepare_on_bind = false;
    bool pipeline_statistics_query = false;
    bool pipeline_creation_feedback = false;
    bool device_diagnostics_config = false;
    bool separate_depth_stencil_layouts = true;
    bool depth_range_unrestricted = false;
    bool swapchain_maintenance1 = false;
    bool swapchain_colorspace = false;
    bool image_view_min_lod = true;
    bool null_descriptor = true;
    bool vertex_attribute_divisor = true;
    bool fragment_shader_barycentric = true;
    bool fragment_shader_interlock = false;
    bool maintenance8 = false;
    bool maintenance10 = false;
    bool a8_render_target = false;
    unsigned float_controls = 0;
    bool buffer_device_address = true;
    bool ray_tracing = false;
    bool robust_image_access = true;
    bool sliced_3d_uav = true;
    bool xdxr_acceleration_structures = false;
    bool mesh_shader = true;
    bool mesh_shader_queries = false;
    bool shader_output_layer = true;
    bool shader_output_viewport_index = true;
    bool custom_border_color = false;
    bool draw_indirect_first_instance = true;
    bool multi_draw_indirect = true;
    bool robust_index_view = true;
    std::uint32_t max_draw_indirect_count = 1;
    bool shader_float64 = false;
    bool logic_op = false;
};


// The folder of translated shaders a device loads when VulkanDeviceInfo::shader_dir is empty: LEONIDA_SHADERS, else
// the path make_game recorded in shader_path.txt next to the executable (`config` names another file, for tests).
// Empty when neither names one, or when the path cannot be converted (a shader_path.txt that is not UTF-8); the reason
// is in `*error` then. Never throws.
std::filesystem::path configured_shader_directory(std::string* error, const std::filesystem::path& config = {});

// The D3D12 device on the Metal backend (gta6rt::metal::create_device), the one d3d12_xs.cpp makes for
// D3D12XboxCreateDevice. The name is d3d12_xs.cpp's cut point.
//
// The queue test settings (LEONIDA_VK_MAX_QUEUES_PER_FAMILY, LEONIDA_VK_GRAPHICS_FAMILY_ONLY; docs/testing.md) give
// the device fewer Vulkan queues than the GPU has, so the tests run the path where D3D12 queues share one. Only the
// tests apply them: the game's device (d3d12_xs, LEONIDA_D3D12=vulkan) passes `ignore`, so a variable left set does
// not serialize the game's queues; it says once that it ignores them.
enum class QueueTestSettings { apply, ignore };
HRESULT create_vulkan_device_standalone(REFIID riid, void** out,
                                        QueueTestSettings queue_test_settings = QueueTestSettings::apply);

// Tells a device from create_vulkan_device* that `outer` (the Xbox-ABI proxy that owns it) is what its children's
// GetDevice must return. No reference is taken: the outer object owns the device. `outer` may be the proxy's
// IDXGIDevice1 pointer (what create_device_proxy returns for that IID); the proxy itself becomes the outer. For a
// proxy it also makes the proxy's IDXGIDevice1::GetAdapter return this layer's adapter (d3d12_xs_device.h).
void set_device_outer(ID3D12Device* device, IUnknown* outer);

// Console Xbox CreateDescriptorHeap takes a 16-byte PC D3D12_DESCRIPTOR_HEAP_DESC followed by address/size at
// +0x10/+0x18 (issue #313 comment 7107265058). This helper creates a heap with that console ABI contract on a
// device from create_vulkan_device*; it is intended for the Xbox device-proxy slot 14 path, not normal PC callers.
HRESULT create_console_descriptor_heap(ID3D12Device* device, const D3D12_DESCRIPTOR_HEAP_DESC* desc,
                                       D3D12_GPU_VIRTUAL_ADDRESS console_address, std::uint64_t console_size,
                                       REFIID riid, void** out);

// Resolves the resource that a UAV clear writes. Xbox depth/stencil UAVs name one of two color images by T#.
leonida::ResourceId uav_image_resource(ID3D12Resource* resource, std::uint64_t address);
// What a device from create_vulkan_device* was created with (enabled features included), or null. For tests.
const VulkanDeviceInfo* vulkan_device_info(ID3D12Device* device);
// The device's pipeline workers (src/pipeline_workers.h) and what they have compiled; zeros without any. For tests.
leonida::PipelineWorkers::Stats pipeline_worker_stats(ID3D12Device* device);
// True when a slot-133 buffer is a shadow copied from the title's pages, false when they were imported. For tests.
bool title_shadow_buffer(ID3D12Resource* resource);
// Slot-133 imports (VK_EXT_external_memory_host) whose memory `device` holds: an import declined after its memory
// was allocated is freed before create_buffer_at_host_address returns. For tests.
std::uint64_t title_host_imports(ID3D12Device* device);
// VirtualQuery calls made by the title-buffer shadows in their latest refresh. For tests.
std::uint64_t title_shadow_refresh_queries(ID3D12Device* device);
// The trackers behind the title-buffer shadows and title windows (copies whose pages overlap or touch share one).
// For tests.
std::uint64_t title_shadow_trackers(ID3D12Device* device);
// What the title-buffer shadows' latest refresh did, summed over their trackers: GetWriteWatch calls, write-watched
// pages found written, the write faults taken since they were made, and the bytes of covered pages write-watched and
// hot (asked about rather than guarded). For tests.
struct TitleShadowWatch {
    std::uint64_t asks = 0, written = 0, faults = 0, watched_bytes = 0, hot_bytes = 0;
};
TitleShadowWatch title_shadow_watch(ID3D12Device* device);
// True when `device` has VK_KHR_acceleration_structure + VK_KHR_ray_query enabled.
bool device_supports_ray_tracing(ID3D12Device* device);

// Xbox device slot 133 (the console's CreatePlacedResourceX): a buffer on the caller's own pages at `address`, which
// becomes its console VA. The pages are imported with VK_EXT_external_memory_host, so what the CPU writes there is
// what the GPU reads. A buffer description only; E_NOTIMPL on a device that is not the Vulkan layer, without the
// extension, or for a texture.
HRESULT create_buffer_at_host_address(ID3D12Device* device, void* address, const D3D12_RESOURCE_DESC& desc,
                                      std::uint32_t layout, REFIID riid, void** out, const D3D12_CLEAR_VALUE* clear = nullptr);


// Whether the requested colour format has an integer numeric class.
bool color_resolve_format_is_integer(DXGI_FORMAT format);

// Whether both resources use the requested typed, non-integer colour format for a regional average resolve.
bool color_resolve_format_matches(DXGI_FORMAT format, ID3D12Resource* dst, ID3D12Resource* src);

// Whether a resolve of one depth or stencil plane can be replayed: both resources are depth-stencil textures of
// `format` (a depth format, or a plane's shader-resource format such as R24_UNORM_X8_TYPELESS), the source
// multisampled and the destination not, the same plane on both sides, MIN or MAX (or AVERAGE of the depth plane by
// its own format), the destination offset the source rectangle's corner (or 0, 0 without one), and the Vulkan device
// has the mode.
bool depth_stencil_resolve_supported(DXGI_FORMAT format, ID3D12Resource* dst, UINT dst_subresource, UINT x, UINT y,
                                     ID3D12Resource* src, UINT src_subresource, const D3D12_RECT* rect,
                                     D3D12_RESOLVE_MODE mode);

// The maxPushConstantsSize in bytes of the Vulkan physical device behind `device`, or 0 if not a Vulkan device.
std::uint32_t device_max_push_constants_size(ID3D12Device* device);

// True when `device`'s Vulkan physical device has maxPushConstantsSize < 144 bytes, requiring
// the uniform-buffer fallback for the 16-byte tail (heap/code base).
bool device_push_constants_need_fallback(ID3D12Device* device);

// True when `device` puts upload-heap buffers in DEVICE_LOCAL | HOST_VISIBLE memory: its GPU has Resizable BAR and
// LEONIDA_UPLOAD_SYSMEM is not set (D3D-96).
bool device_upload_in_vram(ID3D12Device* device);

// True when the device can map reserved buffers onto its D3D12 heaps.
bool device_sparse_buffer_tiles(ID3D12Device* device);

// The size in bytes of the device's GPU address arena, or 0 without one. For tests.
std::uint64_t device_arena_size(ID3D12Device* device);
// Copies `bytes` of the device's GPU address arena at console VA `va` (what shaders read there: a heap's GPU copy,
// the null slots after it) into `out`. False without an arena or when the range is not inside it. For tests.
bool read_arena(ID3D12Device* device, std::uint64_t va, void* out, std::size_t bytes);

// The blocks of device memory that textures share (src/image_memory.h): how many, their bytes, and the textures in
// them. For tests.
struct ImageMemoryStats {
    std::uint64_t blocks = 0;
    std::uint64_t block_bytes = 0;
    std::uint64_t textures = 0;
};
ImageMemoryStats image_memory_stats(ID3D12Device* device);

// The bindless index the descriptor hash table holds for the 8 key dwords of a T# (or the 4 of an S# followed by
// 0xFFFFFFFF x4), 0 (the fallback) when it has none or `device` has no such set. For tests.
std::uint32_t bindless_hash_lookup(ID3D12Device* device, const std::uint32_t (&key)[8]);
// The free indices of that set's 2D image and sampler arrays: how many more distinct 2D T#s and S#s
// CreateShaderResourceView and CreateSampler can register before they log that the array is full. Zeros when
// `device` has no such set. For tests.
struct BindlessRoom {
    std::uint32_t images_2d = 0;
    std::uint32_t samplers = 0;
};
BindlessRoom bindless_room(ID3D12Device* device);

// What a T# the title wrote itself into a console descriptor heap came to (bind_title_image).
enum class TitleImage { known, bound, unknown, refused, not_tsharp };

// Statistics on unbound T# descriptors refused during console descriptor heap binding.
struct UnboundTsharpReasonStats {
    std::size_t distinct_slots = 0;
    std::uint64_t total_refusals = 0;
};

struct UnboundTsharpStats {
    std::map<std::string, UnboundTsharpReasonStats> by_reason;
    std::size_t total_distinct_slots = 0;
    std::uint64_t total_refusals = 0;
};

// Returns statistics on unbound T# descriptor refusals across console descriptor heaps.
UnboundTsharpStats unbound_tsharp_stats();
// Resets unbound T# refusal statistics (for testing).
void reset_unbound_tsharp_stats();
// Records an unbound T# refusal (for testing).
void note_refused_tsharp_for_test(std::uint64_t heap_va, std::size_t offset,
                                  const std::array<std::uint32_t, 8>& words, const char* why);

// Simulates console descriptor heap T# binding for a synthetic descriptor against registered resources.
// Returns TitleImage::bound on success, TitleImage::refused on rule violation (with *why set to the refusal string),
// or TitleImage::unknown / TitleImage::not_tsharp.
TitleImage test_bind_title_image(ID3D12Device* device, const std::array<std::uint32_t, 8>& words,
                                 const char** why = nullptr);

// Returns the internal image address registered for a resource in the device's image table.
std::uint64_t resource_image_address(ID3D12Resource* resource);
// Gives each S# a sampler in that set and a key in its hash table, as CreateSampler does, without a heap slot: for
// the literal S#s a shader builds from immediates (`literal_samplers` in the shader index). An S# that has them
// already is left as it is. E_INVALIDARG when `device` is not such a device; E_FAIL when one of them could not be
// registered (no bindless set, or its sampler array is full), the others are registered all the same.
HRESULT register_samplers(ID3D12Device* device, std::span<const leonida::gfx10::SamplerWords> samplers);
// True when GetClockCalibration on `queue` reads calibrated timestamps, false when it submits a timestamp query
// (or `queue` is not one of this module's queues). For tests.
bool queue_uses_calibrated_timestamps(ID3D12CommandQueue* queue);

// An IDXGIFactory4 bound to `device`. Its swapchains present through the device's MetalPresenter.
HRESULT create_dxgi_factory(ID3D12Device* device, REFIID riid, void** out);

// SDRWhiteLevel of DISPLAYCONFIG_SDR_WHITE_LEVEL (1000 = 80 nits) to nits.
float sdr_white_nits_from_level(unsigned long level);

// Puts back buffers of a device from create_vulkan_device* into a window (leonida::MetalPresenter), for the
// presentation front end the game uses (DXGI or PresentX, still open). Holds a reference to the device.
class WindowPresenter {
public:
    // `device` is kept alive while the presenter lives; null when the owner guarantees that itself.
    WindowPresenter(IUnknown* device, std::unique_ptr<leonida::MetalPresenter> presenter);
    ~WindowPresenter();
    WindowPresenter(const WindowPresenter&) = delete;
    WindowPresenter& operator=(const WindowPresenter&) = delete;

    // `back_buffer` is a colour texture in D3D12's PRESENT state. sync_interval 0 presents without vsync.
    // S_OK also covers a skipped present while the window is minimised.
    // `src_rect` (when not null) limits the blit to that part of the texture, which is stretched to the window.
    // `tear` (with sync_interval > 0): present in the tearing mode when the swapchain has one (tear_supported()), so a
    // frame that missed its vblank is shown at once; last_present_tore() tells whether that happened.
    HRESULT present(ID3D12Resource* back_buffer, UINT sync_interval, const D3D12_RECT* src_rect = nullptr,
                    bool tear = false);
    // A black frame, nothing read (PresentX with no planes).
    HRESULT present_blank(UINT sync_interval);
    // Show the shell image through startup clears, then hand off to the first non-black output.
    bool present_splash(ID3D12Resource* image);
    // One or two planes composed (VulkanPresenter::present_planes); `planes` names the textures by ResourceId, so the
    // caller fills it with resource_id(). DXGI_ERROR_UNSUPPORTED for a source the compose pass cannot read.
    HRESULT present_planes(const leonida::PresentPlanes& planes, UINT sync_interval, bool tear = false);
    // The swapchain holds HDR10 (ST.2084) values; and a test hook (VulkanPresenter::force_hdr_encoding).
    bool hdr_active() const;
    void force_hdr_encoding(bool on);
    std::uint64_t composed_presents() const;  // presents that went through the compose pass
    // Nits of SDR white the user set in Windows for the output the window is on (DISPLAYCONFIG_SDR_WHITE_LEVEL), or 0 when
    // it cannot be read (no window, no display config, not Windows). Only meaningful while hdr_active().
    float display_sdr_white_nits() const;
    void set_window(HWND window) { window_ = window; }
    bool tear_supported() const;
    bool last_present_tore() const;
    std::uint64_t tear_presents() const;
    // Screenshot support for tests: every presented frame is also copied to host memory (call before presenting).
    bool enable_capture();
    // See VulkanPresenter::set_abort: polled while a present waits for a swapchain image.
    void set_abort(std::function<bool()> abort);
    // See VulkanPresenter::read_back: false after a present skipped while the window is minimised, too.
    bool read_back(std::vector<std::uint8_t>& pixels, std::uint32_t* width, std::uint32_t* height);
    std::uint32_t width() const;
    std::uint32_t height() const;
    std::uint64_t presented() const;

private:
    IUnknown* device_;
    HWND window_ = nullptr;
    std::unique_ptr<leonida::MetalPresenter> presenter_;
};

// Whether `device` (from create_vulkan_device*) was created with VK_EXT_swapchain_colorspace on its instance.
bool device_has_swapchain_colorspace(ID3D12Device* device);

// A placeholder PSO for one the layer refused, so the title binds an object, not null. Its draws, mesh draws and
// dispatches are skipped and counted under `reason`. S_OK with *out set, or the failure.
HRESULT make_refused_pipeline_state(ID3D12Device* device, bool compute, const std::string& reason, REFIID riid,
                                    void** out);

// Null when `device` is not a Vulkan device created with presentation, or the window cannot be presented to.
// The presenter holds a reference on the device, or on the Xbox device proxy that is its outer identity, so it may
// outlive the caller's reference. `hold_device` false takes none: for a presenter that the device proxy owns itself
// and destroys before its device (the presentation gta6rt's proxy builds, d3d12_xs_device.cpp), where a reference
// to the proxy would keep the proxy, the presenter and its window alive for good.
std::unique_ptr<WindowPresenter> create_window_presenter(ID3D12Device* device, HWND window, bool hold_device = true);

// Where each root parameter's arguments go in the 32 user-data dwords the translated shaders read (docs section
// 3.5). Built when a Vulkan device creates the root signature. A signature can be bound at either bind point, so it
// carries both layouts:
//   - graphics: rt::rootsig::pack_stage(Stage::Pixel), the console layout of vertex, pixel and geometry shaders
//     (from s8, one dword per table), which the game's own shaders read: VS_Blit/PS_BlitColour and the Scaleform
//     VS load their table's first descriptor from s[8:9] with s9 forced to 0. A vertex, pixel or geometry shader
//     reads SGPR i from push dword i, so root dword k is push dword 8 + k. A signature that would spill there
//     (more than 24 dwords) keeps rt::rootsig::pack() (from s0, two dwords per table), logged: the graphics spill
//     array is not modelled. A mesh shader's root arguments start at s16; the translator loads its s16.. from push
//     dwords 8.. ("root_window" in index.json), so the same block serves both stages of a mesh pipeline. Its window
//     is 16 dwords: mesh_ok says whether Stage::Mesh places the signature too, and a mesh draw with one it refuses
//     invalidates the list (D3D-448-GRAPHICS);
//   - compute: rt::rootsig::pack_stage(Stage::Compute), the measured console layout (from s4, one dword per table).
// A layout that refuses the signature is marked unusable; binding the signature there invalidates the list.
// LEONIDA_GRAPHICS_STAGE_LAYOUT=0 keeps rt::rootsig::pack() (from s0, two dwords per address) for every graphics
// signature instead of the console layout of the vertex, pixel and geometry stages described above.
// (The switch between the two packings is from the patch graphics-root-layout of
// Sigtryggur-Mani-Gudmundsson/graphics-runtime-patches at d9a0bc1b, author smg as "Codex", GPL-3.0.)
inline bool local_graphics_stage_layout() {
    static const bool on = [] { const char* v = std::getenv("LEONIDA_GRAPHICS_STAGE_LAYOUT"); return !(v && *v == '0'); }();
    return on;
}

struct RootArgumentLayout {
    struct Parameter {
        rt::rootsig::ParamType type;
        std::uint32_t user_offset;     // graphics: first push dword; unused for a parameter with no dwords
        std::uint32_t constant_count;  // constants only
        std::uint32_t compute_offset = rt::rootsig::kNoSgpr;  // compute: first user SGPR
        // A table: the slots its ranges declare, for the scan of title-written V#s before a submission
        // (src/title_va_alias.h) and the submission-time heap refresh. Views are capped at 1024 slots (an unbounded
        // range counts as 1024) and samplers at the shader-visible sampler heap limit. 0: not a table.
        std::uint32_t table_descriptors = 0;
    };
    std::vector<Parameter> parameters;
    std::uint32_t flags = 0;  // D3D12_ROOT_SIGNATURE_FLAGS from the serialized blob
    // The graphics layout is the console's (pack_stage(Stage::Pixel)): a table takes one dword. False: pack()'s, two.
    bool graphics_console = false;
    bool graphics_ok = true;
    // The console graphics layout also fits a mesh shader's 16-dword window (s16..s31 = push dwords 8..23). False
    // for a signature on pack()'s s0 layout, which a mesh shader translated with the root window cannot read.
    bool mesh_ok = false;
    bool compute_ok = true;
    // One past the last user-data dword the explicit root arguments take at each bind point, where the console puts
    // the directly indexed heaps (resource heap, then sampler heap). kNoSgpr: no such dword (the compute arguments
    // spill, or graphics keeps pack()'s s0 layout).
    std::uint32_t compute_arguments_end = rt::rootsig::kNoSgpr;
    std::uint32_t graphics_arguments_end = rt::rootsig::kNoSgpr;
};

// For the command list: the argument layout of a root signature from a Vulkan device, and the id it records for a
// compute pipeline state (0: not a compute PSO of a Vulkan device).
const RootArgumentLayout* root_signature_layout(ID3D12RootSignature* signature);
std::uintptr_t compute_pipeline_id(ID3D12PipelineState* pipeline);
std::uintptr_t graphics_pipeline_id(ID3D12PipelineState* pipeline);
// What each stage of a Vulkan device's PSO asked for (src/subgroup_size.h), in creation order; null for another PSO.
const std::vector<leonida::SubgroupRequest>* pipeline_subgroup_requests(ID3D12PipelineState* pipeline);
// PSOs of any Vulkan device not yet deleted, counting one whose deletion waits for its running build (tests).
long live_pipeline_states();

// Unwraps an Xbox-ABI heap proxy if needed and inspects one of this module's descriptor heaps; returns its owning
// device identity pointer, or nullptr when `heap` is null or belongs to another implementation.
IUnknown* descriptor_heap_info(ID3D12DescriptorHeap* heap, D3D12_DESCRIPTOR_HEAP_DESC& desc,
                               D3D12_GPU_VIRTUAL_ADDRESS& gpu_base);

// Xbox device slots 70/71: a buffer view of the title's own memory, with no resource object. `view` is a PC
// D3D12_SHADER_RESOURCE_VIEW_DESC (or, `unordered`, D3D12_UNORDERED_ACCESS_VIEW_DESC) of a buffer, `address` the
// memory's console VA and `width` its bytes. Writes the V# into `destination` and returns true; false when `device`
// is not this module's or the layer cannot write that view (the slot is left as it was).
bool write_placed_buffer_view(ID3D12Device* device, bool unordered, const void* view, std::uint64_t address,
                              std::uint64_t width, D3D12_CPU_DESCRIPTOR_HANDLE destination);

// Xbox placed texture SRV (slot 70) or UAV (slot 71). The 56-byte console resource description includes the
// XG layout at +44. The PC SRV/UAV description and placement address select the view. A GPU-writable
// image must already exist for a UAV. False leaves the destination descriptor zeroed.
bool write_placed_texture_view(ID3D12Device* device, bool unordered, const std::uint32_t* resource_words,
                               const std::uint32_t* view_words, std::uint64_t address,
                               D3D12_CPU_DESCRIPTOR_HANDLE destination);

// The VA shaders read a descriptor table at: for one inside a console descriptor heap (Xbox slot 14) whose arena copy
// lies elsewhere, the matching VA in that copy; any other VA unchanged.
D3D12_GPU_VIRTUAL_ADDRESS console_descriptor_table_va(D3D12_GPU_VIRTUAL_ADDRESS va);

// XDXR acceleration-structure commands (docs/d3d12_vulkan.md 4.8) of a list of `device`: every address the command
// writes or reads, as the title named it, becomes the VA shaders reach that memory by (src/title_va_alias.h: a
// slot-133 buffer's import or shadow, a window of the title's memory, a geometry copy), checked to hold every byte the
// builder touches there. False (logged by the device) when an address has no such copy: the command must not be
// recorded, since heap_base + a title address is no allocation. True and unchanged when `device` is not a Vulkan
// device of this module or builds KHR structures.
bool resolve_acceleration_structure_build(IUnknown* device, leonida::BuildAccelerationStructure& build);
bool resolve_acceleration_structure_copy(IUnknown* device, leonida::CopyAccelerationStructure& copy);
bool resolve_acceleration_structure_postbuild(IUnknown* device, leonida::AccelerationStructurePostbuild& info);

// Reads the shader-visible descriptor heap GPU base addresses currently bound on `list` (0 when unbound).
// Returns false when `list` is not one of this module's command lists.
bool bound_descriptor_heap_bases(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS& cbv_srv_uav,
                                 D3D12_GPU_VIRTUAL_ADDRESS& sampler);

// The size of every descriptor slot (GetDescriptorHandleIncrementSize), for handle ranges.
inline constexpr UINT descriptor_size = 32;

// ExecuteCommandLists returns void; this is what the last call on the queue did.
leonida::ReplayResult last_execute_result(ID3D12CommandQueue* queue);

// True when the current thread or process token has SeIncreaseBasePriorityPrivilege enabled (or the test override
// forces it), which D3D12 requires for D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME.
bool has_global_realtime_privilege();
// Test override for has_global_realtime_privilege(): -1 queries the token (default), 0 forces denied, 1 forces granted.
void set_global_realtime_privilege_override_for_test(int state);

// The front/back stencil references last recorded by ID3D12GraphicsCommandList8::OMSetFrontAndBackStencilRef on
// `list`. The values are kept for the future pipeline path, which is what would apply them. False when `list` is
// not one of this module's command lists.
bool front_back_stencil_ref(ID3D12GraphicsCommandList8* list, UINT& front, UINT& back);

// Makes `list` hold a reference to `object` until its next Reset or its release, as it holds the pipeline states it
// names: for an object only the recording uses, such as a shim's own scratch buffer. A list is Reset once it has
// been executed (or a held batch, src/queue_holds.h, keeps it pinned until it has run), and a resource released
// after its submission is destroyed through the device's deferred queue once that submission has retired, so the
// object outlives every GPU use the recording makes of it. False when `list` is not one of this module's command
// lists or `object` is null.
bool hold_until_reset(ID3D12GraphicsCommandList* list, IUnknown* object);

// A copy of the commands `list` has recorded so far, for tests of what a bundle puts into its parent. False when
// `list` is not one of this module's command lists.
bool recorded_commands(ID3D12GraphicsCommandList* list, std::vector<leonida::Command>& commands);

inline leonida::ResourceId resource_id(const ID3D12Resource* resource) {
    return reinterpret_cast<leonida::ResourceId>(rt::d3d12_xs::texture_backing(resource));
}

// A query heap's ID in the command-list IR and the VulkanContext: its pointer value, as for resources.
inline leonida::QueryHeapId query_heap_id(const ID3D12QueryHeap* heap) {
    return reinterpret_cast<leonida::QueryHeapId>(heap);
}

}  // namespace rt::d3d12
