#include <cstdint>
#include <cstring>
#include <vector>

#include "shim_call.h"
#include "test.h"
#include "title_notes.h"
#include "xg_xs_layout.h"

using tcall::call;
using tcall::call_thunk;

namespace {

constexpr const char* kXg = "xg_xs.dll";
constexpr int32_t kENotImpl = static_cast<int32_t>(0x80004001u), kEInvalidArg = static_cast<int32_t>(0x80070057u);

rt::xg_xs::ResourceDesc desc2d(uint64_t w, uint32_t h, uint16_t mips, uint32_t format, uint32_t layout, uint32_t flags = 0) {
    rt::xg_xs::ResourceDesc d{};
    d.dimension = 3;
    d.width = w;
    d.height = h;
    d.depth_or_array = 1;
    d.mips = mips;
    d.format = format;
    d.samples = 1;
    d.layout = layout;
    d.flags = flags;
    return d;
}

gta6rt_gva slot(const void* computer, int index) {
    const void* const* vt = *static_cast<const void* const* const*>(computer);
    return reinterpret_cast<uintptr_t>(vt[index]);
}

}  // namespace

TEST(xg_suggest_swizzle_mode) {
    auto d = desc2d(256, 256, 1, 28, 0);
    uint32_t mode = 0;
    CHECK_EQ(call<int32_t>(kXg, "XGSuggestSwizzleMode", &d, 0u, &mode), 0);
    CHECK_EQ(mode & 0xFFFFFF00u, 0x100u);
    CHECK_EQ(call<int32_t>(kXg, "XGSuggestSwizzleMode", &d, 1u, &mode), kENotImpl);
    CHECK_EQ(call<int32_t>(kXg, "XGSuggestSwizzleMode", static_cast<void*>(nullptr), 0u, &mode), kEInvalidArg);
    d.flags = 2;
    CHECK_EQ(call<int32_t>(kXg, "XGSuggestSwizzleMode", &d, 0u, &mode), 0);
}

TEST(xg_texture_computer_sizes_and_vtable_thunks) {
    auto d = desc2d(128, 128, 1, 28, 0x100 | 5);
    void* computer = nullptr;
    CHECK_EQ(call<int32_t>(kXg, "XGCreateTextureComputer", &d, &computer), 0);
    CHECK(computer != nullptr);
    if (!computer) return;
    const uint64_t size = call_thunk<uint64_t>(slot(computer, 3), computer);
    const uint64_t alignment = call_thunk<uint64_t>(slot(computer, 4), computer);
    CHECK(size >= 128ull * 128 * 4);
    CHECK_EQ(alignment, 4096u);
    CHECK_EQ(call_thunk<uint64_t>(slot(computer, 5), computer, 0u, 0u), 0u);
    CHECK_EQ(call_thunk<uint32_t>(slot(computer, 0), computer), 2u);
    CHECK_EQ(call_thunk<uint32_t>(slot(computer, 1), computer), 1u);
    CHECK_EQ(call_thunk<int32_t>(slot(computer, 20), computer), kENotImpl);

    std::vector<uint8_t> linear(128 * 128 * 4), tiled(size, 0), back(linear.size(), 0);
    for (size_t i = 0; i < linear.size(); ++i) linear[i] = static_cast<uint8_t>(i * 131 + (i >> 8));
    bool overflow = false;
    leonida::title_texel_writes::take(overflow);
    CHECK_EQ(call_thunk<int32_t>(slot(computer, 8), computer, tiled.data(), 0u, 0u, linear.data(), 128u * 4, 0u), 0);
    auto ranges = leonida::title_texel_writes::take(overflow);
    CHECK(!ranges.empty() && ranges.front().first == reinterpret_cast<uintptr_t>(tiled.data()));
    CHECK(tiled != std::vector<uint8_t>(size, 0));
    CHECK_EQ(call_thunk<int32_t>(slot(computer, 9), computer, back.data(), 0u, 0u, 0u, tiled.data(), 128u * 4, 0u), 0);
    CHECK(back == linear);
    CHECK_EQ(call_thunk<int32_t>(slot(computer, 8), computer, tiled.data(), 9u, 0u, linear.data(), 128u * 4, 0u), kEInvalidArg);
    CHECK_EQ(call_thunk<int32_t>(slot(computer, 8), computer, tiled.data(), 0u, 0u, linear.data(), 64u, 0u), kEInvalidArg);
    uint32_t bit = 7;
    const uint64_t off = call_thunk<uint64_t>(slot(computer, 6), computer, 0u, 0u, uint64_t{3}, 2u, 0u, 0u, &bit);
    CHECK(off < size);
    CHECK_EQ(call_thunk<uint32_t>(slot(computer, 1), computer), 0u);
}

TEST(xg_failed_creation_still_returns_a_usable_computer) {
    void* computer = nullptr;
    CHECK_EQ(call<int32_t>(kXg, "XGCreateTextureComputer", static_cast<void*>(nullptr), &computer), kEInvalidArg);
    CHECK(computer != nullptr);
    if (computer) {
        CHECK_EQ(call_thunk<uint64_t>(slot(computer, 3), computer), 0u);
        CHECK_EQ(call_thunk<int32_t>(slot(computer, 8), computer, static_cast<void*>(nullptr), 0u, 0u, static_cast<void*>(nullptr), 0u, 0u), kENotImpl);
        CHECK_EQ(call_thunk<uint32_t>(slot(computer, 0), computer), 1u);
    }
    CHECK_EQ(call<int32_t>(kXg, "XGCreateTextureComputer", static_cast<void*>(nullptr), static_cast<void**>(nullptr)), static_cast<int32_t>(0x80004003u));
}

TEST(xg_render_target_uses_the_second_model) {
    auto d = desc2d(256, 256, 1, 28, 0, 1);
    void* computer = nullptr;
    const int32_t hr = call<int32_t>(kXg, "XGCreateTextureComputer", &d, &computer);
    CHECK(hr == 0);
    CHECK(computer != nullptr);
    if (hr == 0 && computer) {
        CHECK(call_thunk<uint64_t>(slot(computer, 3), computer) >= 256ull * 256 * 4);
        CHECK_EQ(call_thunk<uint32_t>(slot(computer, 1), computer), 0u);
    }
}

TEST(xg_layout_notes_record_the_title_layout_word) {
    auto d = desc2d(512, 512, 1, 28, 0x100 | 5);
    void* computer = nullptr;
    CHECK_EQ(call<int32_t>(kXg, "XGCreateTextureComputer", &d, &computer), 0);
    uint32_t layout = 0;
    CHECK(leonida::title_xg_layouts::find({3, 28, 512, 1, 1, 512}, layout));
    CHECK_EQ(layout, 0x105u);
    if (computer) call_thunk<uint32_t>(slot(computer, 1), computer);
}
