// The title pool on its own (no title, no xmem): one Mach memory entry mapped once at a 16 KiB-aligned host base,
// title addresses as further views of it (mach_vm_remap, as xmem's gta6rt_vm_alias makes them), and Metal buffers
// made with bytesNoCopy over chunks of the host base only, never over a title view. Checks coherence both ways
// through a compute kernel, views remapped and made no-access while the GPU uses the chunk, a vm_read_overwrite
// copy into a view (Rosetta splits VM objects for those), a 4 KiB view, chunks wrapped late, and what residency of
// a chunk costs in footprint and wired memory with each way of naming it.
#include "test_util.h"

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

using namespace gta6rt::gpu;

namespace {

constexpr std::uint64_t kPool = 2ull << 30, kChunk = 256ull << 20, kSlot = 64 << 10, kTitle = 64ull << 20;

struct Env {
    Device* dev = nullptr;
    Queue* q = nullptr;
    std::unique_ptr<ShaderLibrary> lib;
    std::unique_ptr<RootLayout> root;
    std::unique_ptr<ComputePipeline> bump, gather, spin;
    std::uint8_t* pool = nullptr;
    std::uint8_t* title = nullptr;
    std::unique_ptr<Buffer> chunk[8];
};

double since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

struct Memory {
    double footprint_mib, wired_mib;
};

Memory memory_now() {
    task_vm_info_data_t vi{};
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vi), &n);
    vm_statistics64_data_t vs{};
    mach_msg_type_number_t c = HOST_VM_INFO64_COUNT;
    host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vs), &c);
    vm_size_t page = 0;
    host_page_size(mach_host_self(), &page);
    return {double(vi.phys_footprint) / 1048576.0, double(vs.wire_count) * double(page) / 1048576.0};
}

bool view(Env& e, std::uint64_t title_offset, std::uint64_t pool_offset, std::uint64_t bytes, int prot = PROT_READ | PROT_WRITE) {
    mach_vm_address_t at = reinterpret_cast<mach_vm_address_t>(e.title + title_offset);
    vm_prot_t cur = 0, max = 0;
    const kern_return_t kr = mach_vm_remap(mach_task_self(), &at, bytes, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                                           mach_task_self(), reinterpret_cast<mach_vm_address_t>(e.pool + pool_offset),
                                           FALSE, &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) {
        std::printf("mach_vm_remap title+0x%llx <- pool+0x%llx (%llu bytes): %d\n", (unsigned long long)title_offset,
                    (unsigned long long)pool_offset, (unsigned long long)bytes, kr);
        return false;
    }
    return mprotect(e.title + title_offset, bytes, prot) == 0;
}

void addr(std::uint32_t* u, std::uint64_t a) {
    u[0] = std::uint32_t(a);
    u[1] = std::uint32_t(a >> 32);
}

std::uint64_t gpu_of(Env& e, std::uint64_t pool_offset) {
    Buffer* b = e.chunk[pool_offset / kChunk].get();
    return b ? b->host_gpu_address() + pool_offset % kChunk : 0;
}

