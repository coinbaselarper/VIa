// BCrypt RNG and algorithm provider shims (bcrypt.dll).
//
// game_scarlett_master.exe directly imports BCryptOpenAlgorithmProvider, BCryptGenRandom, and
// BCryptCloseAlgorithmProvider (all 3 imports of bcrypt.dll). This shim:
//   - serves the RNG algorithm only; any other algorithm id fails with STATUS_NOT_FOUND, as a host without that
//     provider would;
//   - tracks live BCRYPT_ALG_HANDLEs so double-close, null, or foreign handles fail safely with STATUS_INVALID_HANDLE;
//   - fills buffers from the os random source (arc4random-class), or from a deterministic splitmix64 stream when
//     LEONIDA_RNG_SEED=<uint64> (decimal or 0x hex) is set, for reproducible runs.
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <set>

#include <gta6rt/os.h>

#include "shims.h"
#include "util.h"

namespace rt::bcrypt_rng {
namespace {

constexpr int32_t kStatusSuccess = 0;
constexpr int32_t kStatusUnsuccessful = static_cast<int32_t>(0xC0000001u);
constexpr int32_t kStatusInvalidHandle = static_cast<int32_t>(0xC0000008u);
constexpr int32_t kStatusInvalidParameter = static_cast<int32_t>(0xC000000Du);
constexpr int32_t kStatusNoMemory = static_cast<int32_t>(0xC0000017u);
constexpr int32_t kStatusNotFound = static_cast<int32_t>(0xC0000225u);
constexpr uint32_t kUseSystemPreferredRng = 0x2;

struct AlgHandle {
    bool is_rng;
};

std::mutex g_lock;
std::set<AlgHandle*>& live() {
    static auto* s = new std::set<AlgHandle*>;
    return *s;
}

std::once_flag g_once;
bool g_seeded_mode = false;
uint64_t g_prng_state = 0;
std::atomic<bool> g_disable_rng_for_test{false};

bool parse_u64(const char* text, uint64_t& out) {
    if (!text || !*text) return false;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(text, &end, 0);
    if (!end || *end != '\0') return false;
    out = v;
    return true;
}

void ensure_initialized() {
    std::call_once(g_once, [] {
        uint64_t seed = 0;
        if (parse_u64(gta6rt_env_get("LEONIDA_RNG_SEED"), seed)) {
            g_seeded_mode = true;
            g_prng_state = seed ? seed : 0x9E3779B97F4A7C15ULL;
            RTLOG(INFO, "bcrypt: deterministic LEONIDA_RNG_SEED enabled (0x%llx)", static_cast<unsigned long long>(seed));
        }
    });
}

uint64_t next_splitmix64() {
    uint64_t z = (g_prng_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

void fill_seeded(uint8_t* buffer, uint32_t count) {
    std::lock_guard lock(g_lock);
    uint32_t offset = 0;
    while (offset < count) {
        const uint64_t word = next_splitmix64();
        const uint32_t chunk = (count - offset) < 8 ? (count - offset) : 8;
        std::memcpy(buffer + offset, &word, chunk);
        offset += chunk;
    }
}

int32_t WINAPI open_algorithm_provider(AlgHandle** out, const char16_t* alg_id, const char16_t* implementation, uint32_t flags) {
    (void)implementation; (void)flags;
    ensure_initialized();
    if (!out || !alg_id) return kStatusInvalidParameter;
    *out = nullptr;
    if (!u16ieq(alg_id, u"RNG")) {
        RTLOG(WARN, "bcrypt: algorithm %s is not served", narrow(alg_id).c_str());
        return kStatusNotFound;
    }
    auto* h = new (std::nothrow) AlgHandle{true};
    if (!h) return kStatusNoMemory;
    {
        std::lock_guard lock(g_lock);
        live().insert(h);
    }
    *out = h;
    return kStatusSuccess;
}

int32_t WINAPI close_algorithm_provider(AlgHandle* h, uint32_t flags) {
    (void)flags;
    if (!h) return kStatusInvalidHandle;
    {
        std::lock_guard lock(g_lock);
        if (live().erase(h) == 0) return kStatusInvalidHandle;
    }
    delete h;
    return kStatusSuccess;
}

int32_t WINAPI gen_random(AlgHandle* h, uint8_t* buffer, uint32_t count, uint32_t flags) {
    ensure_initialized();
    if (!buffer) return kStatusInvalidParameter;
    if (flags & kUseSystemPreferredRng) {
        if (h) return kStatusInvalidParameter;
    } else {
        std::lock_guard lock(g_lock);
        if (!h || live().count(h) == 0) return kStatusInvalidHandle;
    }
    if (count == 0) return kStatusSuccess;
    if (g_seeded_mode) {
        fill_seeded(buffer, count);
        return kStatusSuccess;
    }
    if (!g_disable_rng_for_test.load(std::memory_order_acquire) && gta6rt_random_bytes(buffer, count) == GTA6RT_OK)
        return kStatusSuccess;
    return kStatusUnsuccessful;
}

}  // namespace

void set_host_rng_enabled_for_test(bool enabled) { g_disable_rng_for_test.store(!enabled, std::memory_order_release); }

void reseed_for_test(const char* seed_text) {
    uint64_t seed = 0;
    std::lock_guard lock(g_lock);
    if (parse_u64(seed_text, seed)) {
        g_seeded_mode = true;
        g_prng_state = seed ? seed : 0x9E3779B97F4A7C15ULL;
    } else {
        g_seeded_mode = false;
    }
    std::call_once(g_once, [] {});
}

}  // namespace rt::bcrypt_rng

namespace {
using namespace rt::bcrypt_rng;
}

SHIM_TABLE(bcrypt_rng,
    SHIM("bcrypt.dll", "BCryptOpenAlgorithmProvider", open_algorithm_provider),
    SHIM("bcrypt.dll", "BCryptGenRandom", gen_random),
    SHIM("bcrypt.dll", "BCryptCloseAlgorithmProvider", close_algorithm_provider),
);
