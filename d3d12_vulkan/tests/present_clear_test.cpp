// Host-only S1.6 check, no guest code: D3D12XboxCreateDevice from this module's d3d12_xs.dll export table (leonida's
// own path: create_vulkan_device_standalone behind the Xbox-ABI proxy, driver dlopen'd by platform/vk_driver), a
// visible CAMetalLayer window, then frames that clear a render target and present it through the window presenter
// (VK_EXT_metal_surface swapchain). The presenter's capture of the last presented image must be the clear colour.
// Exit 0 pass, 1 fail, 77 skip (no driver).
#include "d3d12_com.h"
#include "d3d12_xs_device.h"
#include "mac_window.h"
#include "package_splash.h"
#include "register.h"
#include "vk_driver.h"

#include <dxgi1_6.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define EXPECT(condition)                                                    \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
            return 1;                                                        \
        }                                                                    \
    } while (0)

namespace {

using XboxCreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, const void*, const IID*, void**);
using XboxHeapHandleFn = std::uint64_t(WINAPI*)(void* heap);

template <class T>
struct Ref {
    T* p = nullptr;
    ~Ref() { reset(); }
    void reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T* operator->() const { return p; }
};

bool is_colour(const std::vector<std::uint8_t>& pixels, VkFormat format, int r0, int g0, int b0, std::size_t* bad) {
    const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
    for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
        const int r = pixels[i + (bgra ? 2 : 0)], g = pixels[i + 1], b = pixels[i + (bgra ? 0 : 2)];
        if (std::abs(r - r0) > 1 || std::abs(g - g0) > 1 || std::abs(b - b0) > 1) {
            *bad = i / 4;
            std::fprintf(stderr, "texel %zu is %d,%d,%d, expected %d,%d,%d\n", *bad, r, g, b, r0, g0, b0);
            return false;
        }
    }
    return !pixels.empty();
}

