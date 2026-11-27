#pragma once
#include <malloc/malloc.h>
#include <stdlib.h>
#ifdef __cplusplus
extern "C++" {
inline size_t malloc_usable_size(void* p) { return malloc_size(p); }
inline void* memalign(size_t a, size_t s) { void* r = nullptr; return posix_memalign(&r, a < sizeof(void*) ? sizeof(void*) : a, s) == 0 ? r : nullptr; }
}
#endif
