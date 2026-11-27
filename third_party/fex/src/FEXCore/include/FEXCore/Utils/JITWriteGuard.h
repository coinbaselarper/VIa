// SPDX-License-Identifier: MIT
#pragma once

#if defined(__APPLE__) && defined(__aarch64__)
#include <pthread.h>

namespace FEXCore::Utils {
// MAP_JIT memory on Apple silicon is per-thread either writable or executable.
// Code writers hold this guard; nesting is counted so only the outermost scope toggles.
class JITWriteGuard final {
public:
  JITWriteGuard() {
    if (Depth()++ == 0) {
      pthread_jit_write_protect_np(0);
    }
  }
  ~JITWriteGuard() {
    if (--Depth() == 0) {
      pthread_jit_write_protect_np(1);
    }
  }
  JITWriteGuard(const JITWriteGuard&) = delete;
  JITWriteGuard& operator=(const JITWriteGuard&) = delete;

private:
  static int& Depth() {
    static thread_local int D = 0;
    return D;
  }
};
} // namespace FEXCore::Utils
#else
namespace FEXCore::Utils {
class JITWriteGuard final {
public:
  JITWriteGuard() = default;
};
} // namespace FEXCore::Utils
#endif