// The path a DXGI title takes: the device's IDXGIDevice1 -> adapter -> factory -> CreateSwapChainForHwnd on the
// queue, back buffers from GetBuffer, Present. Swapchain images cannot be read back through DXGI, so this checks
// that every call succeeds and the output reports a display.
int dxgi_phase(ID3D12Device* device, gta6rt_mac_window* window, int frames) {
    Ref<IDXGIDevice1> dxgi_device;
    EXPECT(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device.p))));
    Ref<IDXGIAdapter> adapter;
    EXPECT(SUCCEEDED(dxgi_device->GetAdapter(&adapter.p)));
    DXGI_ADAPTER_DESC adapter_desc{};
    EXPECT(SUCCEEDED(adapter->GetDesc(&adapter_desc)));
    char name[128] = {};
    for (int i = 0; i < 127 && adapter_desc.Description[i]; ++i) name[i] = char(adapter_desc.Description[i]);
    std::printf("dxgi adapter: %s\n", name);
    Ref<IDXGIOutput> output;
    EXPECT(SUCCEEDED(adapter->EnumOutputs(0, &output.p)));
    DXGI_OUTPUT_DESC output_desc{};
    EXPECT(SUCCEEDED(output->GetDesc(&output_desc)));
    std::printf("dxgi output 0: %ldx%ld\n", long(output_desc.DesktopCoordinates.right - output_desc.DesktopCoordinates.left),
                long(output_desc.DesktopCoordinates.bottom - output_desc.DesktopCoordinates.top));
    Ref<IDXGIFactory2> factory;
    EXPECT(SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory.p))));

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    Ref<ID3D12Fence> fence;
    Ref<ID3D12DescriptorHeap> rtv_heap;
    EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
    EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                               IID_PPV_ARGS(&list.p))));
    EXPECT(SUCCEEDED(list->Close()));
    EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    Ref<IDXGISwapChain1> chain1;
    const HRESULT made = factory->CreateSwapChainForHwnd(queue.p, reinterpret_cast<HWND>(window), &desc, nullptr,
                                                         nullptr, &chain1.p);
    std::printf("CreateSwapChainForHwnd: hr=0x%08x\n", static_cast<unsigned>(made));
    EXPECT(SUCCEEDED(made));
    Ref<IDXGISwapChain3> chain;
    EXPECT(SUCCEEDED(chain1->QueryInterface(IID_PPV_ARGS(&chain.p))));
    DXGI_SWAP_CHAIN_DESC1 got{};
    EXPECT(SUCCEEDED(chain->GetDesc1(&got)));
    std::printf("dxgi swapchain: %ux%u, %u buffers\n", got.Width, got.Height, got.BufferCount);
    EXPECT(got.Width && got.Height);

    const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap.p))));
    const SIZE_T base =
        SIZE_T(reinterpret_cast<XboxHeapHandleFn>((*reinterpret_cast<void***>(rtv_heap.p))[9])(rtv_heap.p));
    const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    Ref<ID3D12Resource> buffers[2];
    for (UINT i = 0; i < 2; ++i) {
        EXPECT(SUCCEEDED(chain->GetBuffer(i, IID_PPV_ARGS(&buffers[i].p))));
        device->CreateRenderTargetView(buffers[i].p, nullptr, D3D12_CPU_DESCRIPTOR_HANDLE{base + i * step});
    }
    ID3D12CommandList* lists[] = {list.p};
    UINT64 signalled = 0;
    const FLOAT colour[4] = {0.1f, 0.8f, 0.3f, 1.0f};
    for (int f = 0; f < frames; ++f) {
        gta6rt_mac_window_pump_events(0);
        EXPECT(SUCCEEDED(fence->SetEventOnCompletion(signalled, nullptr)));
        EXPECT(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.p, nullptr)));
        const UINT index = chain->GetCurrentBackBufferIndex();
        EXPECT(index < 2);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = buffers[index].p;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &barrier);
        list->ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE{base + index * step}, colour, 0, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        list->ResourceBarrier(1, &barrier);
        EXPECT(SUCCEEDED(list->Close()));
        queue->ExecuteCommandLists(1, lists);
        EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
        const HRESULT presented = chain->Present(1, 0);
        if (FAILED(presented)) std::fprintf(stderr, "Present frame %d: hr=0x%08x\n", f, static_cast<unsigned>(presented));
        EXPECT(SUCCEEDED(presented));
        EXPECT(SUCCEEDED(queue->Signal(fence.p, ++signalled)));
    }
    EXPECT(SUCCEEDED(fence->SetEventOnCompletion(signalled, nullptr)));
    std::printf("dxgi: %d frames presented\n", frames);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    int frames = 120;
    if (argc > 1) frames = std::max(2, std::atoi(argv[1]));
    const char* package = argc > 2 ? argv[2] : nullptr;
    setenv("LEONIDA_D3D12_LISTS", "pc", 1);

    const rthost::VulkanDriver* driver = rthost::vulkan_driver();
    if (!driver) {
        std::fprintf(stderr, "SKIP: no Vulkan driver\n");
        return 77;
    }
    std::printf("driver: %s %s\n", driver->name(), driver->path.c_str());

    constexpr std::uint32_t width = 640, height = 360;
    gta6rt_mac_window* window = gta6rt_mac_window_create("gta6rt S1.6: D3D12 clear on Vulkan", width, height, 1);
    EXPECT(window);
    gta6rt_mac_window_pump_events(0.2);
    std::uint32_t pw = 0, ph = 0;
    EXPECT(gta6rt_mac_window_client_pixels(window, &pw, &ph));
    std::printf("window: %ux%u pixels, visible=%d\n", pw, ph, gta6rt_mac_window_visible(window));

    const auto create = reinterpret_cast<XboxCreateDeviceFn>(
        rt::d3d12_vulkan::find_export("d3d12_xs.dll", "D3D12XboxCreateDevice"));
    EXPECT(create);
    Ref<ID3D12Device> device;
    std::uint32_t params[24] = {};
    const HRESULT created = create(nullptr, params, &__uuidof(ID3D12Device), reinterpret_cast<void**>(&device.p));
    std::printf("D3D12XboxCreateDevice: hr=0x%08x device=%p\n", static_cast<unsigned>(created),
                static_cast<void*>(device.p));
    EXPECT(SUCCEEDED(created) && device.p);
    const rt::d3d12::VulkanDeviceInfo* info = rt::d3d12::vulkan_device_info(device.p);
    if (!info) info = rt::d3d12::vulkan_device_info(rt::d3d12_xs::device_proxy_real_device(device.p));
    EXPECT(info && info->presentation);

    auto presenter = rt::d3d12::create_window_presenter(device.p, reinterpret_cast<HWND>(window));
    EXPECT(presenter);
    EXPECT(presenter->enable_capture());

    if (package) {
        rt::package_splash::configure(package);
        EXPECT(rt::package_splash::configured());
        ID3D12Device* real = rt::d3d12_xs::device_proxy_real_device(device.p);
        Ref<ID3D12Resource> splash;
        splash.p = rt::package_splash::create_texture(real ? real : device.p, width, height);
        EXPECT(splash.p);
        EXPECT(presenter->present_splash(splash.p));
        std::vector<std::uint8_t> pixels;
        VkExtent2D size{};
        VkFormat format = VK_FORMAT_UNDEFINED;
        EXPECT(presenter->read_back(pixels, &size, &format));
        std::size_t lit = 0;
        for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) lit += (pixels[i] | pixels[i + 1] | pixels[i + 2]) != 0;
        std::printf("splash: %ux%u presented, %zu of %zu pixels not black\n", size.width, size.height, lit,
                    pixels.size() / 4);
        EXPECT(lit > 0);
    }

    {
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        Ref<ID3D12CommandQueue> queue;
        Ref<ID3D12CommandAllocator> allocator;
        Ref<ID3D12GraphicsCommandList> list;
        Ref<ID3D12Fence> fence;
        Ref<ID3D12DescriptorHeap> rtv_heap;
        Ref<ID3D12Resource> back_buffer;
        EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
        EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
        EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                                   IID_PPV_ARGS(&list.p))));
        EXPECT(SUCCEEDED(list->Close()));
        EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));
        const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap.p))));
        // The proxy's heap answers slot 9 in the console form: the 8-byte handle in RAX.
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv{
            SIZE_T(reinterpret_cast<XboxHeapHandleFn>((*reinterpret_cast<void***>(rtv_heap.p))[9])(rtv_heap.p))};
        const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_PRESENT, nullptr,
                                                         IID_PPV_ARGS(&back_buffer.p))));
        device->CreateRenderTargetView(back_buffer.p, nullptr, rtv);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = back_buffer.p;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ID3D12CommandList* lists[] = {list.p};
        UINT64 signalled = 0;
        const FLOAT colour[4] = {0.2f, 0.6f, 0.9f, 1.0f};
        const FLOAT red[4] = {1.f, 0.f, 0.f, 1.f};

        const auto frame = [&](const FLOAT* clear) -> bool {
            gta6rt_mac_window_pump_events(0);
            if (FAILED(fence->SetEventOnCompletion(signalled, nullptr))) return false;
            if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.p, nullptr))) return false;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            list->ResourceBarrier(1, &barrier);
            list->ClearRenderTargetView(rtv, clear, 0, nullptr);
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            list->ResourceBarrier(1, &barrier);
            if (FAILED(list->Close())) return false;
            queue->ExecuteCommandLists(1, lists);
            if (rt::d3d12::last_execute_result(queue.p) != leonida::ReplayResult::success) return false;
            if (FAILED(presenter->present(back_buffer.p, 1))) return false;
            return SUCCEEDED(queue->Signal(fence.p, ++signalled));
        };

        std::vector<std::uint8_t> pixels;
        VkExtent2D size{};
        VkFormat format = VK_FORMAT_UNDEFINED;
        std::size_t bad = 0;

        EXPECT(frame(red));
        EXPECT(presenter->read_back(pixels, &size, &format));
        EXPECT(is_colour(pixels, format, 255, 0, 0, &bad));

        for (int i = 1; i < frames; ++i) EXPECT(frame(colour));
        EXPECT(SUCCEEDED(fence->SetEventOnCompletion(signalled, nullptr)));
        const VkExtent2D extent = presenter->extent();
        std::printf("presented: %llu frames, swapchain %ux%u\n",
                    static_cast<unsigned long long>(presenter->presented()), extent.width, extent.height);
        EXPECT(presenter->presented() >= std::uint64_t(frames) - 2);
        EXPECT(presenter->read_back(pixels, &size, &format));
        std::printf("read back: %ux%u format %d, %zu bytes\n", size.width, size.height, static_cast<int>(format),
                    pixels.size());
        EXPECT(size.width == extent.width && size.height == extent.height);
        EXPECT(is_colour(pixels, format, 51, 153, 230, &bad));
    }
    presenter.reset();
    std::printf("presenter phase: PASS\n");

    gta6rt_mac_window* dxgi_window = gta6rt_mac_window_create("gta6rt S1.6: DXGI swapchain", width, height, 1);
    EXPECT(dxgi_window);
    gta6rt_mac_window_pump_events(0.1);
    if (dxgi_phase(device.p, dxgi_window, frames)) return 1;
    std::printf("dxgi phase: PASS\n");
    gta6rt_mac_window_destroy(dxgi_window);
    device.reset();
    gta6rt_mac_window_destroy(window);
    gta6rt_mac_window_pump_events(0.05);
    std::printf("present_clear_test: PASS\n");
    return 0;
}
