// port/d3d12_metal: copy of d3d12_vulkan/shims/dxgi_com.cpp; adapter over gta6rt::gpu, no swap chains (ORIGIN.md).
#include "dxgi_com.h"
#include "d3d12_stubs.h"
#include "display_info.h"


#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace rt::dxgi {
namespace {

// Native DXGI accepts up to 40 queued frames on a waitable swapchain.
constexpr UINT max_waitable_frame_latency = 40;

void stub(const char* method) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    std::lock_guard lock(mutex);
    if (seen.insert(method).second) std::fprintf(stderr, "[rt] dxgi: unimplemented %s\n", method);
}

// IDXGIObject private data, shared by the factory, adapter and swapchain. SetPrivateData keeps a copy of the bytes;
// SetPrivateDataInterface holds one reference until the entry is replaced, removed or the owner dies (Microsoft:
// IDXGIObject::SetPrivateDataInterface). Zero bytes remove the entry. GetPrivateData gives an interface back with a
// reference added, and a buffer too small for the entry gets DXGI_ERROR_MORE_DATA and the size to retry with.
class PrivateDataStore {
public:
    PrivateDataStore() = default;
    PrivateDataStore(const PrivateDataStore&) = delete;
    PrivateDataStore& operator=(const PrivateDataStore&) = delete;
    ~PrivateDataStore() {
        for (auto& entry : data_)
            if (entry.object) entry.object->Release();
    }

    HRESULT get(REFGUID guid, UINT* size, void* data) {
        if (!size) return E_INVALIDARG;
        std::lock_guard lock(mutex_);
        for (const auto& entry : data_) {
            if (entry.key != guid) continue;
            const UINT bytes = entry.object ? UINT(sizeof(IUnknown*)) : UINT(entry.bytes.size());
            if (data && *size < bytes) {
                *size = bytes;
                return DXGI_ERROR_MORE_DATA;
            }
            if (data) {
                if (entry.object) {
                    entry.object->AddRef();
                    std::memcpy(data, &entry.object, sizeof(IUnknown*));
                } else {
                    std::memcpy(data, entry.bytes.data(), bytes);
                }
            }
            *size = bytes;
            return S_OK;
        }
        *size = 0;
        return DXGI_ERROR_NOT_FOUND;
    }

    HRESULT set(REFGUID guid, UINT size, const void* data) {
        if (size && !data) return E_INVALIDARG;
        Entry entry{guid, {}, nullptr};
        if (size) {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            entry.bytes.assign(bytes, bytes + size);
        }
        store(std::move(entry), size != 0);
        return S_OK;
    }

    HRESULT set_interface(REFGUID guid, const IUnknown* object) {
        IUnknown* held = const_cast<IUnknown*>(object);
        if (held) held->AddRef();
        store(Entry{guid, {}, held}, held != nullptr);
        return S_OK;
    }

private:
    struct Entry {
        GUID key;
        std::vector<std::uint8_t> bytes;
        IUnknown* object;
    };

    // The replaced interface is released after the lock is dropped, since its Release may run arbitrary code.
    void store(Entry entry, bool keep) {
        IUnknown* released = nullptr;
        {
            std::lock_guard lock(mutex_);
            for (auto it = data_.begin(); it != data_.end(); ++it) {
                if (it->key != entry.key) continue;
                released = it->object;
                data_.erase(it);
                break;
            }
            if (keep) data_.push_back(std::move(entry));
        }
        if (released) released->Release();
    }

    std::mutex mutex_;
    std::vector<Entry> data_;
};

bool valid_flip_buffer_count(UINT count) {
    return count >= 2 && count <= DXGI_MAX_SWAP_CHAIN_BUFFERS;
}

bool is_direct_present_queue(IUnknown* candidate) {
    ID3D12CommandQueue* queue = nullptr;
    if (!candidate || FAILED(candidate->QueryInterface(IID_PPV_ARGS(&queue))) || !queue) return false;
    const bool direct = queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue->Release();
    return direct;
}

struct ReservationState {
    std::atomic<UINT64> local{0};
    std::atomic<UINT64> non_local{0};
};

// Video-memory reservations are scoped per physical adapter in the process (matching native DXGI,
// where separate IDXGIAdapter3 handles opened for the same LUID observe each other's reservations).
std::shared_ptr<ReservationState> adapter_reservation_state(LUID luid, const void* physical) {
    static std::mutex mutex;
    static std::map<std::tuple<std::uint32_t, std::int32_t, std::uintptr_t>, std::shared_ptr<ReservationState>> table;
    const bool has_luid = (luid.LowPart != 0 || luid.HighPart != 0);
    const auto key =
        has_luid ? std::make_tuple(std::uint32_t(luid.LowPart), std::int32_t(luid.HighPart), std::uintptr_t(0))
                 : std::make_tuple(std::uint32_t(0), std::int32_t(0), reinterpret_cast<std::uintptr_t>(physical));
    std::lock_guard lock(mutex);
    auto& entry = table[key];
    if (!entry) entry = std::make_shared<ReservationState>();
    return entry;
}

// Snapshot of a Win32 monitor (or synthetic headless fallback) exposed through IDXGIOutput*.
// Defaults target a 1920x1080@60 Hz primary output; Win32 enumeration must also fill desktop_coords explicitly.
struct DisplayInfo {
    HMONITOR monitor = nullptr;
    WCHAR device_name[CCHDEVICENAME] = u"\\\\.\\DISPLAY1";  // matches DXGI_OUTPUT_DESC::DeviceName (32 WCHARs, NUL-terminated)
    RECT desktop_coords{0, 0, 1920, 1080};  // Default desktop coordinates
    DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY;
    UINT current_width = 1920;
    UINT current_height = 1080;
    UINT current_refresh_hz = 60;
    bool primary = true;
};

