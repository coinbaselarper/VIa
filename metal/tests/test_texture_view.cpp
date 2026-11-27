#include "test_util.h"

#include <cstring>

using namespace gta6rt::gpu;

namespace {

std::uint32_t texel(std::uint32_t layer, std::uint32_t mip, std::uint32_t x, std::uint32_t y) {
    return (layer << 24) | (mip << 16) | (y << 8) | x;
}

bool fill(Device& dev, Queue& q, Texture& t, std::uint32_t layer, std::uint32_t mip, std::uint32_t w, std::uint32_t h) {
    auto buf = dev.create_buffer({std::uint64_t(w) * h * 4, MemoryKind::upload, usage::copy, "fill"});
    auto list = dev.create_command_list(QueueType::direct);
    if (!buf || !list) return false;
    auto* p = static_cast<std::uint32_t*>(buf->cpu_pointer());
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) p[y * w + x] = texel(layer, mip, x, y);
    BufferTextureCopy c;
    c.buffer = buf.get();
    c.row_pitch = w * 4;
    c.texture = &t;
    c.mip = mip;
    c.layer = layer;
    list->copy_buffer_to_texture(c);
    if (list->close() != Status::ok) return false;
    CommandList* lists[] = {list.get()};
    return q.execute(lists) == Status::ok && q.wait_idle(10'000'000'000ull) == Status::ok;
}

}  // namespace

int main() {
    auto dev = gta6rt::metal::create_device();
    if (!dev) return 1;
    auto q = dev->create_queue(QueueType::direct, "view");
    TextureDesc td;
    td.dimension = TextureDimension::tex2d_array;
    td.format = format::r8g8b8a8_unorm;
    td.width = td.height = 8;
    td.depth_or_layers = 3;
    td.mip_levels = 2;
    td.usage = usage::shader_read | usage::copy | usage::pixel_format_view;
    auto tex = dev->create_texture(td);
    CHECK(tex && q, "texture and queue");
    if (!tex || !q) return test::finish("texture_view");
    for (std::uint32_t l = 0; l < 3; ++l)
        for (std::uint32_t m = 0; m < 2; ++m) CHECK(fill(*dev, *q, *tex, l, m, 8 >> m, 8 >> m), "fill %u %u", l, m);

    std::string err;
    TextureViewDesc vd;
    vd.dimension = TextureDimension::tex2d;
    vd.first_layer = 2;
    vd.first_mip = 1;
    vd.mip_count = 1;
    vd.format = format::b8g8r8a8_unorm;
    auto view = dev->create_texture_view(tex.get(), vd, &err);
    CHECK(view, "layer 2 mip 1 BGRA view: %s", err.c_str());
    if (view) {
        CHECK(view->desc().width == 4 && view->desc().height == 4 && view->desc().mip_levels == 1 &&
                  view->desc().depth_or_layers == 1,
              "view desc %ux%u mips %u layers %u", view->desc().width, view->desc().height, view->desc().mip_levels,
              view->desc().depth_or_layers);
        std::vector<std::uint8_t> got;
        CHECK(test::read_texture(*dev, *q, *view, 4, got) && got.size() == 64, "read view (%zu bytes)", got.size());
        int wrong = 0;
        for (std::uint32_t y = 0; y < 4 && got.size() == 64; ++y)
            for (std::uint32_t x = 0; x < 4; ++x) {
                std::uint32_t v;
                std::memcpy(&v, got.data() + (y * 4 + x) * 4, 4);
                wrong += v != texel(2, 1, x, y);
            }
        CHECK(!wrong, "%d texels differ from layer 2 mip 1", wrong);
    }

    vd = {};
    vd.dimension = TextureDimension::tex2d_array;
    vd.first_layer = 1;
    vd.layer_count = 2;
    auto arr = dev->create_texture_view(tex.get(), vd, &err);
    CHECK(arr && arr->desc().depth_or_layers == 2 && arr->desc().mip_levels == 2, "layers 1-2 array view: %s",
          err.c_str());

    vd = {};
    vd.format = format::r32_float;
    vd.dimension = TextureDimension::tex2d;
    auto same_size = dev->create_texture_view(tex.get(), vd, &err);
    CHECK(same_size, "R32F view of RGBA8 (same texel size): %s", err.c_str());

    vd = {};
    vd.first_mip = 2;
    auto past = dev->create_texture_view(tex.get(), vd, &err);
    CHECK(!past, "a first mip past the texture is refused");
    TextureDesc vol;
    vol.dimension = TextureDimension::tex3d;
    vol.format = format::r8g8b8a8_unorm;
    vol.width = vol.height = vol.depth_or_layers = 4;
    vol.memory = MemoryKind::shared;
    vol.usage = usage::shader_read | usage::copy;
    auto v3 = dev->create_texture(vol);
    CHECK(v3, "3D texture");
    if (v3) {
        std::uint32_t slice[16];
        for (std::uint32_t z = 0; z < 4; ++z) {
            for (std::uint32_t i = 0; i < 16; ++i) slice[i] = texel(z, 0, i % 4, i / 4);
            CHECK(v3->write_region(0, z, 0, 0, 4, 4, slice, 16) == Status::ok, "write_region z=%u", z);
        }
        auto rb = dev->create_buffer({4 * 64, MemoryKind::readback, usage::copy, "3d readback"});
        auto list = dev->create_command_list(QueueType::direct);
        for (std::uint32_t z = 0; z < 4; ++z) {
            BufferTextureCopy c;
            c.buffer = rb.get();
            c.offset = z * 64;
            c.row_pitch = 16;
            c.texture = v3.get();
            c.layer = z;
            list->copy_texture_to_buffer(c);
        }
        CommandList* lists[] = {list.get()};
        CHECK(list->close() == Status::ok && q->execute(lists) == Status::ok && q->wait_idle(10'000'000'000ull) == Status::ok,
              "3D slice copies");
        const auto* got = static_cast<const std::uint32_t*>(rb->cpu_pointer());
        int wrong = 0;
        for (std::uint32_t z = 0; z < 4; ++z)
            for (std::uint32_t i = 0; i < 16; ++i) wrong += got[z * 16 + i] != texel(z, 0, i % 4, i / 4);
        CHECK(!wrong, "%d of 64 3D texels differ (write_region and copy by z slice)", wrong);
    }
    return test::finish("texture_view");
}
