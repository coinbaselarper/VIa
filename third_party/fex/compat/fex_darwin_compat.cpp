#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <pthread.h>
#include <signal.h>
#include <os/os_sync_wait_on_address.h>
#include <linux/futex.h>
#include "fex_darwin_compat.h"

namespace {
std::atomic<uint64_t> Counts[512];

void Unimplemented(const char* What, long A, long B) {
  fprintf(stderr, "[fex compat] %s %ld (0x%lx) unimplemented: ENOSYS\n", What, A, B);
}

long Futex(uint32_t* Addr, int Op, uint32_t Val, const timespec* Timeout, uint32_t Bitset) {
  const int Cmd = Op & FUTEX_CMD_MASK;
  if (Cmd == FUTEX_WAIT || Cmd == FUTEX_WAIT_BITSET) {
    uint64_t Ns = 0;
    if (Timeout) {
      if (Cmd == FUTEX_WAIT) {
        Ns = uint64_t(Timeout->tv_sec) * 1000000000ull + uint64_t(Timeout->tv_nsec);
      } else {
        // FUTEX_WAIT_BITSET takes an absolute deadline.
        timespec Now {};
        clock_gettime((Op & FUTEX_CLOCK_REALTIME) ? CLOCK_REALTIME : CLOCK_MONOTONIC, &Now);
        const int64_t Left = (int64_t(Timeout->tv_sec) - int64_t(Now.tv_sec)) * 1000000000ll + (int64_t(Timeout->tv_nsec) - int64_t(Now.tv_nsec));
        if (Left <= 0) {
          errno = ETIMEDOUT;
          return -1;
        }
        Ns = uint64_t(Left);
      }
    }
    const int R = Timeout ? os_sync_wait_on_address_with_timeout(Addr, Val, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE, OS_CLOCK_MACH_ABSOLUTE_TIME, Ns ? Ns : 1)
                          : os_sync_wait_on_address(Addr, Val, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE);
    if (R < 0 && errno == ETIMEDOUT) {
      return -1;
    }
    return 0;
  }
  if (Cmd == FUTEX_WAKE || Cmd == FUTEX_WAKE_BITSET) {
    // os_sync cannot pick waiters by bitset. FEX's WritePriorityMutex keeps readers and writers on one word and wakes
    // "one writer" by bitset; waking one arbitrary waiter can wake a reader instead and lose the writer's wake. So a
    // bitset-filtered wake wakes every waiter, which futex users must tolerate (they re-check the word).
    const bool One = Val == 1 && (Cmd == FUTEX_WAKE || Bitset == FUTEX_BITSET_MATCH_ANY);
    const int R = One ? os_sync_wake_by_address_any(Addr, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE)
                      : os_sync_wake_by_address_all(Addr, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE);
    return R < 0 ? 0 : 1;
  }
  Unimplemented("futex op", Op, Op);
  errno = ENOSYS;
  return -1;
}
} // namespace

extern "C" long fex_darwin_syscall(long Nr, ...) {
  va_list Ap;
  va_start(Ap, Nr);
  long Ret = -1;
  if (Nr >= FEX_DARWIN_SYS_BASE && Nr < FEX_DARWIN_SYS_BASE + 512) {
    Counts[Nr - FEX_DARWIN_SYS_BASE].fetch_add(1, std::memory_order_relaxed);
  }
  switch (Nr) {
  case SYS_futex: {
    auto Addr = va_arg(Ap, uint32_t*);
    int Op = va_arg(Ap, int);
    uint32_t Val = va_arg(Ap, uint32_t);
    auto Timeout = va_arg(Ap, const timespec*);
    uint32_t Bitset = FUTEX_BITSET_MATCH_ANY;
    const int Cmd = Op & FUTEX_CMD_MASK;
    if (Cmd == FUTEX_WAIT_BITSET || Cmd == FUTEX_WAKE_BITSET) {
      (void)va_arg(Ap, uint32_t*);
      Bitset = va_arg(Ap, uint32_t);
    }
    Ret = Futex(Addr, Op, Val, Timeout, Bitset);
    break;
  }
  case SYS_rt_sigprocmask: {
    int How = va_arg(Ap, int);
    auto Set = va_arg(Ap, const uint64_t*);
    auto Old = va_arg(Ap, uint64_t*);
    sigset_t S = Set ? sigset_t(*Set & 0xffffffffu) : 0, O = 0;
    Ret = pthread_sigmask(How, Set ? &S : nullptr, &O) == 0 ? 0 : -1;
    if (Old) {
      *Old = O;
    }
    break;
  }
  case SYS_gettid: {
    uint64_t Tid = 0;
    pthread_threadid_np(nullptr, &Tid);
    Ret = long(Tid);
    break;
  }
  case SYS_getcpu: {
    auto Cpu = va_arg(Ap, unsigned*);
    auto Node = va_arg(Ap, unsigned*);
    if (Cpu) *Cpu = 0;
    if (Node) *Node = 0;
    Ret = 0;
    break;
  }
  case SYS_getrandom: {
    auto Buf = va_arg(Ap, void*);
    size_t Len = va_arg(Ap, size_t);
    arc4random_buf(Buf, Len);
    Ret = long(Len);
    break;
  }
  default:
    if (Nr < FEX_DARWIN_SYS_BASE || Nr >= FEX_DARWIN_SYS_BASE + 512 || Counts[Nr - FEX_DARWIN_SYS_BASE].load(std::memory_order_relaxed) == 1) {
      Unimplemented("syscall", Nr - FEX_DARWIN_SYS_BASE, Nr);
    }
    errno = ENOSYS;
    Ret = -1;
    break;
  }
  va_end(Ap);
  return Ret;
}

extern "C" void fex_darwin_syscall_stats(void (*Cb)(const char*, uint64_t)) {
  static const struct { long Nr; const char* Name; } Names[] = {
    {SYS_futex, "futex"}, {SYS_rt_sigprocmask, "rt_sigprocmask"}, {SYS_gettid, "gettid"}, {SYS_tgkill, "tgkill"},
    {SYS_getcpu, "getcpu"}, {SYS_getrandom, "getrandom"}, {SYS_renameat2, "renameat2"}, {SYS_statx, "statx"},
    {SYS_pidfd_open, "pidfd_open"},
  };
  for (auto& N : Names) {
    Cb(N.Name, Counts[N.Nr - FEX_DARWIN_SYS_BASE].load());
  }
}
