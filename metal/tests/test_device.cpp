#include "test_util.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace gta6rt::gpu;

namespace {

void run_lists(Queue& q, std::initializer_list<CommandList*> lists) {
    std::vector<CommandList*> v(lists);
    CHECK(q.execute(v) == Status::ok, "execute");
}

void test_fences(Device& dev) {
    auto q = dev.create_queue(QueueType::direct, "fences");
    auto f = dev.create_fence(0);
    CHECK(q && f, "queue and fence");
    std::atomic<int> fired{0};
    CHECK(f->on_completion(2, [&] { fired++; }) == Status::ok, "on_completion");
    CHECK(q->signal(f.get(), 1) == Status::ok, "signal 1");
    CHECK(q->signal(f.get(), 2) == Status::ok, "signal 2");
    CHECK(f->wait_cpu(2, 5'000'000'000ull) == Status::ok, "wait_cpu 2");
    CHECK(f->completed_value() == 2, "completed %llu", (unsigned long long)f->completed_value());
    for (int i = 0; i < 500 && (fired.load() == 0 || *f->value_address() != 2); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(fired.load() == 1, "SetEventOnCompletion callback fired %d", fired.load());
    CHECK(*f->value_address() == 2, "slot-12 mirror %llu", (unsigned long long)*f->value_address());
    CHECK(f->wait_cpu(3, 20'000'000ull) == Status::timeout, "wait for an unsignalled value times out");
    CHECK(f->signal_cpu(5) == Status::ok && f->completed_value() == 5 && *f->value_address() == 5, "CPU signal");

    auto q2 = dev.create_queue(QueueType::compute, "waiter");
    auto g = dev.create_fence(0);
    CHECK(q2->wait(f.get(), 6) == Status::ok, "queue wait");
    CHECK(q2->signal(g.get(), 1) == Status::ok, "signal after wait");
    CHECK(g->wait_cpu(1, 50'000'000ull) == Status::timeout, "GPU wait holds the later signal");
    f->signal_cpu(6);
    CHECK(g->wait_cpu(1, 5'000'000'000ull) == Status::ok, "released after the CPU signal");
    std::printf("fences: signal/wait/on_completion/mirror/cross-queue wait ok\n");
}

void test_buffers_and_va(Device& dev) {
    auto q = dev.create_queue(QueueType::copy, "copy");
    BufferDesc up{4096, MemoryKind::upload, usage::copy, "upload"};
    BufferDesc local{4096, MemoryKind::device_local, usage::copy, "local"};
    BufferDesc back{4096, MemoryKind::readback, usage::copy, "readback"};
    auto a = dev.create_buffer(up), b = dev.create_buffer(local), c = dev.create_buffer(back);
    CHECK(a && b && c, "buffers");
    auto* p = static_cast<std::uint32_t*>(a->cpu_pointer());
    for (int i = 0; i < 1024; ++i) p[i] = 0x9e3779b9u * std::uint32_t(i + 1);
    CHECK(b->cpu_pointer() == nullptr, "device_local is not CPU visible");
    auto l = dev.create_command_list(QueueType::copy);
    l->copy_buffer(b.get(), 0, a.get(), 0, 4096);
    l->copy_buffer(c.get(), 0, b.get(), 0, 4096);
    CHECK(l->close() == Status::ok, "close");
    run_lists(*q, {l.get()});
    CHECK(q->wait_idle(5'000'000'000ull) == Status::ok, "idle");
    CHECK(std::memcmp(a->cpu_pointer(), c->cpu_pointer(), 4096) == 0, "upload -> local -> readback round trip");

    const std::uint64_t va = b->gpu_va();
    Buffer* found = nullptr;
    std::uint64_t off = 0;
    CHECK(dev.resolve_va(va + 100, &found, &off) && found == b.get() && off == 100, "resolve_va inside a buffer");
    CHECK(va != b->host_gpu_address(), "console VA differs from the Metal gpuAddress");
    CHECK(a->gpu_va() != reinterpret_cast<std::uintptr_t>(a->cpu_pointer()), "console VA differs from the CPU address");
    std::printf("va: buffer VA 0x%llx, Metal gpuAddress 0x%llx, CPU 0x%llx\n", (unsigned long long)a->gpu_va(),
                (unsigned long long)a->host_gpu_address(), (unsigned long long)reinterpret_cast<std::uintptr_t>(a->cpu_pointer()));
}

void test_arena(Device& dev) {
    auto q = dev.create_queue(QueueType::direct, "arena");
    HeapDesc hd;
    hd.size = 64ull << 20;
    hd.memory = MemoryKind::shared;
    hd.console_arena = true;
    hd.label = "arena";
    auto heap = dev.create_heap(hd);
    CHECK(heap && heap->arena(), "arena heap");
    if (!heap || !heap->arena()) return;
    const std::uint64_t X = 3ull << 20;
    BufferDesc bd{1ull << 20, MemoryKind::shared, usage::vertex, "placed"};
    auto placed = dev.create_placed_buffer(heap.get(), X, bd);
    CHECK(placed != nullptr, "placed buffer");
    if (!placed) return;
    const std::uint64_t base = heap->host_gpu_address();
    std::printf("arena: heap %llu MiB, arena gpuAddress 0x%llx, placed@+0x%llx gpuAddress 0x%llx (delta 0x%llx)\n",
                (unsigned long long)(heap->size() >> 20), (unsigned long long)base, (unsigned long long)X,
                (unsigned long long)placed->host_gpu_address(), (unsigned long long)(placed->host_gpu_address() - base));
    const char* sv = std::getenv("MTL_SHADER_VALIDATION");
    if (sv && *sv && *sv != '0') {
        const std::uint64_t low = (1ull << 44) - 1;
        std::printf("arena: MTL_SHADER_VALIDATION set: gpuAddress high bits are validation tags, comparing bits 0-43\n");
        CHECK((placed->host_gpu_address() & low) == ((base + X) & low), "placed gpuAddress low bits = arena + offset");
    } else {
        CHECK(placed->host_gpu_address() == base + X, "placed gpuAddress = arena gpuAddress + offset");
    }
    CHECK(placed->gpu_va() == heap->va_base() + X, "placed console VA = heap VA + offset");
    auto* arena_bytes = static_cast<std::uint8_t*>(heap->arena()->cpu_pointer());
    auto* placed_bytes = static_cast<std::uint8_t*>(placed->cpu_pointer());
    for (int i = 0; i < 256; ++i) placed_bytes[i] = std::uint8_t(i * 7 + 1);
    CHECK(std::memcmp(arena_bytes + X, placed_bytes, 256) == 0, "CPU writes through the placed buffer alias the arena");

    BufferDesc rb{256, MemoryKind::readback, usage::copy, "rb"};
    auto out = dev.create_buffer(rb);
    auto l = dev.create_command_list(QueueType::direct);
    l->copy_buffer(out.get(), 0, static_cast<Buffer*>(heap->arena()), X, 256);
    l->close();
    run_lists(*q, {l.get()});
    q->wait_idle(5'000'000'000ull);
    CHECK(std::memcmp(out->cpu_pointer(), placed_bytes, 256) == 0, "GPU reads the placed bytes through the arena");

    Buffer* found = nullptr;
    std::uint64_t off = 0;
    CHECK(dev.resolve_va(heap->va_base() + X + 16, &found, &off) && found == placed.get() && off == 16,
          "resolve_va prefers the placed buffer");
    CHECK(dev.resolve_va(heap->va_base() + (8ull << 20), &found, &off) && found == heap->arena() && off == (8ull << 20),
          "resolve_va falls back to the arena");

    TextureDesc td;
    td.format = format::r8g8b8a8_unorm;
    td.width = 256;
    td.height = 256;
    td.usage = usage::shader_read | usage::render_target;
    const AllocationInfo ai = dev.texture_allocation_info(td);
    HeapDesc th;
    th.size = 8ull << 20;
    th.memory = MemoryKind::device_local;
    auto theap = dev.create_heap(th);
    auto tex = dev.create_placed_texture(theap.get(), ai.alignment * 4, td);
    CHECK(tex != nullptr && tex->placed(), "placed texture at an aligned offset");
    std::printf("placed texture 256x256 RGBA8: size %llu alignment %llu\n", (unsigned long long)ai.size,
                (unsigned long long)ai.alignment);

    std::vector<Barrier> bars(2);
    bars[0].type = BarrierType::transition;
    bars[0].resource = placed.get();
    bars[0].before = 0x800;
    bars[0].after = 0x400;
    bars[1].type = BarrierType::transition;
    bars[1].resource = out.get();
    bars[1].before = 0x400;
    bars[1].after = 0x800;
    Barrier read_read;
    read_read.resource = out.get();
    read_read.before = 0x800;
    read_read.after = 0x800 | 0x80;
    const auto before = dev.stats();
    auto l2 = dev.create_command_list(QueueType::direct);
    l2->copy_buffer(out.get(), 0, placed.get(), 0, 64);
    l2->barrier(bars);
    l2->copy_buffer(out.get(), 64, placed.get(), 64, 64);
    l2->barrier(std::span<const Barrier>(&read_read, 1));
    l2->copy_buffer(out.get(), 128, placed.get(), 128, 64);
    l2->close();
    run_lists(*q, {l2.get()});
    q->wait_idle(5'000'000'000ull);
    const auto after = dev.stats();
    CHECK(after.barriers_fenced - before.barriers_fenced == 2, "placed and committed barriers are both fenced");
    CHECK(after.barriers_noop - before.barriers_noop == 1, "a read-only to read-only transition is a no-op");
    CHECK(after.blit_encoders - before.blit_encoders == 2, "only the fenced barrier split the blit encoder (%llu)",
          (unsigned long long)(after.blit_encoders - before.blit_encoders));
    CHECK(after.fence_joins - before.fence_joins == 2, "the execute boundary and the barrier each joined (%llu)",
          (unsigned long long)(after.fence_joins - before.fence_joins));
}

void test_values(Device& dev) {
    auto qa = dev.create_queue(QueueType::direct, "A");
    auto qb = dev.create_queue(QueueType::compute, "B");
    BufferDesc sd{4096, MemoryKind::shared, usage::copy, "values"};
    auto vals = dev.create_buffer(sd);
    std::memset(vals->cpu_pointer(), 0, 4096);
    auto l = dev.create_command_list(QueueType::direct);
    l->write_value(vals.get(), 0, 0x1122334455667788ull, 8);
    l->write_value(vals.get(), 8, 0xdeadbeefull, 4);
    l->close();
    run_lists(*qa, {l.get()});
    qa->wait_idle(5'000'000'000ull);
    const auto* v64 = static_cast<const std::uint64_t*>(vals->cpu_pointer());
    const auto* v32 = static_cast<const std::uint32_t*>(vals->cpu_pointer());
    CHECK(v64[0] == 0x1122334455667788ull, "write64 0x%llx", (unsigned long long)v64[0]);
    CHECK(v32[2] == 0xdeadbeefu && v32[3] == 0, "write32 0x%x 0x%x", v32[2], v32[3]);

    auto src = dev.create_buffer(sd);
    auto dst = dev.create_buffer(sd);
    std::memset(dst->cpu_pointer(), 0, 4096);
    for (int i = 0; i < 1024; ++i) static_cast<std::uint32_t*>(src->cpu_pointer())[i] = 0xabc00000u + i;
    auto waiter = dev.create_command_list(QueueType::direct);
    waiter->wait_value(vals.get(), 64, 5, 8, WaitCompare::greater_equal);
    waiter->copy_buffer(dst.get(), 0, src.get(), 0, 4096);
    waiter->close();
    run_lists(*qa, {waiter.get()});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(static_cast<std::uint32_t*>(dst->cpu_pointer())[0] == 0, "copy held behind wait64 (100 ms)");
    CHECK(qa->wait_idle(1'000'000ull) == Status::timeout, "queue not idle while held");
    const auto t0 = std::chrono::steady_clock::now();
    *reinterpret_cast<volatile std::uint64_t*>(static_cast<std::uint8_t*>(vals->cpu_pointer()) + 64) = 5;
    CHECK(qa->wait_idle(5'000'000'000ull) == Status::ok, "released by a CPU store");
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    CHECK(std::memcmp(dst->cpu_pointer(), src->cpu_pointer(), 4096) == 0, "copy ran after the wait");
    std::printf("wait64: released %.0f us after the CPU store\n", us);

    std::memset(dst->cpu_pointer(), 0, 4096);
    auto w2 = dev.create_command_list(QueueType::direct);
    w2->wait_value(vals.get(), 128, 7, 4, WaitCompare::equal);
    w2->copy_buffer(dst.get(), 0, src.get(), 0, 4096);
    w2->close();
    run_lists(*qa, {w2.get()});
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(static_cast<std::uint32_t*>(dst->cpu_pointer())[0] == 0, "second wait holds");
    auto writer = dev.create_command_list(QueueType::compute);
    writer->write_value(vals.get(), 128, 7, 4);
    writer->close();
    run_lists(*qb, {writer.get()});
    CHECK(qa->wait_idle(5'000'000'000ull) == Status::ok, "released by a write32 on another queue");
    CHECK(std::memcmp(dst->cpu_pointer(), src->cpu_pointer(), 4096) == 0, "copy after the cross-queue write");
}

void test_d24(Device& dev) {
    FormatInfo fi;
    CHECK(dev.describe_format(format::d24_unorm_s8_uint, fi) && fi.emulated && fi.depth && fi.stencil,
          "D24S8 described as emulated");
    std::printf("D24_UNORM_S8_UINT -> native %u (%s)\n", fi.native, fi.note);
    auto q = dev.create_queue(QueueType::direct, "d24");
    TextureDesc td;
    td.format = format::d24_unorm_s8_uint;
    td.width = 16;
    td.height = 16;
    td.usage = usage::depth_stencil | usage::shader_read;
    auto t = dev.create_texture(td);
    CHECK(t && t->native_format() == fi.native, "D24S8 texture created");
    if (!t) return;
    BufferDesc bd{16 * 16 * 4, MemoryKind::readback, usage::copy, "depth rb"};
    auto rb = dev.create_buffer(bd);
    auto l = dev.create_command_list(QueueType::direct);
    TargetView v{t.get(), 0, 0, 0};
    l->clear_depth_stencil(v, true, 0.3f, true, 0x5a);
    BufferTextureCopy c;
    c.buffer = rb.get();
    c.texture = t.get();
    c.row_pitch = 16 * 4;
    c.depth_plane = true;
    l->copy_texture_to_buffer(c);
    CHECK(l->close() == Status::ok, "close");
    run_lists(*q, {l.get()});
    CHECK(q->wait_idle(5'000'000'000ull) == Status::ok, "idle");
    float got;
    std::memcpy(&got, rb->cpu_pointer(), 4);
    const double code = std::nearbyint(0.3f * 16777215.0f);
    const float want = float(code / 16777215.0);
    std::uint32_t gb, wb, rb3;
    const float raw = 0.3f;
    std::memcpy(&gb, &got, 4);
    std::memcpy(&wb, &want, 4);
    std::memcpy(&rb3, &raw, 4);
    std::printf("D24 clear 0.3: read 0x%08x (%.9g), D24 code %.0f -> 0x%08x, unquantised 0.3f 0x%08x\n", gb, got, code,
                wb, rb3);
    CHECK(gb == wb, "cleared depth is the quantised D24 value");
}

}  // namespace

int main() {
    auto dev = gta6rt::metal::create_device();
    if (!dev) {
        std::printf("no Metal device\n");
        return 1;
    }
    test::print_caps(dev->caps());
    CHECK(dev->caps().argument_buffers_tier == 2, "argument buffers tier 2");
    test_fences(*dev);
    test_buffers_and_va(*dev);
    test_arena(*dev);
    test_values(*dev);
    test_d24(*dev);
    test::print_stats(dev->stats());
    return test::finish("metal_test_device");
}
