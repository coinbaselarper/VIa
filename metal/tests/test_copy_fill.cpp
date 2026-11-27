#include "test_util.h"

#include <cstring>

using namespace gta6rt::gpu;

namespace {

std::uint32_t texel(std::uint32_t x, std::uint32_t y) { return 0xa0000000u | (y << 8) | x; }

bool run(Queue& q, CommandList& list) {
    if (list.close() != Status::ok) return false;
    CommandList* lists[] = {&list};
    return q.execute(lists) == Status::ok && q.wait_idle(10'000'000'000ull) == Status::ok;
}

}  // namespace

int main() {
    auto dev = gta6rt::metal::create_device();
    if (!dev) return 1;
    auto q = dev->create_queue(QueueType::direct, "copy_fill");
    TextureDesc td;
    td.dimension = TextureDimension::tex2d;
    td.format = format::r8g8b8a8_unorm;
    td.width = td.height = 8;
    td.usage = usage::shader_read | usage::copy;
    auto src = dev->create_texture(td);
    auto dst = dev->create_texture(td);
    CHECK(src && dst && q, "textures and queue");
    if (!src || !dst || !q) return test::finish("copy_fill");

    auto up = dev->create_buffer({8 * 8 * 4, MemoryKind::upload, usage::copy, "up"});
    auto* p = static_cast<std::uint32_t*>(up->cpu_pointer());
    for (std::uint32_t y = 0; y < 8; ++y)
        for (std::uint32_t x = 0; x < 8; ++x) p[y * 8 + x] = texel(x, y);
    auto list = dev->create_command_list(QueueType::direct);
    BufferTextureCopy c;
    c.buffer = up.get();
    c.row_pitch = 32;
    c.texture = src.get();
    list->copy_buffer_to_texture(c);
    CHECK(run(*q, *list), "upload");

    auto zero = dev->create_buffer({8 * 8 * 4, MemoryKind::upload, usage::copy, "zero"});
    std::memset(zero->cpu_pointer(), 0, 8 * 8 * 4);
    list = dev->create_command_list(QueueType::direct);
    c.buffer = zero.get();
    c.texture = dst.get();
    list->copy_buffer_to_texture(c);
    TextureCopy k;
    k.src = src.get();
    k.src_x = 2;
    k.src_y = 1;
    k.dst = dst.get();
    k.dst_x = 4;
    k.dst_y = 5;
    k.width = 3;
    k.height = 2;
    list->copy_texture(k);
    CHECK(run(*q, *list), "copy_texture");
    std::vector<std::uint8_t> got;
    CHECK(test::read_texture(*dev, *q, *dst, 4, got) && got.size() == 256, "read back (%zu bytes)", got.size());
    int wrong = 0;
    for (std::uint32_t y = 0; y < 8 && got.size() == 256; ++y)
        for (std::uint32_t x = 0; x < 8; ++x) {
            std::uint32_t v;
            std::memcpy(&v, got.data() + (y * 8 + x) * 4, 4);
            const bool inside = x >= 4 && x < 7 && y >= 5 && y < 7;
            wrong += v != (inside ? texel(x - 4 + 2, y - 5 + 1) : 0u);
        }
    CHECK(!wrong, "%d texels differ after a 3x2 copy from (2,1) to (4,5)", wrong);

    auto buf = dev->create_buffer({4096, MemoryKind::shared, usage::copy | usage::unordered_access, "fill"});
    std::memset(buf->cpu_pointer(), 0xee, 4096);
    list = dev->create_command_list(QueueType::direct);
    list->fill_buffer_u32(buf.get(), 64, 1024, 0x12345678u);
    list->fill_buffer_u32(buf.get(), 2048, 512, 0x07070707u);
    CHECK(run(*q, *list), "fill_buffer_u32");
    const auto* w = static_cast<const std::uint32_t*>(buf->cpu_pointer());
    int bad = 0;
    for (std::uint32_t i = 0; i < 1024; ++i) {
        const std::uint32_t at = i * 4;
        const std::uint32_t want = at >= 64 && at < 64 + 1024        ? 0x12345678u
                                   : at >= 2048 && at < 2048 + 512 ? 0x07070707u
                                                                   : 0xeeeeeeeeu;
        bad += w[i] != want;
    }
    CHECK(!bad, "%d words wrong after two fills (kernel and blit)", bad);
    return test::finish("copy_fill");
}
