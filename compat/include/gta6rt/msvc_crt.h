#pragma once

/*
 * Header-only replacements for the MSVC CRT extensions leonida uses (_s functions, _str*, _w* on UTF-16).
 * These are host-side helpers; the guest's own CRT imports (ucrtbase, vcruntime) are bound by shims_core.
 */

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/_types/_errno_t.h>
#include <gta6rt/winnt_base.h>

#define _TRUNCATE ((size_t)-1)

static inline size_t gta6rt_u16len(const WCHAR* s) {
    size_t n = 0;
    while (s[n]) ++n;
    return n;
}

/* UTF-16 -> UTF-8 into a caller buffer; returns bytes written excluding NUL, or (size_t)-1 if it does not fit. */
static inline size_t gta6rt_u16_to_utf8(const WCHAR* in, char* out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; in[i]; ++i) {
        uint32_t c = in[i];
        if (c >= 0xD800 && c <= 0xDBFF && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (in[i + 1] - 0xDC00);
            ++i;
        }
        char b[4];
        size_t n;
        if (c < 0x80) {
            b[0] = (char)c;
            n = 1;
        } else if (c < 0x800) {
            b[0] = (char)(0xC0 | (c >> 6));
            b[1] = (char)(0x80 | (c & 0x3F));
            n = 2;
        } else if (c < 0x10000) {
            b[0] = (char)(0xE0 | (c >> 12));
            b[1] = (char)(0x80 | ((c >> 6) & 0x3F));
            b[2] = (char)(0x80 | (c & 0x3F));
            n = 3;
        } else {
            b[0] = (char)(0xF0 | (c >> 18));
            b[1] = (char)(0x80 | ((c >> 12) & 0x3F));
            b[2] = (char)(0x80 | ((c >> 6) & 0x3F));
            b[3] = (char)(0x80 | (c & 0x3F));
            n = 4;
        }
        if (o + n >= cap) return (size_t)-1;
        memcpy(out + o, b, n);
        o += n;
    }
    if (o >= cap) return (size_t)-1;
    out[o] = 0;
    return o;
}