static_assert(sizeof(DisplayInfo{}.device_name) == sizeof(DXGI_OUTPUT_DESC{}.DeviceName));

// macOS: the displays CoreGraphics lists (platform/display_info.h), primary first; HMONITOR is the display id. A
// headless host gets the single 1920x1080@60 default.
std::vector<DisplayInfo> enumerate_displays() {
    std::vector<DisplayInfo> displays;
    for (const rthost::Display& d : rthost::displays()) {
        DisplayInfo info{};
        info.monitor = reinterpret_cast<HMONITOR>(static_cast<std::uintptr_t>(d.id));
        info.desktop_coords = {LONG(d.x), LONG(d.y), LONG(d.x + LONG(d.width)), LONG(d.y + LONG(d.height))};
        info.primary = d.primary;
        const std::string name = "\\\\.\\DISPLAY" + std::to_string(displays.size() + 1);
        for (std::size_t i = 0; i < name.size() && i + 1 < CCHDEVICENAME; ++i) info.device_name[i] = WCHAR(name[i]);
        info.device_name[std::min<std::size_t>(name.size(), CCHDEVICENAME - 1)] = 0;
        info.current_width = d.width;
        info.current_height = d.height;
        info.current_refresh_hz = d.refresh_hz;
        info.rotation = d.rotation_degrees == 90    ? DXGI_MODE_ROTATION_ROTATE90
                        : d.rotation_degrees == 180 ? DXGI_MODE_ROTATION_ROTATE180
                        : d.rotation_degrees == 270 ? DXGI_MODE_ROTATION_ROTATE270
                                                    : DXGI_MODE_ROTATION_IDENTITY;
        displays.push_back(info);
    }
    if (displays.empty()) displays.push_back(DisplayInfo{});
    return displays;
}

bool is_supported_display_format(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return true;
    default:
        return false;
    }
}

std::vector<DXGI_MODE_DESC> collect_modes(const DisplayInfo& info, DXGI_FORMAT fmt) {
    std::vector<DXGI_MODE_DESC> modes;
    if (!is_supported_display_format(fmt)) return modes;

    std::set<std::tuple<UINT, UINT, UINT>> seen;
    auto push_mode = [&](UINT w, UINT h, UINT hz) {
        if (!w || !h || !hz) return;
        if (!seen.emplace(w, h, hz).second) return;
        DXGI_MODE_DESC m{};
        m.Width = w;
        m.Height = h;
        m.RefreshRate = {hz, 1};
        m.Format = fmt;
        m.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
        m.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
        modes.push_back(m);
    };

    if (info.monitor)
        for (const auto& mode : rthost::display_modes(std::uint32_t(reinterpret_cast<std::uintptr_t>(info.monitor))))
            push_mode(mode[0], mode[1], mode[2]);
    push_mode(info.current_width, info.current_height, info.current_refresh_hz);

    std::sort(modes.begin(), modes.end(), [](const DXGI_MODE_DESC& a, const DXGI_MODE_DESC& b) {
        return std::tie(a.Width, a.Height, a.RefreshRate.Numerator) <
               std::tie(b.Width, b.Height, b.RefreshRate.Numerator);
    });
    return modes;
}

constexpr DXGI_MODE_DESC1 to_mode_desc1(const DXGI_MODE_DESC& m) noexcept {
    return {m.Width, m.Height, m.RefreshRate, m.Format, m.ScanlineOrdering, m.Scaling, FALSE};
}

constexpr DXGI_MODE_DESC from_mode_desc1(const DXGI_MODE_DESC1& m) noexcept {
    return {m.Width, m.Height, m.RefreshRate, m.Format, m.ScanlineOrdering, m.Scaling};
}

template <class DescT, class ConvertFn>
HRESULT fill_mode_list(const DisplayInfo& info, DXGI_FORMAT fmt, UINT* num_modes, DescT* out, ConvertFn convert) {
    if (!num_modes) return DXGI_ERROR_INVALID_CALL;
    const std::vector<DXGI_MODE_DESC> modes = collect_modes(info, fmt);
    const UINT total = static_cast<UINT>(modes.size());
    if (!out) {
        *num_modes = total;
        return S_OK;
    }
    const UINT to_copy = std::min(*num_modes, total);
    for (UINT i = 0; i < to_copy; ++i) out[i] = convert(modes[i]);
    // Host DXGI writes the number of copied entries back into *pNumModes when returning MORE_DATA.
    *num_modes = to_copy;
    return (to_copy < total) ? DXGI_ERROR_MORE_DATA : S_OK;
}

