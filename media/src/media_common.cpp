#include "media_common.h"

#include <gta6rt/isa.h>

#include <mach/mach_time.h>

namespace rt::media {
namespace {

uint64_t isa_invoker(uint64_t fn, const uint64_t* args, uint32_t count) {
    gta6rt_guest_args a{};
    for (uint32_t i = 0; i < count && i < 16; ++i) a.args[i] = args[i];
    a.count = count;
    gta6rt_guest_ret r{};
    const gta6rt_status s = gta6rt_isa_call_guest(fn, &a, &r);
    if (s != GTA6RT_OK) {
        MLOG(ERROR, "media", "guest call to 0x%llx failed: %s", static_cast<unsigned long long>(fn),
             gta6rt_status_name(s));
        return 0;
    }
    return r.rax;
}

std::atomic<GuestInvoker> g_invoker{&isa_invoker};

}  // namespace

void set_guest_invoker(GuestInvoker invoker) { g_invoker.store(invoker ? invoker : &isa_invoker); }

uint64_t guest_call(uint64_t fn, const uint64_t* args, uint32_t count) { return g_invoker.load()(fn, args, count); }

uint64_t direct_guest_invoker(uint64_t fn, const uint64_t* a, uint32_t count) {
    using F = uint64_t(GTA6RT_GUEST_ABI*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    uint64_t v[6] = {};
    for (uint32_t i = 0; i < count && i < 6; ++i) v[i] = a[i];
    return reinterpret_cast<F>(fn)(v[0], v[1], v[2], v[3], v[4], v[5]);
}

namespace {
const mach_timebase_info_data_t g_timebase = [] {
    mach_timebase_info_data_t t{};
    mach_timebase_info(&t);
    return t;
}();
}  // namespace

uint64_t now_ns() {
    return static_cast<uint64_t>(static_cast<__uint128_t>(mach_absolute_time()) * g_timebase.numer /
                                 g_timebase.denom);
}

}  // namespace rt::media
