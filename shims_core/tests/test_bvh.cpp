#include <cstdint>
#include <cstring>

#include "shim_call.h"
#include "test.h"
#include "xbox_bvh_hooks.h"

using tcall::call;

namespace {
constexpr const char* kB = "xboxbvhbuilder.dll";
constexpr const char* kCtor = "??0XboxBvhBuilder@@QEAA@PEAUID3D12Device11@@_N@Z";
constexpr const char* kDtor = "??1XboxBvhBuilder@@QEAA@XZ";
constexpr const char* kMorton = "?Set64BitMortonCodes@XboxBvhBuilder@@QEAAX_N@Z";
constexpr const char* kPrebuild =
    "?GetRaytracingAccelerationStructurePrebuildInfo@XboxBvhBuilder@@QEAAXPEBUD3D12_BUILD_RAYTRACING_ACCELERATION_"
    "STRUCTURE_INPUTS@@PEBUD3D12XBOX_XDXR_STREAM_DESC@@PEAUD3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO@@@Z";
constexpr const char* kRehydrate =
    "?RehydrateRaytracingAccelerationStructureBatched2X@XboxBvhBuilder@@QEAAJPEAUID3D12GraphicsCommandList8@@IPEBU"
    "D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC@@PEB_K@Z";
}  // namespace

TEST(bvh_shim_serves_all_seven_imports) {
    for (const char* name : {kCtor, kDtor, kMorton, kPrebuild, kRehydrate,
                             "?BuildRaytracingAccelerationStructureBatched2X@XboxBvhBuilder@@QEAAXPEAUID3D12GraphicsCommandList6@@IPEBUD3D12XBOX_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_BATCHED_DESC2@@@Z",
                             "?CopyRaytracingAccelerationStructure@XboxBvhBuilder@@QEAAXPEAUID3D12GraphicsCommandList4@@_K1W4D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE@@@Z"}) {
        const auto e = rt::shims::find_entry(kB, name);
        CHECK(e.fn != nullptr || e.frame != nullptr);
    }
}

TEST(bvh_builder_state_is_keyed_by_the_title_object) {
    alignas(16) uint8_t storage[2][64] = {};
    CHECK_EQ(rt::xbox_bvh::live_count(), 0u);
    CHECK(call<void*>(kB, kCtor, static_cast<void*>(storage[0]), static_cast<void*>(nullptr), true) == storage[0]);
    CHECK(call<void*>(kB, kCtor, static_cast<void*>(storage[1]), static_cast<void*>(nullptr), false) == storage[1]);
    CHECK_EQ(rt::xbox_bvh::live_count(), 2u);
    call<void>(kB, kMorton, static_cast<void*>(storage[0]), true);

    uint64_t info[3] = {0xAA, 0xBB, 0xCC};
    uint8_t inputs[32] = {};
    call<void>(kB, kPrebuild, static_cast<void*>(storage[0]), static_cast<const void*>(inputs), static_cast<const void*>(nullptr), static_cast<void*>(info));
    CHECK(info[0] == 0 && info[1] == 0 && info[2] == 0);

    CHECK_EQ(call<int32_t>(kB, kRehydrate, static_cast<void*>(storage[0]), static_cast<void*>(nullptr), 0u, static_cast<const void*>(nullptr), static_cast<const uint64_t*>(nullptr)), 0);
    CHECK_EQ(call<int32_t>(kB, kRehydrate, static_cast<void*>(storage[0]), static_cast<void*>(nullptr), 1u, static_cast<const void*>(nullptr), static_cast<const uint64_t*>(nullptr)), static_cast<int32_t>(0x80070057u));

    call<void>(kB, kDtor, static_cast<void*>(storage[0]));
    call<void>(kB, kDtor, static_cast<void*>(storage[0]));
    CHECK_EQ(rt::xbox_bvh::live_count(), 1u);
    call<void>(kB, kDtor, static_cast<void*>(storage[1]));
    CHECK_EQ(rt::xbox_bvh::live_count(), 0u);
}