bool submit(Env& e, ComputePipeline* p, const std::uint32_t* u, std::uint32_t threads, bool wait = true) {
    auto l = e.dev->create_command_list(QueueType::compute);
    l->set_root_layout(e.root.get());
    l->set_compute_pipeline(p);
    l->set_user_data(0, std::span<const std::uint32_t>(u, 8));
    l->dispatch(threads, 1, 1, dispatch_flag::thread_counts);
    if (l->close() != Status::ok) return false;
    CommandList* ls[] = {l.get()};
    if (e.q->execute(ls) != Status::ok) return false;
    return !wait || e.q->wait_idle(30'000'000'000ull) == Status::ok;
}

bool bump(Env& e, std::uint64_t pool_offset, std::uint32_t words) {
    std::uint32_t u[8] = {};
    addr(u, gpu_of(e, pool_offset));
    u[2] = words;
    return submit(e, e.bump.get(), u, words);
}

std::unique_ptr<Buffer> wrap(Env& e, unsigned k, bool resident, const char* label) {
    std::string err;
    HostBufferDesc d;
    d.cpu = e.pool + std::uint64_t(k) * kChunk;
    d.size = kChunk;
    d.label = label;
    d.resident = resident;
    const auto t0 = std::chrono::steady_clock::now();
    auto b = e.dev->create_host_buffer(d, &err);
    std::printf("chunk %u wrapped (%s) in %.3f ms: %s, gpuAddress 0x%llx\n", k, resident ? "device residency" : "per encoder",
                since(t0) * 1e3, b ? "ok" : err.c_str(), b ? (unsigned long long)b->host_gpu_address() : 0ull);
    if (b && !resident) gta6rt::metal::set_encoder_resource(e.dev, b.get(), true);
    return b;
}

void coherence(Env& e) {
    CHECK(reinterpret_cast<std::uintptr_t>(e.pool) % 16384 == 0, "pool base %p is 16 KiB aligned", e.pool);
    CHECK(view(e, 0, 0x110000, 2 * kSlot), "view of two slots");
    CHECK(view(e, 8 * kSlot, 0x110000, kSlot), "second view of the first slot");
    auto* t = reinterpret_cast<std::uint32_t*>(e.title);
    auto* t2 = reinterpret_cast<std::uint32_t*>(e.title + 8 * kSlot);
    auto* p = reinterpret_cast<std::uint32_t*>(e.pool + 0x110000);
    for (std::uint32_t i = 0; i < 2 * kSlot / 4; ++i) t[i] = i;
    int bad = 0;
    for (std::uint32_t i = 0; i < 2 * kSlot / 4; ++i) bad += p[i] != i;
    for (std::uint32_t i = 0; i < kSlot / 4; ++i) bad += t2[i] != i;
    CHECK(bad == 0, "title writes are pool and alias bytes (%d wrong)", bad);
    CHECK(bump(e, 0x110000, 2 * kSlot / 4), "GPU bump through the pool buffer");
    bad = 0;
    for (std::uint32_t i = 0; i < 2 * kSlot / 4; ++i) bad += t[i] != i * 3 + 1;
    for (std::uint32_t i = 0; i < kSlot / 4; ++i) bad += t2[i] != i * 3 + 1;
    std::printf("coherence: GPU writes through the pool buffer seen in both title views: %d of %llu words wrong\n", bad,
                (unsigned long long)(3 * kSlot / 4));
    CHECK(bad == 0, "GPU writes reach the title views");

    CHECK(view(e, 16 * kSlot + 0x1000, 0x200000 + 0x1000, 5 * 4096), "a 20 KiB view at a 4 KiB (not 16 KiB) offset");
    auto* small = reinterpret_cast<std::uint32_t*>(e.title + 16 * kSlot + 0x1000);
    for (std::uint32_t i = 0; i < 5 * 1024; ++i) small[i] = 7 * i;
    CHECK(bump(e, 0x201000, 5 * 1024), "GPU bump of the 20 KiB view's pool bytes");
    bad = 0;
    for (std::uint32_t i = 0; i < 5 * 1024; ++i) bad += small[i] != 7 * i * 3 + 1;
    std::printf("coherence: 20 KiB view at a 4 KiB offset: %d of 5120 words wrong\n", bad);
    CHECK(bad == 0, "small view coherent");
}

void remaps(Env& e) {
    constexpr std::uint64_t base = 0x400000;
    for (unsigned s = 0; s < 8; ++s) {
        auto* p = reinterpret_cast<std::uint32_t*>(e.pool + base + s * kSlot);
        for (std::uint32_t i = 0; i < kSlot / 4; ++i) p[i] = s << 24 | i;
    }
    int bad = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (unsigned n = 0; n < 2000; ++n) {
        const unsigned s = (n * 5) % 8;
        if (!view(e, 24 * kSlot, base + s * kSlot, kSlot)) {
            ++bad;
            break;
        }
        const auto* t = reinterpret_cast<const std::uint32_t*>(e.title + 24 * kSlot);
        bad += t[n % (kSlot / 4)] != (s << 24 | (n % (kSlot / 4)));
    }
    std::printf("remap: 2000 remaps of one title page over 8 slots in %.1f ms, %d wrong reads\n", since(t0) * 1e3, bad);
    CHECK(bad == 0, "remapped title page reads its slot");

    std::uint32_t u[8] = {};
    constexpr std::uint64_t spin_at = 0x800000;
    addr(u, gpu_of(e, spin_at));
    u[2] = 200000;
    std::memset(e.pool + spin_at, 0, 256 * 1024);
    CHECK(view(e, 32 * kSlot, spin_at, 4 * kSlot), "view of the spun slots");
    const auto s0 = std::chrono::steady_clock::now();
    CHECK(submit(e, e.spin.get(), u, 65536, false), "spin submitted");
    unsigned remapped = 0;
    while (since(s0) < 0.25) {
        view(e, 32 * kSlot, base + (remapped % 8) * kSlot, kSlot);
        view(e, 33 * kSlot, base + ((remapped + 3) % 8) * kSlot, kSlot, PROT_NONE);
        mprotect(e.title + 34 * kSlot, kSlot, PROT_NONE);
        mprotect(e.title + 34 * kSlot, kSlot, PROT_READ | PROT_WRITE);
        ++remapped;
    }
    CHECK(e.q->wait_idle(30'000'000'000ull) == Status::ok, "spin completes");
    CHECK(view(e, 32 * kSlot, spin_at, 4 * kSlot), "view of the spun slots again");
    std::uint32_t expect = 0;
    for (std::uint32_t k = 0; k < 200000; ++k) expect = expect * 1664525u + 1013904223u;
    const auto* t = reinterpret_cast<const std::uint32_t*>(e.title + 32 * kSlot);
    bad = 0;
    for (std::uint32_t i = 0; i < 65536; ++i) bad += t[i] != expect;
    std::printf("remap under GPU use: %u remap/protect rounds of title views during a %.0f ms kernel on the same "
                "chunk; %d of 65536 results wrong when the view is mapped back\n",
                remapped, since(s0) * 1e3, bad);
    CHECK(bad == 0, "the GPU's writes land in the pool whatever the views do");
}

void decommit(Env& e) {
    constexpr std::uint64_t at = 0xc00000;
    CHECK(view(e, 40 * kSlot, at, kSlot), "view");
    auto* t = reinterpret_cast<std::uint32_t*>(e.title + 40 * kSlot);
    for (std::uint32_t i = 0; i < kSlot / 4; ++i) t[i] = i;
    CHECK(mprotect(t, kSlot, PROT_NONE) == 0, "decommit as no-access");
    CHECK(bump(e, at, kSlot / 4), "GPU bump while the view is no-access");
    const auto* p = reinterpret_cast<const std::uint32_t*>(e.pool + at);
    int bad = 0;
    for (std::uint32_t i = 0; i < kSlot / 4; ++i) bad += p[i] != i * 3 + 1;
    CHECK(mprotect(t, kSlot, PROT_READ | PROT_WRITE) == 0, "recommit");
    for (std::uint32_t i = 0; i < kSlot / 4; ++i) bad += t[i] != i * 3 + 1;
    std::printf("decommit as no-access: GPU wrote the pool while the view was no-access, %d wrong after recommit\n", bad);
    CHECK(bad == 0, "decommitted view keeps the alias");
}

void read_overwrite(Env& e) {
    constexpr std::uint64_t at = 0x1000000, bytes = 1 << 20;
    CHECK(view(e, 48 * kSlot, at, bytes), "1 MiB view");
    std::vector<std::uint32_t> src(bytes / 4);
    for (std::uint32_t i = 0; i < bytes / 4; ++i) src[i] = 0xabc00000u ^ i;
    mach_vm_size_t got = 0;
    const kern_return_t kr = mach_vm_read_overwrite(mach_task_self(), reinterpret_cast<mach_vm_address_t>(src.data()),
                                                    bytes, reinterpret_cast<mach_vm_address_t>(e.title + 48 * kSlot), &got);
    CHECK(kr == KERN_SUCCESS && got == bytes, "vm_read_overwrite of 1 MiB into the view: %d", kr);
    const auto* p = reinterpret_cast<const std::uint32_t*>(e.pool + at);
    int bad = 0;
    for (std::uint32_t i = 0; i < bytes / 4; ++i) bad += p[i] != (0xabc00000u ^ i);
    std::printf("vm_read_overwrite of 1 MiB into a title view: %d of %llu words wrong in the pool\n", bad,
                (unsigned long long)(bytes / 4));
    CHECK(bad == 0, "a kernel copy into the view lands in the pool");
    std::string err;
    HostBufferDesc d;
    d.cpu = e.pool + at;
    d.size = bytes;
    d.label = "sub-range after the copy";
    d.resident = false;
    auto sub = e.dev->create_host_buffer(d, &err);
    CHECK(sub != nullptr, "bytesNoCopy over the pool range the copy went into: %s", err.c_str());
    CHECK(bump(e, at, bytes / 4), "GPU bump through chunk 0 after the copy");
    bad = 0;
    const auto* t = reinterpret_cast<const std::uint32_t*>(e.title + 48 * kSlot);
    for (std::uint32_t i = 0; i < bytes / 4; ++i) bad += t[i] != (0xabc00000u ^ i) * 3 + 1;
    CHECK(bad == 0, "GPU writes after the copy reach the view (%d wrong)", bad);
}

void growth(Env& e) {
    e.chunk[2] = wrap(e, 2, false, "pool chunk 2 (late)");
    CHECK(e.chunk[2] != nullptr, "late chunk");
    if (!e.chunk[2]) return;
    const std::uint64_t at = 2 * kChunk + 0x30000;
    CHECK(view(e, 56 * kSlot, at, kSlot), "view into chunk 2");
    auto* t = reinterpret_cast<std::uint32_t*>(e.title + 56 * kSlot);
    for (std::uint32_t i = 0; i < kSlot / 4; ++i) t[i] = i + 5;
    std::uint32_t u[8] = {};
    addr(u, gpu_of(e, at));
    addr(u + 3, gpu_of(e, 0x110000));
    u[2] = kSlot / 4;
    u[5] = 1;
    CHECK(submit(e, e.gather.get(), u, kSlot / 4), "gather chunk 2 -> chunk 0");
    const auto* t0 = reinterpret_cast<const std::uint32_t*>(e.title);
    int bad = 0;
    for (std::uint32_t i = 0; i < kSlot / 4; ++i) bad += t0[i] != i + 5;
    std::printf("growth: chunk 2 wrapped after chunk 0 was in use; one kernel read chunk 2 and wrote chunk 0: %d wrong\n", bad);
    CHECK(bad == 0, "two chunks in one kernel");
}

// GPU-VA-only buffers (BufferDesc::va_only): a hold takes GPU VA and no memory, and a chunk wrapped right after a
// released probe of its size gets the probe's address.
void va_holds(Env& e) {
    BufferDesc hd;
    hd.size = 1ull << 30;
    hd.va_only = true;
    hd.resident = false;
    const Memory m0 = memory_now();
    auto hold = e.dev->create_buffer(hd);
    if (!hold) {
        std::printf("va holds: no placement-sparse buffers on this OS; skipped\n");
        return;
    }
    const Memory m1 = memory_now();
    CHECK(hold->host_gpu_address() != 0, "a VA hold has a GPU address");
    std::printf("va holds: 1 GiB hold at gpu 0x%llx, footprint %+.1f MiB, machine wired %+.0f MiB\n",
                (unsigned long long)hold->host_gpu_address(), m1.footprint_mib - m0.footprint_mib, m1.wired_mib - m0.wired_mib);
    CHECK(m1.footprint_mib - m0.footprint_mib < 16, "a VA hold costs no footprint");
    int same = 0;
    for (unsigned i = 0; i < 16; ++i) {
        hd.size = 4u << 20;
        std::uint64_t probe_va = 0;
        {
            auto probe = e.dev->create_buffer(hd);
            CHECK(probe != nullptr, "probe");
            if (!probe) return;
            probe_va = probe->host_gpu_address();
        }
        std::string err;
        HostBufferDesc d;
        d.cpu = e.pool + 7 * kChunk + i * (4u << 20);
        d.size = 4u << 20;
        d.resident = false;
        auto b = e.dev->create_host_buffer(d, &err);
        CHECK(b != nullptr, "wrap after a probe: %s", err.c_str());
        same += b && b->host_gpu_address() == probe_va;
    }
    std::printf("va holds: a 4 MiB chunk wrapped right after a released probe got the probe's GPU address %d of 16\n", same);
    CHECK(same >= 12, "probe address reused");
}

void residency(Env& e) {
    constexpr unsigned k = 3;
    std::memset(e.pool + k * kChunk, 1, 1 << 20);
    const Memory m0 = memory_now();
    auto b = wrap(e, k, false, "residency probe");
    CHECK(b != nullptr, "probe chunk");
    if (!b) return;
    gta6rt::metal::set_encoder_resource(e.dev, b.get(), false);
    const Memory m1 = memory_now();
    std::uint32_t u[8] = {};
    addr(u, gpu_of(e, 0x110000));
    u[2] = 16;
    CHECK(submit(e, e.bump.get(), u, 16), "kernel with the probe chunk not named");
    const Memory m2 = memory_now();
    gta6rt::metal::set_encoder_resource(e.dev, b.get(), true);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(submit(e, e.bump.get(), u, 16), "kernel with the probe chunk named per encoder");
    const double first = since(t0);
    const Memory m3 = memory_now();
    const auto t1 = std::chrono::steady_clock::now();
    CHECK(submit(e, e.bump.get(), u, 16), "second kernel");
    const double second = since(t1);
    gta6rt::metal::set_encoder_resource(e.dev, b.get(), false);
    CHECK(submit(e, e.bump.get(), u, 16), "kernel after the chunk is no longer named");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const Memory m4 = memory_now();
    std::printf("residency of a 256 MiB chunk with 1 MiB touched (footprint / machine wired, MiB):\n"
                "  before wrap %.0f / %.0f; wrapped, not named %.0f / %.0f; kernel without it %.0f / %.0f;\n"
                "  named per encoder %.0f / %.0f (first kernel %.2f ms, second %.2f ms); no longer named +0.5 s %.0f / %.0f\n",
                m0.footprint_mib, m0.wired_mib, m1.footprint_mib, m1.wired_mib, m2.footprint_mib, m2.wired_mib,
                m3.footprint_mib, m3.wired_mib, first * 1e3, second * 1e3, m4.footprint_mib, m4.wired_mib);
    b.reset();

    constexpr unsigned k2 = 4;
    std::memset(e.pool + k2 * kChunk, 1, 1 << 20);
    const Memory r0 = memory_now();
    auto d = wrap(e, k2, true, "device-resident probe");
    CHECK(d != nullptr, "device-resident probe chunk");
    CHECK(submit(e, e.bump.get(), u, 16), "kernel with the device residency set holding the chunk");
    const Memory r1 = memory_now();
    d.reset();
    CHECK(submit(e, e.bump.get(), u, 16), "kernel after release");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const Memory r2 = memory_now();
    std::printf("  device residency set instead: before %.0f / %.0f, after a kernel %.0f / %.0f, released +0.5 s %.0f / %.0f\n",
                r0.footprint_mib, r0.wired_mib, r1.footprint_mib, r1.wired_mib, r2.footprint_mib, r2.wired_mib);
}

void naming_cost(Env& e) {
    constexpr unsigned kMax = 1024;
    constexpr std::uint64_t kSmall = 256ull << 10;
    std::vector<std::unique_ptr<Buffer>> small;
    for (unsigned i = 0; i < kMax; ++i) {
        HostBufferDesc d;
        d.cpu = e.pool + 5 * kChunk + i * kSmall;
        d.size = kSmall;
        d.label = "naming cost";
        d.resident = false;
        std::string err;
        small.push_back(e.dev->create_host_buffer(d, &err));
        CHECK(small.back() != nullptr, "small chunk %u: %s", i, err.c_str());
    }
    std::uint32_t u[8] = {};
    addr(u, gpu_of(e, 0x110000));
    u[2] = 16;
    unsigned named = 0;
    for (unsigned want : {0u, 16u, 64u, 256u, 1024u}) {
        while (named < want) gta6rt::metal::set_encoder_resource(e.dev, small[named++].get(), true);
        constexpr int kLists = 300;
        submit(e, e.bump.get(), u, 16);
        const auto t0 = std::chrono::steady_clock::now();
        for (int n = 0; n < kLists; ++n) {
            auto l = e.dev->create_command_list(QueueType::compute);
            l->set_root_layout(e.root.get());
            l->set_compute_pipeline(e.bump.get());
            l->set_user_data(0, std::span<const std::uint32_t>(u, 8));
            for (int k = 0; k < 4; ++k) {
                l->dispatch(16, 1, 1, dispatch_flag::thread_counts);
                Barrier b;
                b.type = BarrierType::uav;
                l->barrier(std::span<const Barrier>(&b, 1));
            }
            l->close();
            CommandList* ls[] = {l.get()};
            e.q->execute(ls);
        }
        e.q->wait_idle(30'000'000'000ull);
        std::printf("naming cost: %u buffers named per encoder, %d lists of 4 dispatches: %.3f ms a list\n", want, kLists,
                    since(t0) * 1e3 / kLists);
    }
    for (auto& b : small) gta6rt::metal::set_encoder_resource(e.dev, b.get(), false);
    e.q->wait_idle(30'000'000'000ull);
}

}  // namespace

int main() {
    auto dev = gta6rt::metal::create_device();
    if (!dev) return 1;
    vm_size_t kernel_page = 0;
    host_page_size(mach_host_self(), &kernel_page);
    std::printf("process page %d B, vm_page_size %llu B, host (kernel) page %llu B, maxBufferLength %llu MiB\n",
                getpagesize(), (unsigned long long)vm_page_size, (unsigned long long)kernel_page,
                (unsigned long long)(dev->caps().max_buffer_length >> 20));
    auto queue = dev->create_queue(QueueType::compute, "titlepool");
    Env e;
    e.dev = dev.get();
    e.q = queue.get();
    std::string err;
    e.lib = dev->create_library_msl(test::read_file(test::shader_path("titlepool.metal")), MslOptions{}, &err);
    CHECK(e.lib != nullptr, "library: %s", err.c_str());
    if (!e.lib) return test::finish("metal_test_titlepool");
    e.root = dev->create_root_layout(RootLayoutDesc{});
    auto make = [&](const char* name) {
        ComputePipelineDesc d;
        d.cs = {e.lib.get(), name};
        d.threads = {64, 1, 1};
        d.root = e.root.get();
        auto p = dev->create_compute_pipeline(d);
        CHECK(p->status() == Status::ok, "%s: %s", name, p->error().c_str());
        return p;
    };
    e.bump = make("cs_bump");
    e.gather = make("cs_gather");
    e.spin = make("cs_spin");

    memory_object_size_t size = kPool;
    mach_port_t entry = MACH_PORT_NULL;
    kern_return_t kr = mach_make_memory_entry_64(mach_task_self(), &size, 0,
                                                 MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE, &entry, MACH_PORT_NULL);
    CHECK(kr == KERN_SUCCESS && size == kPool, "memory entry of 2 GiB: %d", kr);
    mach_vm_address_t base = 0;
    kr = mach_vm_map(mach_task_self(), &base, kPool, 0xffff, VM_FLAGS_ANYWHERE, entry, 0, FALSE,
                     VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
    CHECK(kr == KERN_SUCCESS, "pool mapping: %d", kr);
    void* title = mmap(nullptr, kTitle, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK(title != MAP_FAILED, "title range");
    if (test::failures) return test::finish("metal_test_titlepool");
    e.pool = reinterpret_cast<std::uint8_t*>(base);
    e.title = static_cast<std::uint8_t*>(title);
    std::printf("pool: memory entry of %llu MiB mapped at %p; title range %p\n", (unsigned long long)(kPool >> 20),
                e.pool, e.title);
    e.chunk[0] = wrap(e, 0, false, "pool chunk 0");
    CHECK(e.chunk[0] != nullptr, "chunk 0");
    if (test::failures) return test::finish("metal_test_titlepool");
    coherence(e);
    remaps(e);
    decommit(e);
    read_overwrite(e);
    growth(e);
    va_holds(e);
    residency(e);
    naming_cost(e);
    std::printf("encoder resources now %llu, useResource calls %llu\n",
                (unsigned long long)gta6rt::metal::encoder_resource_count(e.dev),
                (unsigned long long)gta6rt::metal::encoder_resource_uses(e.dev));
    for (auto& c : e.chunk)
        if (c) gta6rt::metal::set_encoder_resource(e.dev, c.get(), false);
    queue->wait_idle(30'000'000'000ull);
    for (auto& c : e.chunk) c.reset();
    test::print_stats(dev->stats());
    return test::finish("metal_test_titlepool");
}
