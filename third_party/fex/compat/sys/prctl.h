#pragma once
#include <errno.h>
#ifdef __cplusplus
inline int prctl(int, ...) {
  errno = ENOSYS;
  return -1;
}
#endif
