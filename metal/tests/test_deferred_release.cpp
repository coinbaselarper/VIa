// Releases wait for the GPU (MetalDevice::retire). A command buffer that waits at an event is committed but not
// scheduled; a buffer its dispatch reads only through a raw GPU address (named by the device residency set alone,
// not retained by the command buffer, as the translated shaders' V# buffers are) is destroyed meanwhile, then the
// event is signalled. With deferred releases the dispatch reads the buffer and the command buffer completes;
// GTA6RT_METAL_RELEASE_AT_ONCE=1 (negative control) releases at once, as before (spike/r7-device-loss: code 9).
#include "test_util.h"

#include <cstring>

using namespace gta6rt::gpu;

int main() {
    auto dev = gta6rt::metal::create_device();
    if (!dev) return 1;
    const bool at_once = std::getenv("GTA6RT_METAL_RELEASE_AT_ONCE") != nullptr;
    auto q = dev->create_queue(QueueType::compute, "deferred release");
    auto gate = dev->create_fence(0);
    std::string err;
    auto lib = dev->create_library_msl(test::read_file(test::shader_path("release.metal")), MslOptions{}, &err);
    CHECK(q && gate && lib, "queue, fence, library: %s", err.c_str());
    if (!q || !gate || !lib) return test::finish("metal_test_deferred_release");
    RootLayoutDesc rd;
    auto root = dev->create_root_layout(rd);
    ComputePipelineDesc cd;
    cd.cs = {lib.get(), "cs_copy_address"};
    cd.threads = {64, 1, 1};
    cd.root = root.get();
    auto pipe = dev->create_compute_pipeline(cd);
    CHECK(pipe->status() == Status::ok, "pipeline: %s", pipe->error().c_str());

    int wrong_total = 0, errors_total = 0;
    const int rounds = 8;
    for (int round = 0; round < rounds; ++round) {
        auto in = dev->create_buffer({4096 * 4, MemoryKind::shared, usage::shader_read, "release in"});
        auto* words = static_cast<std::uint32_t*>(in->cpu_pointer());
        std::vector<std::uint32_t> expect(4096);
        for (std::uint32_t i = 0; i < 4096; ++i) expect[i] = words[i] = 0x5a000000u | (std::uint32_t(round) << 16) | i;
        auto out = dev->create_buffer({4096 * 4, MemoryKind::shared, usage::unordered_access, "release out"});
        std::memset(out->cpu_pointer(), 0xee, 4096 * 4);

        auto l = dev->create_command_list(QueueType::compute);
        l->set_root_layout(root.get());
        l->set_compute_pipeline(pipe.get());
        const std::uint64_t o = out->host_gpu_address(), s = in->host_gpu_address();
        std::uint32_t u[4] = {std::uint32_t(o), std::uint32_t(o >> 32), std::uint32_t(s), std::uint32_t(s >> 32)};
        l->set_user_data(0, u);
        l->dispatch(64, 1, 1);
        CHECK(l->close() == Status::ok, "close");
        const std::uint64_t value = std::uint64_t(round) + 1;
        q->wait(gate.get(), value);
        CommandList* lists[] = {l.get()};
        CHECK(q->execute(lists) == Status::ok, "execute");
        const DeviceStats before = dev->stats();
        if (!std::getenv("TEST_KEEP")) in.reset();
        const DeviceStats held = dev->stats();
        gate->signal_cpu(value);
        CHECK(q->wait_idle(10'000'000'000ull) == Status::ok || at_once, "wait_idle");
        const DeviceStats after = dev->stats();
        const auto* got = static_cast<const std::uint32_t*>(out->cpu_pointer());
        int wrong = 0;
        for (std::uint32_t i = 0; i < 4096; ++i) wrong += got[i] != expect[i];
        if (wrong && round == 0) std::printf("first words %08x %08x, expected %08x %08x\n", got[0], got[1], expect[0], expect[1]);
        const int errors = int(after.command_buffer_errors - before.command_buffer_errors);
        wrong_total += wrong;
        errors_total += errors;
        std::printf("round %d: releases held while the command buffer waited %llu (deferred in all %llu, at once %llu); "
                    "after the signal: %d of 4096 words wrong, %d command buffer errors\n",
                    round, (unsigned long long)held.releases_waiting, (unsigned long long)held.releases_deferred,
                    (unsigned long long)held.releases_immediate, wrong, errors);
        if (!at_once && !std::getenv("TEST_KEEP"))
            CHECK(held.releases_waiting > 0, "the buffer's release waited for the command buffer");
    }
    if (at_once) {
        std::printf("negative control (GTA6RT_METAL_RELEASE_AT_ONCE): %d words wrong, %d command buffer errors over %d "
                    "rounds\n", wrong_total, errors_total, rounds);
        CHECK(wrong_total || errors_total, "releasing at once is seen (wrong words or a failed command buffer)");
    } else {
        CHECK(wrong_total == 0 && errors_total == 0, "%d words wrong, %d command buffer errors", wrong_total, errors_total);
        auto l = dev->create_command_list(QueueType::compute);
        CHECK(l->close() == Status::ok, "close");
        CommandList* lists[] = {l.get()};
        q->execute(lists);
        q->wait_idle(1'000'000'000ull);
        CHECK(dev->stats().releases_waiting == 0, "every held release went once the GPU passed it (%llu left)",
              (unsigned long long)dev->stats().releases_waiting);
    }
    return test::finish("metal_test_deferred_release");
}
