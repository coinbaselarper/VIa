#pragma once
#ifdef __APPLE__
#include <sys/mman.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <limits.h>
#include <math.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0
#endif
#ifndef MAP_32BIT
#define MAP_32BIT 0
#endif
#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 0
#define MADV_NOHUGEPAGE 0
#endif

#define FEX_DARWIN_SYS_BASE 0x10000
#undef SYS_futex
#define SYS_futex (FEX_DARWIN_SYS_BASE + 202)
#undef SYS_rt_sigprocmask
#define SYS_rt_sigprocmask (FEX_DARWIN_SYS_BASE + 14)
#undef SYS_gettid
#define SYS_gettid (FEX_DARWIN_SYS_BASE + 186)
#undef SYS_tgkill
#define SYS_tgkill (FEX_DARWIN_SYS_BASE + 234)
#undef SYS_getcpu
#define SYS_getcpu (FEX_DARWIN_SYS_BASE + 309)
#undef SYS_getrandom
#define SYS_getrandom (FEX_DARWIN_SYS_BASE + 318)
#undef SYS_renameat2
#define SYS_renameat2 (FEX_DARWIN_SYS_BASE + 316)
#undef SYS_statx
#define SYS_statx (FEX_DARWIN_SYS_BASE + 332)
#undef SYS_pidfd_open
#define SYS_pidfd_open (FEX_DARWIN_SYS_BASE + 434)

#ifdef __cplusplus
extern "C" {
#endif
long fex_darwin_syscall(long nr, ...);
#ifdef __cplusplus
}
#endif
#define syscall(...) fex_darwin_syscall(__VA_ARGS__)

#ifndef MADV_DONTDUMP
#define MADV_DONTDUMP MADV_NORMAL
#endif

#ifdef __cplusplus
#include <errno.h>
static inline long getdents64(int, void*, size_t) {
  errno = ENOSYS;
  return -1;
}
static inline void sincos(double x, double* s, double* c) { __sincos(x, s, c); }
#endif
#endif
