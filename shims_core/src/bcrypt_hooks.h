#pragma once

namespace rt::bcrypt_rng {

// Test seams: disable the os random source, or force the LEONIDA_RNG_SEED stream (null: back to the os source).
void set_host_rng_enabled_for_test(bool enabled);
void reseed_for_test(const char* seed_text);

}  // namespace rt::bcrypt_rng
