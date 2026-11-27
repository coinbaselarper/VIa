#include "test_util.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace test {

int failures = 0;

using namespace gta6rt::gpu;

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

std::string shader_path(const char* name) { return std::string(GTA6RT_METAL_SHADER_DIR) + "/" + name; }

bool headless() {
    const char* h = std::getenv("GTA6RT_METAL_HEADLESS");
    return h && *h && *h != '0';
}

void print_caps(const DeviceCaps& c) {
    std::printf("device: %s, process %s\n", c.name.c_str(), c.process_arch.c_str());
    std::printf("  argument buffers tier %u, residency sets %d, unified memory %d\n", c.argument_buffers_tier,
                c.residency_sets, c.unified_memory);
    std::printf("  families: apple7 %d apple8 %d apple9 %d metal3 %d metal4 %d\n", c.family_apple7, c.family_apple8,
                c.family_apple9, c.family_metal3, c.family_metal4);
    std::printf("  barycentrics %d, placement sparse %d, d24s8 %d, simd width %u\n", c.barycentrics, c.placement_sparse,
                c.d24s8, c.simd_width);
    std::printf("  max buffer %llu MiB, threadgroup memory %u B, working set %llu MiB, timestamp frequency %llu Hz\n",
                (unsigned long long)(c.max_buffer_length >> 20), c.max_threadgroup_memory,
                (unsigned long long)(c.recommended_working_set >> 20), (unsigned long long)c.timestamp_frequency);
}

void print_stats(const DeviceStats& s) {
    std::printf("stats: pipelines created %llu compiled %llu; encoders render %llu blit %llu; command buffers %llu\n",
                (unsigned long long)s.pipelines_created, (unsigned long long)s.pipelines_compiled,
                (unsigned long long)s.render_encoders, (unsigned long long)s.blit_encoders,
                (unsigned long long)s.command_buffers);
    std::printf("       barriers no-op %llu fenced %llu; clears folded %llu; stores discarded %llu; value writes %llu "
                "waits %llu; rect draws %llu\n",
                (unsigned long long)s.barriers_noop, (unsigned long long)s.barriers_fenced,
                (unsigned long long)s.clears_folded, (unsigned long long)s.stores_discarded,
                (unsigned long long)s.value_writes, (unsigned long long)s.value_waits,
                (unsigned long long)s.rect_draws_expanded);
    std::printf("       fence joins %llu (render-target joins %llu); device residency commits %llu; command buffer errors %llu "
                "(discarded by GPU recovery %llu, event signals repaired %llu)\n",
                (unsigned long long)s.fence_joins, (unsigned long long)s.target_joins,
                (unsigned long long)s.residency_commits, (unsigned long long)s.command_buffer_errors,
                (unsigned long long)s.command_buffer_victims, (unsigned long long)s.event_signals_repaired);
}

bool read_texture(Device& device, Queue& queue, Texture& texture, std::uint32_t bpp, std::vector<std::uint8_t>& out,
                  Status* status) {
    out.clear();
    Status dummy;
    Status& s = status ? *status : dummy;
    s = Status::out_of_memory;
    const auto& d = texture.desc();
    BufferDesc bd;
    bd.size = std::uint64_t(d.width) * d.height * bpp;
    bd.memory = MemoryKind::readback;
    auto rb = device.create_buffer(bd);
    auto list = device.create_command_list(QueueType::direct);
    if (!rb || !list) return false;
    static const bool fill = [] {
        const char* v = std::getenv("GTA6RT_TEST_NO_READBACK_FILL");
        return !(v && *v && *v != '0');
    }();
    if (fill) std::memset(rb->cpu_pointer(), kReadbackFill, bd.size);
    BufferTextureCopy c;
    c.buffer = rb.get();
    c.texture = &texture;
    c.row_pitch = d.width * bpp;
    list->copy_texture_to_buffer(c);
    if ((s = list->close()) != Status::ok) return false;
    CommandList* lists[] = {list.get()};
    if ((s = queue.execute(lists)) != Status::ok) return false;
    if ((s = queue.wait_idle(10'000'000'000ull)) != Status::ok) return false;
    const auto* p = static_cast<const std::uint8_t*>(rb->cpu_pointer());
    out.assign(p, p + bd.size);
    return true;
}

int finish(const char* name) {
    if (failures) {
        std::printf("%s: FAIL (%d checks failed)\n", name, failures);
        return 1;
    }
    std::printf("%s: PASS\n", name);
    return 0;
}

}  // namespace test