HRESULT find_closest_mode(const DisplayInfo& info, const DXGI_MODE_DESC& req, IUnknown* concerned_device,
                          DXGI_MODE_DESC& out) {
    out = {};
    if (req.Format == DXGI_FORMAT_UNKNOWN && !concerned_device) return DXGI_ERROR_INVALID_CALL;
    const DXGI_FORMAT target_fmt = (req.Format == DXGI_FORMAT_UNKNOWN) ? DXGI_FORMAT_B8G8R8A8_UNORM : req.Format;
    const std::vector<DXGI_MODE_DESC> modes = collect_modes(info, target_fmt);
    if (modes.empty()) return DXGI_ERROR_NOT_FOUND;

    const UINT target_w = req.Width ? req.Width : info.current_width;
    const UINT target_h = req.Height ? req.Height : info.current_height;
    const bool has_refresh = (req.RefreshRate.Numerator > 0 && req.RefreshRate.Denominator > 0);
    const std::uint64_t target_mhz =
        has_refresh ? (std::uint64_t(req.RefreshRate.Numerator) * 1000ull) / req.RefreshRate.Denominator
                    : std::uint64_t(info.current_refresh_hz) * 1000ull;

    auto abs_diff = [](std::uint64_t a, std::uint64_t b) -> std::uint64_t { return a > b ? (a - b) : (b - a); };
    auto score = [&](const DXGI_MODE_DESC& m) {
        const std::uint64_t area_diff = abs_diff(std::uint64_t(m.Width) * m.Height, std::uint64_t(target_w) * target_h);
        const std::uint64_t dim_diff = abs_diff(m.Width, target_w) + abs_diff(m.Height, target_h);
        const std::uint64_t m_mhz =
            m.RefreshRate.Denominator ? (std::uint64_t(m.RefreshRate.Numerator) * 1000ull) / m.RefreshRate.Denominator : 0;
        const std::uint64_t hz_diff = abs_diff(m_mhz, target_mhz);
        return std::make_tuple(dim_diff, area_diff, hz_diff);
    };

    const DXGI_MODE_DESC* best = &modes[0];
    auto best_score = score(*best);
    for (std::size_t i = 1; i < modes.size(); ++i) {
        const auto s = score(modes[i]);
        if (s < best_score) {
            best = &modes[i];
            best_score = s;
        }
    }
    out = *best;
    return S_OK;
}

void wait_for_vblank() {
    using DwmFlushFn = HRESULT(WINAPI*)();
    static const DwmFlushFn dwm_flush = []() -> DwmFlushFn {
        // Resolve dynamically so dxgi_com.cpp needs no extra dwmapi.lib link entry in CMakeLists.txt.
        const HMODULE mod = LoadLibraryW(L"dwmapi.dll");
        return mod ? reinterpret_cast<DwmFlushFn>(GetProcAddress(mod, "DwmFlush")) : nullptr;
    }();
    if (dwm_flush && SUCCEEDED(dwm_flush())) return;
    Sleep(16);
}

// Win32/DXGI-backed IDXGIOutput6 reporting desktop monitor descriptors and display modes.
class Output final : public IDXGIOutput6 {
public:
    Output(IDXGIAdapter* parent, DisplayInfo info) : parent_(parent), info_(info) {
        if (parent_) parent_->AddRef();
    }

