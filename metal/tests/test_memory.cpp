#include "test_util.h"

#include <sys/mman.h>

#include <atomic>
#include <chrono>
#include <mutex>

using namespace gta6rt::gpu;

namespace {

std::uint64_t cat(const MemoryBudget& b, MemoryCategory c) { return b.bytes[std::size_t(c)]; }
std::uint64_t objs(const MemoryBudget& b, MemoryCategory c) { return b.objects[std::size_t(c)]; }

void print(const char* when, const MemoryBudget& b) {
    std::printf("%s: device allocated %.2f MiB, footprint %.1f MiB;", when, b.device_allocated / 1048576.0,
                b.process_footprint / 1048576.0);
    for (std::size_t i = 0; i < std::size_t(MemoryCategory::count); ++i)
        std::printf(" %s %.2f MiB/%llu", memory_category_name(MemoryCategory(i)), b.bytes[i] / 1048576.0,
                    (unsigned long long)b.objects[i]);
    std::printf("\n");
}

}  // namespace

int main() {
    auto dev = gta6rt::metal::create_device();
    if (!dev) return 1;
    test::print_caps(dev->caps());
    const MemoryBudget b0 = dev->memory_budget();
    std::printf("budget: recommended working set %.0f MiB, physical memory %.0f MiB, pressure %u\n",
                b0.recommended_working_set / 1048576.0, b0.physical_memory / 1048576.0, unsigned(b0.pressure));
    print("baseline", b0);

    std::vector<std::unique_ptr<Buffer>> bufs;
    bufs.push_back(dev->create_buffer({1 << 20, MemoryKind::shared, usage::copy, "1 MiB shared"}));
    bufs.push_back(dev->create_buffer({4 << 20, MemoryKind::device_local, usage::copy, "4 MiB local"}));
    TextureDesc td;
    td.format = format::r8g8b8a8_unorm;
    td.width = td.height = 1024;
    td.usage = usage::render_target | usage::shader_read;
    auto t1 = dev->create_texture(td);
    td.format = format::d32_float;
    td.width = td.height = 512;
    td.usage = usage::depth_stencil;
    auto t2 = dev->create_texture(td);
    HeapDesc hd;
    hd.size = 16 << 20;
    auto heap = dev->create_heap(hd);
    auto placed = dev->create_placed_buffer(heap.get(), 0, {1 << 20, MemoryKind::device_local, usage::copy, "placed"});
    auto table = dev->create_descriptor_heap({DescriptorKind::texture, 65536, "table"});
    auto queries = dev->create_query_heap({QueryType::occlusion, 1024, "queries"});
    const std::uint64_t page = dev->caps().host_page_size;
    const std::uint64_t win_bytes = 8ull << 20;
    void* host = mmap(nullptr, win_bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    std::string err;
    auto window = dev->create_host_buffer({host, win_bytes, 0, "window"}, &err);
    CHECK(bufs[0] && bufs[1] && t1 && t2 && heap && placed && table && queries && window, "allocations: %s", err.c_str());
    const MemoryBudget b1 = dev->memory_budget();
    print("allocated", b1);
    struct Expect { MemoryCategory c; std::uint64_t min, objects; };
    const Expect ex[] = {{MemoryCategory::buffer, 5ull << 20, 2},
                         {MemoryCategory::texture, (4ull << 20) + (1ull << 20), 2},
                         {MemoryCategory::heap, 16ull << 20, 1},
                         {MemoryCategory::descriptor_table, 512ull << 10, 1},
                         {MemoryCategory::internal, 8ull << 10, 1},
                         {MemoryCategory::host_window, win_bytes, 1}};
    std::uint64_t gpu_ledger = 0;
    for (const Expect& e : ex) {
        const std::uint64_t d = cat(b1, e.c) - cat(b0, e.c);
        const std::uint64_t n = objs(b1, e.c) - objs(b0, e.c);
        std::printf("  %-16s +%llu B in %llu objects (requested >= %llu)\n", memory_category_name(e.c),
                    (unsigned long long)d, (unsigned long long)n, (unsigned long long)e.min);
        CHECK(d >= e.min && d < e.min + e.min / 8 + (64 << 10) && n == e.objects, "%s accounting",
              memory_category_name(e.c));
        if (e.c != MemoryCategory::host_window) gpu_ledger += d;
    }
    const std::int64_t dev_delta = std::int64_t(b1.device_allocated) - std::int64_t(b0.device_allocated);
    std::printf("  ledger (GPU categories) +%llu B, MTLDevice currentAllocatedSize %+lld B (difference %+lld B)\n",
                (unsigned long long)gpu_ledger, (long long)dev_delta, (long long)(dev_delta - std::int64_t(gpu_ledger)));
    CHECK(std::llabs(dev_delta - std::int64_t(gpu_ledger)) < std::int64_t(win_bytes + (2 << 20)),
          "ledger agrees with currentAllocatedSize to within the window and 2 MiB");

    bufs.clear();
    t1.reset();
    t2.reset();
    placed.reset();
    heap.reset();
    table.reset();
    queries.reset();
    window.reset();
    munmap(host, win_bytes);
    const MemoryBudget b2 = dev->memory_budget();
    print("freed", b2);
    for (const Expect& e : ex)
        CHECK(cat(b2, e.c) == cat(b0, e.c) && objs(b2, e.c) == objs(b0, e.c), "%s back to baseline",
              memory_category_name(e.c));

    std::mutex m;
    std::vector<std::unique_ptr<Texture>> cache;
    td.format = format::r8g8b8a8_unorm;
    td.width = td.height = 512;
    td.usage = usage::shader_read;
    for (int i = 0; i < 64; ++i) cache.push_back(dev->create_texture(td));
    std::atomic<int> calls{0};
    std::vector<MemoryPressure> seen;
    const std::uint64_t token = dev->add_memory_pressure_handler([&](MemoryPressure level) {
        std::lock_guard lock(m);
        seen.push_back(level);
        ++calls;
        if (level != MemoryPressure::normal) cache.resize(cache.size() / 2);
    });
    const std::uint64_t before = cat(dev->memory_budget(), MemoryCategory::texture);
    dev->simulate_memory_pressure(MemoryPressure::warning);
    const std::uint64_t after_warn = cat(dev->memory_budget(), MemoryCategory::texture);
    dev->simulate_memory_pressure(MemoryPressure::critical);
    const std::uint64_t after_crit = cat(dev->memory_budget(), MemoryCategory::texture);
    dev->simulate_memory_pressure(MemoryPressure::normal);
    const MemoryBudget b3 = dev->memory_budget();
    dev->remove_memory_pressure_handler(token);
    dev->simulate_memory_pressure(MemoryPressure::warning);
    std::printf("governor: 64 cached 1 MiB textures (%.1f MiB); warning -> %.1f MiB, critical -> %.1f MiB; handler calls %d "
                "(levels %u %u %u), budget pressure after normal %u, events %llu; removed handler not called again: %d\n",
                before / 1048576.0, after_warn / 1048576.0, after_crit / 1048576.0, calls.load(),
                seen.size() > 0 ? unsigned(seen[0]) : 9, seen.size() > 1 ? unsigned(seen[1]) : 9,
                seen.size() > 2 ? unsigned(seen[2]) : 9, unsigned(b3.pressure), (unsigned long long)b3.pressure_events,
                calls.load() == 3);
    CHECK(calls.load() == 3 && seen.size() == 3 && seen[0] == MemoryPressure::warning &&
              seen[1] == MemoryPressure::critical && seen[2] == MemoryPressure::normal,
          "pressure handlers run with each level");
    CHECK(after_warn < before && after_crit < after_warn, "the governor's evictions show in the texture ledger");
    CHECK(b3.pressure == MemoryPressure::normal, "budget reports the last level");

    const auto t0 = std::chrono::steady_clock::now();
    std::uint64_t sink = 0;
    for (int i = 0; i < 10000; ++i) sink += dev->memory_budget().device_allocated;
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 10000;
    std::printf("memory_budget(): %.2f us a call (%llu)\n", us, (unsigned long long)(sink & 1));
    test::print_stats(dev->stats());
    return test::finish("metal_test_memory");
}