/* UTF-8 -> UTF-16; returns code units written excluding NUL, or (size_t)-1 if it does not fit. */
static inline size_t gta6rt_utf8_to_u16(const char* in, WCHAR* out, size_t cap) {
    size_t o = 0;
    const unsigned char* s = (const unsigned char*)in;
    while (*s) {
        uint32_t c = *s++;
        if (c >= 0xF0 && s[0] && s[1] && s[2]) {
            c = ((c & 0x07) << 18) | ((uint32_t)(s[0] & 0x3F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
            s += 3;
        } else if (c >= 0xE0 && s[0] && s[1]) {
            c = ((c & 0x0F) << 12) | ((uint32_t)(s[0] & 0x3F) << 6) | (s[1] & 0x3F);
            s += 2;
        } else if (c >= 0xC0 && s[0]) {
            c = ((c & 0x1F) << 6) | (s[0] & 0x3F);
            s += 1;
        }
        if (c >= 0x10000) {
            if (o + 2 >= cap) return (size_t)-1;
            c -= 0x10000;
            out[o++] = (WCHAR)(0xD800 + (c >> 10));
            out[o++] = (WCHAR)(0xDC00 + (c & 0x3FF));
        } else {
            if (o + 1 >= cap) return (size_t)-1;
            out[o++] = (WCHAR)c;
        }
    }
    if (o >= cap) return (size_t)-1;
    out[o] = 0;
    return o;
}

static inline errno_t getenv_s(size_t* required, char* buf, size_t cap, const char* name) {
    const char* v = getenv(name);
    if (!v) {
        if (required) *required = 0;
        if (buf && cap) buf[0] = 0;
        return 0;
    }
    size_t n = strlen(v) + 1;
    if (required) *required = n;
    if (!buf || cap == 0) return 0;
    if (n > cap) {
        buf[0] = 0;
        return ERANGE;
    }
    memcpy(buf, v, n);
    return 0;
}

static inline errno_t _dupenv_s(char** out, size_t* len, const char* name) {
    const char* v = getenv(name);
    *out = NULL;
    if (len) *len = 0;
    if (!v) return 0;
    *out = strdup(v);
    if (!*out) return ENOMEM;
    if (len) *len = strlen(v) + 1;
    return 0;
}

static inline errno_t _putenv_s(const char* name, const char* value) {
    int rc = (value && value[0]) ? setenv(name, value, 1) : unsetenv(name);
    return rc == 0 ? 0 : errno;
}

static inline errno_t _wgetenv_s(size_t* required, WCHAR* buf, size_t cap, const WCHAR* name) {
    char key[512];
    if (gta6rt_u16_to_utf8(name, key, sizeof key) == (size_t)-1) return EINVAL;
    const char* v = getenv(key);
    if (!v) {
        if (required) *required = 0;
        if (buf && cap) buf[0] = 0;
        return 0;
    }
    WCHAR tmp[4096];
    size_t n = gta6rt_utf8_to_u16(v, tmp, 4096);
    if (n == (size_t)-1) return ERANGE;
    if (required) *required = n + 1;
    if (!buf || cap == 0) return 0;
    if (n + 1 > cap) {
        buf[0] = 0;
        return ERANGE;
    }
    memcpy(buf, tmp, (n + 1) * sizeof(WCHAR));
    return 0;
}

static inline errno_t _wputenv_s(const WCHAR* name, const WCHAR* value) {
    char k[512], v[4096];
    if (gta6rt_u16_to_utf8(name, k, sizeof k) == (size_t)-1) return EINVAL;
    if (gta6rt_u16_to_utf8(value, v, sizeof v) == (size_t)-1) return EINVAL;
    return _putenv_s(k, v);
}

static inline errno_t fopen_s(FILE** f, const char* path, const char* mode) {
    *f = fopen(path, mode);
    return *f ? 0 : errno;
}

static inline errno_t _wfopen_s(FILE** f, const WCHAR* path, const WCHAR* mode) {
    char p[4096], m[32];
    *f = NULL;
    if (gta6rt_u16_to_utf8(path, p, sizeof p) == (size_t)-1) return ENAMETOOLONG;
    if (gta6rt_u16_to_utf8(mode, m, sizeof m) == (size_t)-1) return EINVAL;
    char* w = m;
    for (const char* c = m; *c; ++c)
        if (*c != 't') *w++ = *c;
    *w = 0;
    return fopen_s(f, p, m);
}

static inline int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
static inline int _strnicmp(const char* a, const char* b, size_t n) { return strncasecmp(a, b, n); }
static inline char* _strdup(const char* s) { return strdup(s); }
static inline int _wcsicmp(const WCHAR* a, const WCHAR* b) {
    for (;; ++a, ++b) {
        WCHAR x = (*a < 0x80) ? (WCHAR)tolower(*a) : *a;
        WCHAR y = (*b < 0x80) ? (WCHAR)tolower(*b) : *b;
        if (x != y || !x) return (int)x - (int)y;
    }
}
static inline int _wcsnicmp(const WCHAR* a, const WCHAR* b, size_t n) {
    for (; n; --n, ++a, ++b) {
        WCHAR x = (*a < 0x80) ? (WCHAR)tolower(*a) : *a;
        WCHAR y = (*b < 0x80) ? (WCHAR)tolower(*b) : *b;
        if (x != y || !x) return (int)x - (int)y;
    }
    return 0;
}

static inline errno_t strcpy_s(char* dst, size_t cap, const char* src) {
    size_t n = strlen(src);
    if (!dst || cap == 0) return EINVAL;
    if (n >= cap) {
        dst[0] = 0;
        return ERANGE;
    }
    memcpy(dst, src, n + 1);
    return 0;
}

static inline errno_t strncpy_s(char* dst, size_t cap, const char* src, size_t count) {
    if (!dst || cap == 0) return EINVAL;
    size_t n = strnlen(src, count == _TRUNCATE ? cap - 1 : count);
    if (n >= cap) {
        if (count == _TRUNCATE) n = cap - 1;
        else {
            dst[0] = 0;
            return ERANGE;
        }
    }
    memcpy(dst, src, n);
    dst[n] = 0;
    return 0;
}

static inline errno_t strcat_s(char* dst, size_t cap, const char* src) {
    size_t d = strnlen(dst, cap);
    if (d == cap) return EINVAL;
    return strcpy_s(dst + d, cap - d, src);
}

static inline errno_t memcpy_s(void* dst, size_t cap, const void* src, size_t n) {
    if (n > cap) return ERANGE;
    memcpy(dst, src, n);
    return 0;
}

static inline errno_t memmove_s(void* dst, size_t cap, const void* src, size_t n) {
    if (n > cap) return ERANGE;
    memmove(dst, src, n);
    return 0;
}

#ifdef __cplusplus
static inline int sprintf_s(char* buf, size_t cap, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
static inline int sprintf_s(char* buf, size_t cap, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return r;
}
static inline int vsprintf_s(char* buf, size_t cap, const char* fmt, va_list ap) { return vsnprintf(buf, cap, fmt, ap); }
extern "C++" {
template <size_t N> inline int sprintf_s(char (&buf)[N], const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, N, fmt, ap);
    va_end(ap);
    return r;
}
template <size_t N> inline int vsprintf_s(char (&buf)[N], const char* fmt, va_list ap) { return vsnprintf(buf, N, fmt, ap); }
template <size_t N> inline errno_t getenv_s(size_t* required, char (&buf)[N], const char* name) {
    return getenv_s(required, buf, N, name);
}
template <size_t N> inline errno_t _wgetenv_s(size_t* required, WCHAR (&buf)[N], const WCHAR* name) {
    return _wgetenv_s(required, buf, N, name);
}
template <size_t N> inline errno_t strcpy_s(char (&dst)[N], const char* src) { return strcpy_s(dst, N, src); }
template <size_t N> inline errno_t strcat_s(char (&dst)[N], const char* src) { return strcat_s(dst, N, src); }
template <size_t N> inline errno_t strncpy_s(char (&dst)[N], const char* src, size_t count) {
    return strncpy_s(dst, N, src, count);
}
}
#else
#define sprintf_s snprintf
#define vsprintf_s vsnprintf
#endif
#define _snprintf snprintf
#define _vsnprintf vsnprintf
#define _snprintf_s(buf, cap, count, ...) snprintf((buf), (cap), __VA_ARGS__)
#define _vsnprintf_s(buf, cap, count, fmt, ap) vsnprintf((buf), (cap), (fmt), (ap))
#define sscanf_s sscanf
#define _strtoui64 strtoull
#define _strtoi64 strtoll
#define _atoi64 atoll
#define _fseeki64 fseeko
#define _ftelli64 ftello

static inline void* _aligned_malloc(size_t size, size_t alignment) {
    void* p = NULL;
    if (alignment < sizeof(void*)) alignment = sizeof(void*);
    return posix_memalign(&p, alignment, size ? size : 1) == 0 ? p : NULL;
}
static inline void _aligned_free(void* p) { free(p); }

static inline unsigned short _byteswap_ushort(unsigned short v) { return __builtin_bswap16(v); }
static inline unsigned int _byteswap_ulong(unsigned int v) { return __builtin_bswap32(v); }
static inline unsigned long long _byteswap_uint64(unsigned long long v) { return __builtin_bswap64(v); }
#ifndef _rotl
static inline unsigned int _rotl(unsigned int v, int s) { return (v << (s & 31)) | (v >> ((32 - s) & 31)); }
#endif
#ifndef _rotr
static inline unsigned int _rotr(unsigned int v, int s) { return (v >> (s & 31)) | (v << ((32 - s) & 31)); }
#endif
static inline unsigned long long _rotl64(unsigned long long v, int s) { return (v << (s & 63)) | (v >> ((64 - s) & 63)); }
static inline unsigned long long _rotr64(unsigned long long v, int s) { return (v >> (s & 63)) | (v << ((64 - s) & 63)); }