    ~Output() {
        if (parent_) parent_->Release();
    }

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (!left) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIOutput) ||
            riid == __uuidof(IDXGIOutput1) || riid == __uuidof(IDXGIOutput2) || riid == __uuidof(IDXGIOutput3) ||
            riid == __uuidof(IDXGIOutput4) || riid == __uuidof(IDXGIOutput5) || riid == __uuidof(IDXGIOutput6)) {
            *out = static_cast<IDXGIOutput6*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return DXGI_ERROR_NOT_FOUND; }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!parent_) return E_NOINTERFACE;
        return parent_->QueryInterface(riid, out);
    }

    // IDXGIOutput
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_OUTPUT_DESC* desc) override {
        if (!desc) return E_INVALIDARG;
        fill_base_desc(*desc);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDisplayModeList(DXGI_FORMAT fmt, UINT, UINT* num_modes, DXGI_MODE_DESC* desc) override {
        return fill_mode_list(info_, fmt, num_modes, desc, [](const DXGI_MODE_DESC& m) { return m; });
    }
    HRESULT STDMETHODCALLTYPE FindClosestMatchingMode(const DXGI_MODE_DESC* mode_to_match, DXGI_MODE_DESC* closest,
                                                      IUnknown* concerned_device) override {
        if (!mode_to_match || !closest) return DXGI_ERROR_INVALID_CALL;
        return find_closest_mode(info_, *mode_to_match, concerned_device, *closest);
    }
    HRESULT STDMETHODCALLTYPE WaitForVBlank() override {
        wait_for_vblank();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE TakeOwnership(IUnknown* device, BOOL) override {
        return device ? S_OK : E_INVALIDARG;
    }
    void STDMETHODCALLTYPE ReleaseOwnership() override {}
    HRESULT STDMETHODCALLTYPE GetGammaControlCapabilities(DXGI_GAMMA_CONTROL_CAPABILITIES* caps) override {
        if (caps) std::memset(caps, 0, sizeof(*caps));
        return DXGI_ERROR_INVALID_CALL;
    }
    HRESULT STDMETHODCALLTYPE SetGammaControl(const DXGI_GAMMA_CONTROL*) override { return DXGI_ERROR_INVALID_CALL; }
    HRESULT STDMETHODCALLTYPE GetGammaControl(DXGI_GAMMA_CONTROL*) override { return DXGI_ERROR_INVALID_CALL; }
    HRESULT STDMETHODCALLTYPE SetDisplaySurface(IDXGISurface*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData(IDXGISurface*) override { return DXGI_ERROR_INVALID_CALL; }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* stats) override {
        if (!stats) return DXGI_ERROR_INVALID_CALL;
        std::memset(stats, 0, sizeof(*stats));
        LARGE_INTEGER qpc{};
        if (QueryPerformanceCounter(&qpc)) stats->SyncQPCTime = qpc;
        return S_OK;
    }

    // IDXGIOutput1
    HRESULT STDMETHODCALLTYPE GetDisplayModeList1(DXGI_FORMAT fmt, UINT, UINT* num_modes,
                                                  DXGI_MODE_DESC1* desc) override {
        return fill_mode_list(info_, fmt, num_modes, desc, to_mode_desc1);
    }
    HRESULT STDMETHODCALLTYPE FindClosestMatchingMode1(const DXGI_MODE_DESC1* mode_to_match, DXGI_MODE_DESC1* closest,
                                                       IUnknown* concerned_device) override {
        if (!mode_to_match || !closest) return DXGI_ERROR_INVALID_CALL;
        DXGI_MODE_DESC best{};
        const HRESULT hr = find_closest_mode(info_, from_mode_desc1(*mode_to_match), concerned_device, best);
        *closest = SUCCEEDED(hr) ? to_mode_desc1(best) : DXGI_MODE_DESC1{};
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData1(IDXGIResource*) override { return DXGI_ERROR_INVALID_CALL; }
    HRESULT STDMETHODCALLTYPE DuplicateOutput(IUnknown* device, IDXGIOutputDuplication** dup) override {
        if (dup) *dup = nullptr;
        if (!device || !dup) return E_INVALIDARG;
        return DXGI_ERROR_UNSUPPORTED;
    }

    // IDXGIOutput2
    BOOL STDMETHODCALLTYPE SupportsOverlays() override { return FALSE; }

    // IDXGIOutput3
    HRESULT STDMETHODCALLTYPE CheckOverlaySupport(DXGI_FORMAT, IUnknown* device, UINT* flags) override {
        if (flags) *flags = 0;
        return (device && flags) ? S_OK : E_INVALIDARG;
    }

    // IDXGIOutput4
    HRESULT STDMETHODCALLTYPE CheckOverlayColorSpaceSupport(DXGI_FORMAT, DXGI_COLOR_SPACE_TYPE, IUnknown* device,
                                                            UINT* flags) override {
        if (flags) *flags = 0;
        return (device && flags) ? S_OK : E_INVALIDARG;
    }

    // IDXGIOutput5
    HRESULT STDMETHODCALLTYPE DuplicateOutput1(IUnknown* device, UINT, UINT, const DXGI_FORMAT*,
                                               IDXGIOutputDuplication** dup) override {
        if (dup) *dup = nullptr;
        if (!device || !dup) return E_INVALIDARG;
        return DXGI_ERROR_UNSUPPORTED;
    }

    // IDXGIOutput6
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_OUTPUT_DESC1* desc) override {
        if (!desc) return E_INVALIDARG;
        std::memset(desc, 0, sizeof(*desc));
        DXGI_OUTPUT_DESC base{};
        fill_base_desc(base);
        std::memcpy(desc->DeviceName, base.DeviceName, sizeof(desc->DeviceName));
        desc->DesktopCoordinates = base.DesktopCoordinates;
        desc->AttachedToDesktop = base.AttachedToDesktop;
        desc->Rotation = base.Rotation;
        desc->Monitor = base.Monitor;
        desc->BitsPerColor = 8;
        desc->ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        // Standard sRGB / ITU-R BT.709 primaries and baseline SDR luminance.
        desc->RedPrimary[0] = 0.64f;
        desc->RedPrimary[1] = 0.33f;
        desc->GreenPrimary[0] = 0.30f;
        desc->GreenPrimary[1] = 0.60f;
        desc->BluePrimary[0] = 0.15f;
        desc->BluePrimary[1] = 0.06f;
        desc->WhitePoint[0] = 0.3127f;
        desc->WhitePoint[1] = 0.3290f;
        desc->MinLuminance = 0.0f;
        desc->MaxLuminance = 80.0f;
        desc->MaxFullFrameLuminance = 80.0f;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CheckHardwareCompositionSupport(UINT* flags) override {
        if (!flags) return E_INVALIDARG;
        *flags = 0;
        return S_OK;
    }

private:
    void fill_base_desc(DXGI_OUTPUT_DESC& desc) const noexcept {
        std::memset(&desc, 0, sizeof(desc));
        std::memcpy(desc.DeviceName, info_.device_name, sizeof(desc.DeviceName));
        desc.DesktopCoordinates = info_.desktop_coords;
        desc.AttachedToDesktop = TRUE;
        desc.Rotation = info_.rotation;
        desc.Monitor = info_.monitor;
    }

    std::atomic<ULONG> refs_{1};
    IDXGIAdapter* parent_ = nullptr;
    DisplayInfo info_{};
};


// Vulkan-backed IDXGIAdapter4 reporting the physical device's name, IDs, LUID (matching
// ID3D12Device::GetAdapterLuid), memory heap sizes, and video memory budget info.

// The adapter's user-mode driver version, as DXGI's CheckInterfaceSupport(IDXGIDevice) reports it: what
// D3DKMTQueryAdapterInfo(KMTQAITYPE_UMD_DRIVER_VERSION) gives for the adapter's LUID (measured equal to host DXGI on
// an RTX 5060 Ti, 0x0020000000100842 for driver 621.14, and on WARP). gdi32's entry points are looked up at run
// time and the host dxgi.dll isn't used; 0 when they are missing (Wine) or the query fails.
// macOS: no gdi32/D3DKMT, and the Vulkan driver reports no Windows UMD version; 0 ("missing", as under Wine).
LARGE_INTEGER umd_driver_version(LUID) { return LARGE_INTEGER{}; }

class Adapter final : public IDXGIAdapter4 {
public:
    Adapter(IDXGIFactory4* parent, leonida::gpu::Device* gpu) : parent_(parent), gpu_(gpu) {
        if (parent_) parent_->AddRef();
        populate_desc();
        umd_version_ = umd_driver_version(desc_.AdapterLuid);
        reservations_ = adapter_reservation_state(desc_.AdapterLuid, gpu_);
    }

    ~Adapter() {
        if (parent_) parent_->Release();
    }

    LUID luid() const { return desc_.AdapterLuid; }

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (!left) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIAdapter) ||
            riid == __uuidof(IDXGIAdapter1) || riid == __uuidof(IDXGIAdapter2) || riid == __uuidof(IDXGIAdapter3) ||
            riid == __uuidof(IDXGIAdapter4)) {
            *out = static_cast<IDXGIAdapter4*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override {
        return private_data_.set(guid, size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* object) override {
        return private_data_.set_interface(guid, object);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override {
        return private_data_.get(guid, size, data);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!parent_) return E_NOINTERFACE;
        return parent_->QueryInterface(riid, out);
    }

    // IDXGIAdapter
    HRESULT STDMETHODCALLTYPE EnumOutputs(UINT index, IDXGIOutput** out) override {
        if (!out) return E_INVALIDARG;
        *out = nullptr;
        const std::vector<DisplayInfo> displays = enumerate_displays();
        if (index >= displays.size()) return DXGI_ERROR_NOT_FOUND;
        *out = new Output(static_cast<IDXGIAdapter*>(this), displays[index]);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC* desc) override {
        if (!desc) return E_INVALIDARG;
        std::memcpy(desc->Description, desc_.Description, sizeof(desc->Description));
        desc->VendorId = desc_.VendorId;
        desc->DeviceId = desc_.DeviceId;
        desc->SubSysId = desc_.SubSysId;
        desc->Revision = desc_.Revision;
        desc->DedicatedVideoMemory = desc_.DedicatedVideoMemory;
        desc->DedicatedSystemMemory = desc_.DedicatedSystemMemory;
        desc->SharedSystemMemory = desc_.SharedSystemMemory;
        desc->AdapterLuid = desc_.AdapterLuid;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID interface_name, LARGE_INTEGER* umd_version) override {
        if (interface_name == __uuidof(IDXGIDevice)) {
            if (umd_version) *umd_version = umd_version_;
            return S_OK;
        }
        return DXGI_ERROR_UNSUPPORTED;
    }

    // IDXGIAdapter1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1* desc) override {
        if (!desc) return E_INVALIDARG;
        std::memcpy(desc->Description, desc_.Description, sizeof(desc->Description));
        desc->VendorId = desc_.VendorId;
        desc->DeviceId = desc_.DeviceId;
        desc->SubSysId = desc_.SubSysId;
        desc->Revision = desc_.Revision;
        desc->DedicatedVideoMemory = desc_.DedicatedVideoMemory;
        desc->DedicatedSystemMemory = desc_.DedicatedSystemMemory;
        desc->SharedSystemMemory = desc_.SharedSystemMemory;
        desc->AdapterLuid = desc_.AdapterLuid;
        desc->Flags = desc_.Flags;
        return S_OK;
    }

    // IDXGIAdapter2
    HRESULT STDMETHODCALLTYPE GetDesc2(DXGI_ADAPTER_DESC2* desc) override {
        if (!desc) return E_INVALIDARG;
        *desc = desc_;
        return S_OK;
    }

    // IDXGIAdapter3
    HRESULT STDMETHODCALLTYPE RegisterHardwareContentProtectionTeardownStatusEvent(HANDLE, DWORD* cookie) override {
        if (cookie) *cookie = 0;
        return E_NOTIMPL;
    }
    void STDMETHODCALLTYPE UnregisterHardwareContentProtectionTeardownStatus(DWORD) override {}
    HRESULT STDMETHODCALLTYPE QueryVideoMemoryInfo(UINT node_index, DXGI_MEMORY_SEGMENT_GROUP group,
                                                   DXGI_QUERY_VIDEO_MEMORY_INFO* info) override {
        if (node_index != 0 || !info) return E_INVALIDARG;
        if (group != DXGI_MEMORY_SEGMENT_GROUP_LOCAL && group != DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL)
            return E_INVALIDARG;
        std::memset(info, 0, sizeof(*info));
        const bool want_local = (group == DXGI_MEMORY_SEGMENT_GROUP_LOCAL);
        if (gpu_) {
            const leonida::gpu::MemoryBudget budget = gpu_->memory_budget();
            if (want_local) {
                info->Budget = budget.recommended_working_set;
                info->CurrentUsage = budget.device_allocated;
            }
        }
        if (info->Budget == 0) {
            info->Budget = want_local ? desc_.DedicatedVideoMemory : desc_.SharedSystemMemory;
        }
        info->AvailableForReservation = info->Budget / 2;
        info->CurrentReservation = want_local ? reservations_->local.load(std::memory_order_relaxed)
                                              : reservations_->non_local.load(std::memory_order_relaxed);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVideoMemoryReservation(UINT node_index, DXGI_MEMORY_SEGMENT_GROUP group,
                                                        UINT64 reservation) override {
        if (node_index != 0) return E_INVALIDARG;
        if (group == DXGI_MEMORY_SEGMENT_GROUP_LOCAL) {
            reservations_->local.store(reservation, std::memory_order_relaxed);
            return S_OK;
        }
        if (group == DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL) {
            reservations_->non_local.store(reservation, std::memory_order_relaxed);
            return S_OK;
        }
        return E_INVALIDARG;
    }
    // Microsoft documents only "an HRESULT success or error code" and a cookie for
    // UnregisterVideoMemoryBudgetChangeNotification. The budget here is recomputed on every QueryVideoMemoryInfo and
    // never changes by itself, so the event is registered and never signalled.
    HRESULT STDMETHODCALLTYPE RegisterVideoMemoryBudgetChangeNotificationEvent(HANDLE event, DWORD* cookie) override {
        if (!event || !cookie) return E_INVALIDARG;
        *cookie = next_budget_cookie_.fetch_add(1, std::memory_order_relaxed);
        return S_OK;
    }
    void STDMETHODCALLTYPE UnregisterVideoMemoryBudgetChangeNotification(DWORD) override {}

    // IDXGIAdapter4 (host DXGI answers it since Windows 10 1803). DXGI_ADAPTER_FLAG3_SOFTWARE is the same bit as
    // DXGI_ADAPTER_FLAG_SOFTWARE; a null description is E_INVALIDARG, as on the host.
    HRESULT STDMETHODCALLTYPE GetDesc3(DXGI_ADAPTER_DESC3* desc) override {
        if (!desc) return E_INVALIDARG;
        std::memset(desc, 0, sizeof(*desc));
        std::memcpy(desc->Description, desc_.Description, sizeof(desc->Description));
        desc->VendorId = desc_.VendorId;
        desc->DeviceId = desc_.DeviceId;
        desc->SubSysId = desc_.SubSysId;
        desc->Revision = desc_.Revision;
        desc->DedicatedVideoMemory = desc_.DedicatedVideoMemory;
        desc->DedicatedSystemMemory = desc_.DedicatedSystemMemory;
        desc->SharedSystemMemory = desc_.SharedSystemMemory;
        desc->AdapterLuid = desc_.AdapterLuid;
        desc->Flags = static_cast<DXGI_ADAPTER_FLAG3>(desc_.Flags);
        desc->GraphicsPreemptionGranularity = desc_.GraphicsPreemptionGranularity;
        desc->ComputePreemptionGranularity = desc_.ComputePreemptionGranularity;
        return S_OK;
    }

private:
    void populate_desc() {
        std::memset(&desc_, 0, sizeof(desc_));
        desc_.GraphicsPreemptionGranularity = DXGI_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;
        desc_.ComputePreemptionGranularity = DXGI_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY;
        if (!gpu_) return;
        // Apple GPU (unified memory): the recommended working set is the "dedicated" budget, as a UMA adapter reports.
        const leonida::gpu::DeviceCaps& caps = gpu_->caps();
        MultiByteToWideChar(CP_UTF8, 0, caps.name.c_str(), -1, desc_.Description,
                            static_cast<int>(std::size(desc_.Description) - 1));
        desc_.Description[std::size(desc_.Description) - 1] = L'\0';
        desc_.VendorId = 0x106B;
        desc_.DeviceId = 0;
        desc_.AdapterLuid.LowPart = 0x6d746c31;
        desc_.DedicatedVideoMemory = static_cast<SIZE_T>(caps.recommended_working_set);
        desc_.SharedSystemMemory = static_cast<SIZE_T>(gpu_->memory_budget().physical_memory);
    }

    std::atomic<DWORD> next_budget_cookie_{1};
    std::atomic<ULONG> refs_{1};
    PrivateDataStore private_data_;
    IDXGIFactory4* parent_ = nullptr;
    leonida::gpu::Device* gpu_ = nullptr;
    DXGI_ADAPTER_DESC2 desc_{};
    LARGE_INTEGER umd_version_{};  // CheckInterfaceSupport(IDXGIDevice): the adapter's user-mode driver version
    std::shared_ptr<ReservationState> reservations_;
};

class Factory final : public IDXGIFactory7 {
public:
    explicit Factory(leonida::MetalContext& context) : context_(context) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (!left) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIFactory) ||
            riid == __uuidof(IDXGIFactory1) || riid == __uuidof(IDXGIFactory2) ||
            riid == __uuidof(IDXGIFactory3) || riid == __uuidof(IDXGIFactory4) ||
            riid == __uuidof(IDXGIFactory5) || riid == __uuidof(IDXGIFactory6) ||
            riid == __uuidof(IDXGIFactory7)) {
            *out = static_cast<IDXGIFactory7*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override {
        return private_data_.set(guid, size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* object) override {
        return private_data_.set_interface(guid, object);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override {
        return private_data_.get(guid, size, data);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        return E_NOINTERFACE;
    }

    // IDXGIFactory
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT index, IDXGIAdapter** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (index != 0) return DXGI_ERROR_NOT_FOUND;
        auto* adapter = new Adapter(this, &context_.gpu());
        *out = static_cast<IDXGIAdapter*>(adapter);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND hwnd, UINT flags) override {
        // Host-measured: only the three documented bits are accepted; any other bit is DXGI_ERROR_INVALID_CALL.
        constexpr UINT known = DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_PRINT_SCREEN;
        if (flags & ~known) return DXGI_ERROR_INVALID_CALL;
        assoc_hwnd_ = hwnd;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* hwnd) override {
        if (!hwnd) return E_INVALIDARG;
        *hwnd = assoc_hwnd_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) override {
        if (!out || !device || !desc) return DXGI_ERROR_INVALID_CALL;
        *out = nullptr;
        DXGI_SWAP_CHAIN_DESC1 desc1{};
        desc1.Width = desc->BufferDesc.Width;
        desc1.Height = desc->BufferDesc.Height;
        desc1.Format = desc->BufferDesc.Format;
        desc1.SampleDesc = desc->SampleDesc;
        desc1.BufferUsage = desc->BufferUsage;
        desc1.BufferCount = desc->BufferCount;
        desc1.Scaling = DXGI_SCALING_STRETCH;
        desc1.SwapEffect = desc->SwapEffect;
        desc1.Flags = desc->Flags;
        DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs_desc{};
        fs_desc.Windowed = desc->Windowed;
        fs_desc.RefreshRate = desc->BufferDesc.RefreshRate;
        fs_desc.Scaling = desc->BufferDesc.Scaling;
        fs_desc.ScanlineOrdering = desc->BufferDesc.ScanlineOrdering;
        IDXGISwapChain1* sc1 = nullptr;
        const HRESULT hr = CreateSwapChainForHwnd(device, desc->OutputWindow, &desc1, &fs_desc, nullptr, &sc1);
        if (SUCCEEDED(hr) && sc1) {
            *out = static_cast<IDXGISwapChain*>(sc1);
            if (!desc->Windowed) {
                // When exclusive fullscreen is unavailable, native DXGI creates the
                // windowed chain and returns DXGI_STATUS_OCCLUDED.
                return DXGI_STATUS_OCCLUDED;
            }
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE, IDXGIAdapter**) override { return E_NOTIMPL; }

    // IDXGIFactory1
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT index, IDXGIAdapter1** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (index != 0) return DXGI_ERROR_NOT_FOUND;
        auto* adapter = new Adapter(this, &context_.gpu());
        *out = static_cast<IDXGIAdapter1*>(adapter);
        return S_OK;
    }
    BOOL STDMETHODCALLTYPE IsCurrent() override { return TRUE; }

    // IDXGIFactory2
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override { return FALSE; }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput* restrict_to_output,
                                                     IDXGISwapChain1** out) override {
        if (out) *out = nullptr;
        if (const HRESULT hr = check_hwnd_swap_chain(device, hwnd, desc, out); FAILED(hr)) return hr;
        return create_swap_chain(device, hwnd, desc, restrict_to_output, out);
    }

    // What the host runtime's DXGI refuses for a D3D12 swap chain on a window (dxgi_swapchain_test compares these,
    // RTX 5070 / 621.14): a null queue, window, description or output, an ID3D12Device in place of the queue, the
    // bitblt effects, a scaling or alpha mode a window can't present, a flag outside the ones a D3D12 window swap chain
    // takes (MODE_SWITCH, DISPLAY_ONLY, FRAME_LATENCY_WAITABLE_OBJECT, ALLOW_TEARING, the holographic restriction and
    // 0x2000), a shared, read-only, discard-on-present or unordered-access buffer usage, MSAA, a buffer count outside
    // 2..16 and a format the swap chain can't take are DXGI_ERROR_INVALID_CALL; a side above 16384 is E_INVALIDARG,
    // only when nothing else is wrong.
    static HRESULT check_hwnd_swap_chain(IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                         IDXGISwapChain1** out) {
        if (!device || !hwnd || !desc || !out) return DXGI_ERROR_INVALID_CALL;
        ID3D12CommandQueue* queue = nullptr;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))) || !queue) return DXGI_ERROR_INVALID_CALL;
        queue->Release();
        if (desc->SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL && desc->SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD)
            return DXGI_ERROR_INVALID_CALL;
        if (desc->Scaling != DXGI_SCALING_STRETCH && desc->Scaling != DXGI_SCALING_NONE) return DXGI_ERROR_INVALID_CALL;
        if (desc->AlphaMode != DXGI_ALPHA_MODE_UNSPECIFIED && desc->AlphaMode != DXGI_ALPHA_MODE_IGNORE)
            return DXGI_ERROR_INVALID_CALL;
        constexpr UINT kWindowFlags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH | DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY |
                                      DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                                      DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | 0x1000u | 0x2000u;
        if (desc->Flags & ~kWindowFlags) return DXGI_ERROR_INVALID_CALL;
        constexpr DXGI_USAGE kRefusedUsage =
            DXGI_USAGE_SHARED | DXGI_USAGE_READ_ONLY | DXGI_USAGE_DISCARD_ON_PRESENT | DXGI_USAGE_UNORDERED_ACCESS;
        if (desc->BufferUsage & kRefusedUsage) return DXGI_ERROR_INVALID_CALL;
        // The host checks these before the size: MSAA 4 or one buffer at width 16385 is still INVALID_CALL.
        if (desc->SampleDesc.Count != 1 || desc->SampleDesc.Quality != 0) return DXGI_ERROR_INVALID_CALL;
        if (!valid_flip_buffer_count(desc->BufferCount)) return DXGI_ERROR_INVALID_CALL;
        if (desc->Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc->Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
            desc->Format != DXGI_FORMAT_R10G10B10A2_UNORM && desc->Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
            return DXGI_ERROR_INVALID_CALL;
        if (desc->Width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || desc->Height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
            return E_INVALIDARG;
        return S_OK;
    }

    // The swap chain itself, for a window or (through its HWND) a CoreWindow. The CoreWindow path keeps only these
    // checks: the host's rules for it (ASPECT_RATIO_STRETCH, for one) can't be compared without a real CoreWindow.
    HRESULT create_swap_chain(IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                              IDXGIOutput* restrict_to_output, IDXGISwapChain1** out) {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!hwnd || !desc) return E_INVALIDARG;
        if (desc->SampleDesc.Count != 1 || desc->SampleDesc.Quality != 0) return DXGI_ERROR_INVALID_CALL;
        if (!valid_flip_buffer_count(desc->BufferCount)) return DXGI_ERROR_INVALID_CALL;
        (void)device;
        (void)restrict_to_output;
        // port/d3d12_metal: DXGI swap chains are not on the Metal path (the title presents through PresentX).
        std::fprintf(stderr, "[rt] dxgi: CreateSwapChain on the Metal backend is not implemented (E_NOTIMPL)\n");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* device, IUnknown* window,
                                                           const DXGI_SWAP_CHAIN_DESC1* desc,
                                                           IDXGIOutput* restrict_to_output,
                                                           IDXGISwapChain1** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!device || !window || !desc) return E_INVALIDARG;
        // UWP / Game Core Scarlett titles present to an ICoreWindow. Extract the underlying
        // Win32 HWND via ICoreWindowInterop.
        static constexpr GUID kIID_ICoreWindowInterop = {
            0x45D64A29, 0xA63E, 0x4CB6, {0xB4, 0x98, 0x57, 0x81, 0xD2, 0x98, 0xCB, 0x4F}};
        struct ICoreWindowInterop : public IUnknown {
            virtual HRESULT STDMETHODCALLTYPE get_WindowHandle(HWND* hwnd) = 0;
            virtual HRESULT STDMETHODCALLTYPE put_MessageHandled(unsigned char value) = 0;
        };
        ICoreWindowInterop* interop = nullptr;
        if (FAILED(window->QueryInterface(kIID_ICoreWindowInterop, reinterpret_cast<void**>(&interop))) || !interop) {
            return DXGI_ERROR_INVALID_CALL;
        }
        HWND hwnd = nullptr;
        const HRESULT hr = interop->get_WindowHandle(&hwnd);
        interop->Release();
        if (FAILED(hr) || !hwnd) return DXGI_ERROR_INVALID_CALL;
        return create_swap_chain(device, hwnd, desc, restrict_to_output, out);
    }
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE, LUID*) override { return E_NOTIMPL; }
    // Microsoft documents S_OK with a cookie for UnregisterStereoStatus (E_NOTIMPL is only the answer of the
    // Windows 7 Platform Update). Stereo is never enabled here, so the window or event is never notified.
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND hwnd, UINT msg, DWORD* cookie) override {
        (void)msg;
        if (!hwnd || !cookie) return DXGI_ERROR_INVALID_CALL;
        *cookie = next_stereo_cookie_.fetch_add(1, std::memory_order_relaxed);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE event, DWORD* cookie) override {
        if (!event || !cookie) return DXGI_ERROR_INVALID_CALL;
        *cookie = next_stereo_cookie_.fetch_add(1, std::memory_order_relaxed);
        return S_OK;
    }
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD) override {}
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND hwnd, UINT msg, DWORD* cookie) override {
        (void)msg;
        if (!hwnd || !cookie) return DXGI_ERROR_INVALID_CALL;
        *cookie = next_occlusion_cookie_.fetch_add(1, std::memory_order_relaxed);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE event, DWORD* cookie) override {
        if (!event || !cookie) return DXGI_ERROR_INVALID_CALL;
        *cookie = next_occlusion_cookie_.fetch_add(1, std::memory_order_relaxed);
        return S_OK;
    }
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD) override {}
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*,
                                                            IDXGISwapChain1**) override {
        return E_NOTIMPL;
    }

    // IDXGIFactory3
    UINT STDMETHODCALLTYPE GetCreationFlags() override { return 0; }

    // IDXGIFactory4
    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID luid, REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        auto* adapter = new Adapter(this, &context_.gpu());
        const LUID actual = adapter->luid();
        if (actual.LowPart != luid.LowPart || actual.HighPart != luid.HighPart) {
            adapter->Release();
            return DXGI_ERROR_NOT_FOUND;
        }
        const HRESULT hr = adapter->QueryInterface(riid, out);
        adapter->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID, void**) override { return DXGI_ERROR_NOT_FOUND; }

    // IDXGIFactory5
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE feature, void* data, UINT size) override {
        if (!data) return E_INVALIDARG;
        if (feature == DXGI_FEATURE_PRESENT_ALLOW_TEARING) {
            if (size != sizeof(BOOL)) return E_INVALIDARG;
            std::call_once(tearing_once_, [this] { tearing_supported_ = probe_immediate_present_mode(); });
            *static_cast<BOOL*>(data) = tearing_supported_ ? TRUE : FALSE;
            return S_OK;
        }
        return E_INVALIDARG;
    }

    // IDXGIFactory6. The factory has one adapter, so every preference orders the same list. Measured on the host's
    // dxgi.dll (Windows 11, RTX 3070): a preference outside the enum is accepted, a null `out` is
    // DXGI_ERROR_INVALID_CALL, and an index past the end is DXGI_ERROR_NOT_FOUND with *out cleared.
    HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT index, DXGI_GPU_PREFERENCE, REFIID riid,
                                                         void** out) override {
        if (!out) return DXGI_ERROR_INVALID_CALL;
        *out = nullptr;
        if (index != 0) return DXGI_ERROR_NOT_FOUND;
        auto* adapter = new Adapter(this, &context_.gpu());
        const HRESULT hr = adapter->QueryInterface(riid, out);
        adapter->Release();
        return hr;
    }

    // IDXGIFactory7. The adapter list never changes, so a registered event is never signalled; the registry only
    // keeps the host's answers (same measurement): a null event or cookie and an event registered twice are
    // DXGI_ERROR_INVALID_CALL, cookies start at 1 and are separate from the occlusion cookies, and an unknown
    // cookie is DXGI_ERROR_NOT_FOUND.
    HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(HANDLE event, DWORD* cookie) override {
        if (!event || !cookie) return DXGI_ERROR_INVALID_CALL;
        std::lock_guard lock(adapters_changed_mutex_);
        for (const auto& [registered_cookie, registered_event] : adapters_changed_events_)
            if (registered_event == event) return DXGI_ERROR_INVALID_CALL;
        const DWORD next = next_adapters_changed_cookie_++;
        adapters_changed_events_.emplace(next, event);
        *cookie = next;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(DWORD cookie) override {
        std::lock_guard lock(adapters_changed_mutex_);
        return adapters_changed_events_.erase(cookie) ? S_OK : DXGI_ERROR_NOT_FOUND;
    }

private:
    bool probe_immediate_present_mode() const { return false; }

    std::atomic<ULONG> refs_{1};
    PrivateDataStore private_data_;
    leonida::MetalContext& context_;
    HWND assoc_hwnd_ = nullptr;
    mutable std::once_flag tearing_once_;
    bool tearing_supported_ = false;
    std::atomic<DWORD> next_occlusion_cookie_{1};
    std::mutex adapters_changed_mutex_;
    std::map<DWORD, HANDLE> adapters_changed_events_;
    DWORD next_adapters_changed_cookie_ = 1;
    std::atomic<DWORD> next_stereo_cookie_{1};
};

}  // namespace

HRESULT create_factory(leonida::MetalContext& context, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    auto* factory = new Factory(context);
    const HRESULT hr = factory->QueryInterface(riid, out);
    factory->Release();
    return hr;
}

}  // namespace rt::dxgi
