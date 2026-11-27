#include "test_util.h"

#include <mach/mach.h>
#include <sys/mman.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <numeric>

using namespace gta6rt::gpu;

namespace {

constexpr std::uint64_t kHeapBytes = 185ull << 20;          // the title's 32-bit graphics heap (fork assessment P1)
constexpr std::uint64_t kHeapVa = 0x34310000ull;            // where VIctory's xmem placed it
constexpr std::uint64_t kRingOffset = 0x34e10000ull - kHeapVa;  // the 174 MB constants ring inside it
constexpr std::uint64_t kRingBytes = 174ull << 20;
constexpr std::uint32_t kDraws = 600, kBlock = 28 * 1024;   // ~17 MB a frame in ~600 uploads (fork assessment)
constexpr std::uint32_t kFrames = 60;

double since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

struct Env {
    Device* dev = nullptr;
    Queue* q = nullptr;
    std::unique_ptr<ShaderLibrary> lib;
    std::unique_ptr<RootLayout> root;
    std::unique_ptr<ComputePipeline> read, bump, ocean;
};

void addr(std::uint32_t* u, std::uint64_t a) {
    u[0] = std::uint32_t(a);
    u[1] = std::uint32_t(a >> 32);
}

bool run(Env& e, CommandList& l) {
    if (l.close() != Status::ok) return false;
    CommandList* ls[] = {&l};
    return e.q->execute(ls) == Status::ok && e.q->wait_idle(20'000'000'000ull) == Status::ok;
}

void fill_frame(std::uint8_t* ring, std::uint32_t frame) {
    std::uint8_t* base = ring + std::uint64_t(frame % 6) * kDraws * kBlock;
    for (std::uint32_t d = 0; d < kDraws; ++d) {
        auto* w = reinterpret_cast<std::uint32_t*>(base + std::uint64_t(d) * kBlock);
        const std::uint32_t v = frame * 0x10000u + d;
        for (std::uint32_t i = 0; i < kBlock / 4; ++i) w[i] = v ^ i;
    }
}

int check_frame(const std::uint32_t* out, std::uint32_t frame) {
    int bad = 0;
    for (std::uint32_t d = 0; d < kDraws; ++d) {
        const std::uint32_t v = frame * 0x10000u + d;
        const std::uint32_t* a = out + d * 8;
        const std::uint32_t last = kBlock / 4 - 4;
        for (std::uint32_t k = 0; k < 4; ++k) bad += a[k] != (v ^ k) || a[4 + k] != (v ^ (last + k));
    }
    return bad;
}

void title_heap(Env& e) {
    const std::uint64_t page = e.dev->caps().host_page_size;
    void* want = reinterpret_cast<void*>(kHeapVa);
    void* title = mmap(want, kHeapBytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
    const bool low = title == want;
    if (!low) title = mmap(nullptr, kHeapBytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK(title != MAP_FAILED, "title heap mapping");
    if (title == MAP_FAILED) return;
    vm_address_t alias = 0;
    vm_prot_t cur = 0, maxp = 0;
    const kern_return_t kr = vm_remap(mach_task_self(), &alias, kHeapBytes, 0, VM_FLAGS_ANYWHERE, mach_task_self(),
                                      reinterpret_cast<vm_address_t>(title), FALSE, &cur, &maxp, VM_INHERIT_NONE);
    CHECK(kr == KERN_SUCCESS, "host alias of the title pages (vm_remap %d)", kr);
    std::printf("title heap: %llu MiB at %p (%s), host alias at 0x%llx, page %llu B\n",
                (unsigned long long)(kHeapBytes >> 20), title,
                low ? "below 4 GiB at VIctory's 0x34310000" : "below-4-GiB placement refused in this process, anywhere",
                (unsigned long long)alias, (unsigned long long)page);
    if (kr != KERN_SUCCESS) return;
    auto* tv = static_cast<std::uint8_t*>(title);
    auto* av = reinterpret_cast<std::uint8_t*>(alias);
    tv[123] = 0x5a;
    CHECK(av[123] == 0x5a, "the alias sees the title's writes");

    std::string err;
    HostBufferDesc hd{av, kHeapBytes, kHeapVa, "title graphics heap (alias)"};
    const auto t0 = std::chrono::steady_clock::now();
    auto window = e.dev->create_host_buffer(hd, &err);
    const double wrap = since(t0);
    CHECK(window != nullptr, "bytesNoCopy window: %s", err.c_str());
    if (!window) return;
    Buffer* found = nullptr;
    std::uint64_t off = 0;
    CHECK(e.dev->resolve_va(kHeapVa + kRingOffset + 64, &found, &off) && found == window.get() && off == kRingOffset + 64,
          "resolve_va maps the title VA into the window");
    HostBufferDesc misaligned{av + 4096 * (page > 4096 ? 1 : 0) + (page > 4096 ? 0 : 1), 65536, 0, "unaligned"};
    CHECK(e.dev->create_host_buffer(misaligned, &err) == nullptr, "unaligned window refused: %s", err.c_str());
    std::printf("window: %.3f ms to wrap 185 MiB, gpuAddress 0x%llx, console VA 0x%llx\n", wrap * 1e3,
                (unsigned long long)window->host_gpu_address(), (unsigned long long)window->gpu_va());

    std::uint8_t* ring = tv + kRingOffset;
    const std::uint64_t ring_gpu = window->host_gpu_address() + kRingOffset;
    BufferDesc od{kDraws * 32ull, MemoryKind::readback, usage::unordered_access, "fetched constants"};
    auto out = e.dev->create_buffer(od);
    BufferDesc sd{std::uint64_t(kDraws) * kBlock, MemoryKind::upload, usage::copy, "upload staging"};
    auto staging = e.dev->create_buffer(sd);
    BufferDesc ld{std::uint64_t(kDraws) * kBlock, MemoryKind::device_local, usage::copy | usage::constant, "device copy"};
    auto local = e.dev->create_buffer(ld);
    auto ts = e.dev->create_query_heap({QueryType::timestamp, 4, "copy time"});
    BufferDesc tsd{64, MemoryKind::readback, usage::copy, "ts"};
    auto tsb = e.dev->create_buffer(tsd);

    double inplace_frame = 0, copy_frame = 0, copy_cpu = 0, copy_gpu = 0, write_cpu = 0;
    int bad_inplace = 0, bad_copy = 0;
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        const auto w0 = std::chrono::steady_clock::now();
        fill_frame(ring, f);
        write_cpu += since(w0);
        const std::uint64_t slice = std::uint64_t(f % 6) * kDraws * kBlock;
        auto l = e.dev->create_command_list(QueueType::compute);
        l->set_root_layout(e.root.get());
        l->set_compute_pipeline(e.read.get());
        std::uint32_t u[8] = {};
        addr(u, ring_gpu + slice);
        u[2] = kBlock;
        addr(u + 3, out->host_gpu_address());
        l->set_user_data(0, u);
        l->dispatch(kDraws, 1, 1);
        const auto t1 = std::chrono::steady_clock::now();
        CHECK(run(e, *l), "in-place frame");
        inplace_frame += since(t1);
        bad_inplace += check_frame(static_cast<const std::uint32_t*>(out->cpu_pointer()), f);
    }
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        fill_frame(ring, f);
        const std::uint64_t slice = std::uint64_t(f % 6) * kDraws * kBlock;
        const auto c0 = std::chrono::steady_clock::now();
        std::memcpy(staging->cpu_pointer(), ring + slice, std::uint64_t(kDraws) * kBlock);
        copy_cpu += since(c0);
        auto l = e.dev->create_command_list(QueueType::compute);
        l->end_query(ts.get(), 0);
        for (std::uint32_t d = 0; d < kDraws; ++d)
            l->copy_buffer(local.get(), std::uint64_t(d) * kBlock, staging.get(), std::uint64_t(d) * kBlock, kBlock);
        l->end_query(ts.get(), 1);
        Barrier b;
        b.resource = local.get();
        b.before = 0x400;
        b.after = 0x1;
        l->barrier(std::span<const Barrier>(&b, 1));
        l->set_root_layout(e.root.get());
        l->set_compute_pipeline(e.read.get());
        std::uint32_t u[8] = {};
        addr(u, local->host_gpu_address());
        u[2] = kBlock;
        addr(u + 3, out->host_gpu_address());
        l->set_user_data(0, u);
        l->dispatch(kDraws, 1, 1);
        l->resolve_queries(ts.get(), 0, 2, tsb.get(), 0);
        const auto t1 = std::chrono::steady_clock::now();
        CHECK(run(e, *l), "copy frame");
        copy_frame += since(c0);
        (void)t1;
        const auto* t = static_cast<const std::uint64_t*>(tsb->cpu_pointer());
        copy_gpu += double(t[1] - t[0]) / e.q->timestamp_frequency();
        bad_copy += check_frame(static_cast<const std::uint32_t*>(out->cpu_pointer()), f);
    }
    const double mb = double(kDraws) * kBlock / (1 << 20);
    std::printf("constants ring (%llu MiB at +0x%llx), %u frames of %u draws x %u KiB (%.1f MiB a frame):\n",
                (unsigned long long)(kRingBytes >> 20), (unsigned long long)kRingOffset, kFrames, kDraws, kBlock / 1024, mb);
    std::printf("  title CPU writes into its own pages: %.2f ms a frame (both paths)\n", write_cpu * 1e3 / kFrames);
    std::printf("  in place (GPU reads the title pages through the window): %.3f ms a frame execute-to-idle, %d wrong words\n",
                inplace_frame * 1e3 / kFrames, bad_inplace);
    std::printf("  copy path (memcpy to an upload buffer + 600 GPU copies to device-local): memcpy %.2f ms, GPU copies "
                "%.3f ms (timestamps), frame %.3f ms, %d wrong words\n",
                copy_cpu * 1e3 / kFrames, copy_gpu * 1e3 / kFrames, copy_frame * 1e3 / kFrames, bad_copy);
    std::printf("  avoided by in place: %.1f MiB of copies, %.2f ms CPU and %.3f ms GPU a frame; %.1f MiB of upload + "
                "device copies not allocated\n",
                mb, copy_cpu * 1e3 / kFrames, copy_gpu * 1e3 / kFrames, 2 * mb);
    CHECK(bad_inplace == 0, "the GPU reads the constants where the title wrote them");
    CHECK(bad_copy == 0, "copy path reference");

    std::uint32_t* words = reinterpret_cast<std::uint32_t*>(tv + 0x1000);
    for (std::uint32_t i = 0; i < 1024; ++i) words[i] = i;
    auto l = e.dev->create_command_list(QueueType::compute);
    l->set_root_layout(e.root.get());
    l->set_compute_pipeline(e.bump.get());
    std::uint32_t u[4] = {};
    addr(u, window->host_gpu_address() + 0x1000);
    u[2] = 1024;
    l->set_user_data(0, u);
    l->dispatch(1024, 1, 1, dispatch_flag::thread_counts);
    Barrier b;
    b.type = BarrierType::uav;
    l->barrier(std::span<const Barrier>(&b, 1));
    l->dispatch(1024, 1, 1, dispatch_flag::thread_counts);
    CHECK(run(e, *l), "GPU writes into the window");
    int bad = 0;
    for (std::uint32_t i = 0; i < 1024; ++i) bad += words[i] != (i * 3 + 1) * 3 + 1;
    std::printf("GPU writes into host-owned pages: two dependent dispatches with a UAV barrier, %d of 1024 words wrong "
                "when the title reads them after the fence\n",
                bad);
    CHECK(bad == 0, "GPU writes land in the title pages, ordered by the barrier");
    const MemoryBudget mbud = e.dev->memory_budget();
    std::printf("ledger: host windows %llu B in %llu objects\n",
                (unsigned long long)mbud.bytes[std::size_t(MemoryCategory::host_window)],
                (unsigned long long)mbud.objects[std::size_t(MemoryCategory::host_window)]);
    CHECK(mbud.bytes[std::size_t(MemoryCategory::host_window)] == kHeapBytes, "window accounted as host_window");
    window.reset();
    munmap(reinterpret_cast<void*>(alias), kHeapBytes);
    munmap(title, kHeapBytes);
}

void ocean(Env& e) {
    struct Kind { std::uint32_t fmt, w, bpp; const char* name; bool uint; };
    const Kind kinds[4] = {{format::r32_float, 256, 4, "R32F 256x256", false},
                           {format::r32g32_float, 256, 8, "RG32F 256x256", false},
                           {format::r32g32_float, 64, 8, "RG32F 64x64", false},
                           {format::r16g16_uint, 128, 4, "RG16UI 128x128", true}};
    const std::uint64_t page = e.dev->caps().host_page_size;
    const std::uint32_t align = e.dev->caps().linear_texture_alignment;
    std::uint64_t total = 0;
    std::uint64_t offs[4];
    for (int i = 0; i < 4; ++i) {
        offs[i] = total;
        const std::uint64_t pitch = (std::uint64_t(kinds[i].w) * kinds[i].bpp + align - 1) / align * align;
        total += (pitch * kinds[i].w + 4095) & ~std::uint64_t(4095);
    }
    total = (total + page - 1) / page * page;
    void* mem = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    std::string err;
    auto window = e.dev->create_host_buffer({mem, total, 0, "ocean pages"}, &err);
    CHECK(window != nullptr, "ocean window: %s", err.c_str());
    if (!window) return;
    std::unique_ptr<Texture> lin[4], dev[4];
    for (int i = 0; i < 4; ++i) {
        TextureDesc td;
        td.format = kinds[i].fmt;
        td.width = td.height = kinds[i].w;
        td.usage = usage::shader_read | usage::unordered_access;
        const std::uint32_t pitch = (kinds[i].w * kinds[i].bpp + align - 1) / align * align;
        lin[i] = e.dev->create_buffer_texture(window.get(), offs[i], pitch, td, &err);
        CHECK(lin[i] != nullptr, "%s linear texture: %s", kinds[i].name, err.c_str());
        dev[i] = e.dev->create_texture(td);
        CHECK(dev[i] != nullptr, "%s texture", kinds[i].name);
    }
    if (test::failures) return;
    auto ftab = e.dev->create_descriptor_heap({DescriptorKind::texture, 4, "ocean float"});
    auto utab = e.dev->create_descriptor_heap({DescriptorKind::texture, 4, "ocean uint"});
    BufferDesc rbd{total, MemoryKind::readback, usage::copy, "ocean readback"};
    auto rb = e.dev->create_buffer(rbd);
    for (int path = 0; path < 2; ++path) {
        std::unique_ptr<Texture>* t = path == 0 ? lin : dev;
        for (int i = 0; i < 3; ++i) ftab->write_texture(i, t[i].get());
        utab->write_texture(0, t[3].get());
        double lat = 0;
        int bad = 0;
        const int frames = 30;
        for (int f = 1; f <= frames; ++f) {
            auto l = e.dev->create_command_list(QueueType::compute);
            l->set_root_layout(e.root.get());
            l->set_compute_pipeline(e.ocean.get());
            l->set_descriptor_table(4, ftab.get());
            l->set_descriptor_table(7, utab.get());
            std::uint32_t u[9] = {};
            u[8] = std::uint32_t(f);
            l->set_user_data(0, u);
            l->dispatch(256, 256, 1, dispatch_flag::thread_counts);
            if (path == 1) {
                Barrier b;
                b.type = BarrierType::uav;
                l->barrier(std::span<const Barrier>(&b, 1));
                for (int i = 0; i < 4; ++i) {
                    BufferTextureCopy c;
                    c.buffer = rb.get();
                    c.offset = offs[i];
                    c.texture = dev[i].get();
                    c.row_pitch = (kinds[i].w * kinds[i].bpp + align - 1) / align * align;
                    l->copy_texture_to_buffer(c);
                }
            }
            const auto t0 = std::chrono::steady_clock::now();
            CHECK(run(e, *l), "ocean frame");
            lat += since(t0);
            const auto* base = static_cast<const std::uint8_t*>(path == 0 ? mem : rb->cpu_pointer());
            for (int i = 0; i < 4; ++i) {
                const std::uint32_t pitch = (kinds[i].w * kinds[i].bpp + align - 1) / align * align;
                for (std::uint32_t y = 0; y < kinds[i].w; y += 7)
                    for (std::uint32_t x = 0; x < kinds[i].w; x += 5) {
                        const std::uint8_t* p = base + offs[i] + std::uint64_t(y) * pitch + std::uint64_t(x) * kinds[i].bpp;
                        if (i == 3) {
                            std::uint16_t v[2];
                            std::memcpy(v, p, 4);
                            bad += v[0] != std::uint16_t(x + y * 128) || v[1] != std::uint16_t(f);
                        } else {
                            float v[2] = {};
                            std::memcpy(v, p, kinds[i].bpp);
                            const float w0 = i == 0 ? float(x) + f * 1000.0f : i == 1 ? float(y) : float(x * y);
                            const float w1 = i == 1 ? float(f) : -float(f);
                            bad += v[0] != w0 || (i > 0 && v[1] != w1);
                        }
                    }
            }
        }
        std::printf("ocean read-back, %s: %d wrong texels in %d frames; execute-to-idle %.3f ms a frame; bytes copied "
                    "%llu a frame\n",
                    path == 0 ? "in place (linear textures over host pages, CPU reads them after the fence)"
                              : "copy (device textures + copy_texture_to_buffer)",
                    bad, frames, lat * 1e3 / frames, (unsigned long long)(path == 0 ? 0 : total));
        CHECK(bad == 0, "ocean texels reach the CPU");
    }
    for (auto& t : lin) t.reset();
    window.reset();
    munmap(mem, total);
}

}  // namespace

int main() {
    auto dev = gta6rt::metal::create_device();
    if (!dev) return 1;
    test::print_caps(dev->caps());
    std::printf("host page %llu B, linear texture alignment %u B\n", (unsigned long long)dev->caps().host_page_size,
                dev->caps().linear_texture_alignment);
    for (const char* v : {"MTL_DEBUG_LAYER", "MTL_SHADER_VALIDATION", "GTA6RT_METAL_NO_DEVICE_RESIDENCY"})
        if (const char* s = std::getenv(v)) std::printf("env %s=%s\n", v, s);
    auto queue = dev->create_queue(QueueType::compute, "hostmem");
    Env e;
    e.dev = dev.get();
    e.q = queue.get();
    std::string err;
    e.lib = dev->create_library_msl(test::read_file(test::shader_path("hostmem.metal")), MslOptions{}, &err);
    CHECK(e.lib != nullptr, "library: %s", err.c_str());
    if (!e.lib) return test::finish("metal_test_hostmem");
    RootLayoutDesc rd;
    rd.tables = {{4, DescriptorKind::texture}, {7, DescriptorKind::texture}};
    e.root = dev->create_root_layout(rd);
    auto make = [&](const char* name, std::array<std::uint32_t, 3> t) {
        ComputePipelineDesc d;
        d.cs = {e.lib.get(), name};
        d.threads = t;
        d.root = e.root.get();
        auto p = dev->create_compute_pipeline(d);
        CHECK(p->status() == Status::ok && p->reflection().mismatches.empty(), "%s: %s", name, p->error().c_str());
        return p;
    };
    e.read = make("cs_read_constants", {32, 1, 1});
    e.bump = make("cs_bump", {64, 1, 1});
    e.ocean = make("cs_ocean", {16, 16, 1});
    if (test::failures) return test::finish("metal_test_hostmem");
    title_heap(e);
    ocean(e);
    test::print_stats(dev->stats());
    return test::finish("metal_test_hostmem");
}
