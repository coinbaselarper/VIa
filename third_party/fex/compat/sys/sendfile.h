#pragma once
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#ifdef __cplusplus
inline ssize_t sendfile(int Out, int In, off_t* Offset, size_t Count) {
  char Buf[65536];
  size_t Done = 0;
  if (Offset) {
    lseek(In, *Offset, SEEK_SET);
  }
  while (Done < Count) {
    size_t Want = Count - Done < sizeof(Buf) ? Count - Done : sizeof(Buf);
    ssize_t R = read(In, Buf, Want);
    if (R <= 0) {
      break;
    }
    if (write(Out, Buf, size_t(R)) != R) {
      return -1;
    }
    Done += size_t(R);
  }
  if (Offset) {
    *Offset += off_t(Done);
  }
  return ssize_t(Done);
}
#endif
