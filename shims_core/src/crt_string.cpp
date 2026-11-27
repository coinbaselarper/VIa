// UCRT string, ctype and memory imports (api-ms-win-crt-string, vcruntime140). Guest strings are narrow UTF-8/ASCII or
// UTF-16 (char16_t): the host's wchar_t is 32-bit, so every wide function here is written over char16_t.
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>

#include "shims.h"
#include "util.h"

namespace rt::crt {
namespace {

constexpr int kEINVAL = 22;
constexpr int kERANGE = 34;
constexpr int kSTRUNCATE = 80;

int ascii_class(int c, int (*pred)(int)) { return (c >= 0 && c < 128) ? pred(c) : 0; }

int WINAPI w_isalnum(int c) { return ascii_class(c, ::isalnum); }
int WINAPI w_isalpha(int c) { return ascii_class(c, ::isalpha); }
int WINAPI w_isdigit(int c) { return ascii_class(c, ::isdigit); }
int WINAPI w_islower(int c) { return ascii_class(c, ::islower); }
int WINAPI w_isspace(int c) { return ascii_class(c, ::isspace); }
int WINAPI w_isxdigit(int c) { return ascii_class(c, ::isxdigit); }
int WINAPI w_tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int WINAPI w_toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

size_t WINAPI w_strlen(const char* s) { return std::strlen(s); }
int WINAPI w_strcmp(const char* a, const char* b) {
    const int r = std::strcmp(a, b);
    return r < 0 ? -1 : (r > 0 ? 1 : 0);
}
int WINAPI w_strncmp(const char* a, const char* b, size_t n) {
    const int r = std::strncmp(a, b, n);
    return r < 0 ? -1 : (r > 0 ? 1 : 0);
}
int WINAPI w_stricmp(const char* a, const char* b) {
    for (;; ++a, ++b) {
        const int x = w_tolower(static_cast<unsigned char>(*a)), y = w_tolower(static_cast<unsigned char>(*b));
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}
int WINAPI w_strnicmp(const char* a, const char* b, size_t n) {
    for (; n; --n, ++a, ++b) {
        const int x = w_tolower(static_cast<unsigned char>(*a)), y = w_tolower(static_cast<unsigned char>(*b));
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
    return 0;
}
char* WINAPI w_strcat(char* d, const char* s) { return std::strcat(d, s); }
char* WINAPI w_strcpy(char* d, const char* s) { return std::strcpy(d, s); }
char* WINAPI w_strncat(char* d, const char* s, size_t n) { return std::strncat(d, s, n); }
char* WINAPI w_strncpy(char* d, const char* s, size_t n) { return std::strncpy(d, s, n); }
size_t WINAPI w_strcspn(const char* s, const char* r) { return std::strcspn(s, r); }
char* WINAPI w_strpbrk(const char* s, const char* r) { return const_cast<char*>(std::strpbrk(s, r)); }
char* WINAPI w_strchr(const char* s, int c) { return const_cast<char*>(std::strchr(s, c)); }
char* WINAPI w_strrchr(const char* s, int c) { return const_cast<char*>(std::strrchr(s, c)); }
char* WINAPI w_strstr(const char* s, const char* n) { return const_cast<char*>(std::strstr(s, n)); }

char* WINAPI w_strtok(char* s, const char* delim) {
    static thread_local char* state;
    return ::strtok_r(s, delim, &state);
}
char* WINAPI w_strtok_s(char* s, const char* delim, char** context) {
    if (!delim || !context || (!s && !*context)) {
        errno = kEINVAL;
        return nullptr;
    }
    return ::strtok_r(s, delim, context);
}

int WINAPI w_strcpy_s(char* d, size_t size, const char* s) {
    if (!d || size == 0) return kEINVAL;
    if (!s) {
        d[0] = 0;
        return kEINVAL;
    }
    const size_t n = std::strlen(s);
    if (n >= size) {
        d[0] = 0;
        return kERANGE;
    }
    std::memmove(d, s, n + 1);
    return 0;
}

int WINAPI w_strcat_s(char* d, size_t size, const char* s) {
    if (!d || size == 0) return kEINVAL;
    const size_t dl = strnlen(d, size);
    if (dl == size) {
        d[0] = 0;
        return kEINVAL;
    }
    if (!s) {
        d[0] = 0;
        return kEINVAL;
    }
    const size_t n = std::strlen(s);
    if (dl + n >= size) {
        d[0] = 0;
        return kERANGE;
    }
    std::memmove(d + dl, s, n + 1);
    return 0;
}

int WINAPI w_strncpy_s(char* d, size_t size, const char* s, size_t count) {
    if (count == 0 && !d && size == 0) return 0;
    if (!d || size == 0) return kEINVAL;
    if (count == 0) {
        d[0] = 0;
        return 0;
    }
    if (!s) {
        d[0] = 0;
        return kEINVAL;
    }
    if (count == static_cast<size_t>(-1)) {
        const size_t n = strnlen(s, size - 1);
        std::memmove(d, s, n);
        d[n] = 0;
        return s[n] == 0 ? 0 : kSTRUNCATE;
    }
    const size_t n = strnlen(s, count);
    if (n >= size) {
        d[0] = 0;
        return kERANGE;
    }
    std::memmove(d, s, n);
    d[n] = 0;
    return 0;
}

void* WINAPI w_memchr(const void* p, int c, size_t n) { return const_cast<void*>(std::memchr(p, c, n)); }
int WINAPI w_memcmp(const void* a, const void* b, size_t n) { return std::memcmp(a, b, n); }
void* WINAPI w_memcpy(void* d, const void* s, size_t n) { return std::memcpy(d, s, n); }
void* WINAPI w_memmove(void* d, const void* s, size_t n) { return std::memmove(d, s, n); }
void* WINAPI w_memset(void* d, int c, size_t n) { return std::memset(d, c, n); }

size_t WINAPI w_wcslen(const char16_t* s) { return u16len(s); }
int WINAPI w_wcscmp(const char16_t* a, const char16_t* b) {
    for (;; ++a, ++b) {
        if (*a != *b) return *a < *b ? -1 : 1;
        if (!*a) return 0;
    }
}
int WINAPI w_wcsicmp_core(const char16_t* a, const char16_t* b, size_t n) {
    for (; n; --n, ++a, ++b) {
        const char16_t x = lower_ascii(*a), y = lower_ascii(*b);
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
    return 0;
}
int WINAPI w_wcsnicmp(const char16_t* a, const char16_t* b, size_t n) { return w_wcsicmp_core(a, b, n); }
int WINAPI w_wcsicoll(const char16_t* a, const char16_t* b) { return u16icmp(a, b); }
int WINAPI w_wcscoll(const char16_t* a, const char16_t* b) { return w_wcscmp(a, b); }

char16_t* WINAPI w_wcsncpy(char16_t* d, const char16_t* s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; ++i) d[i] = s[i];
    for (; i < n; ++i) d[i] = 0;
    return d;
}

int WINAPI w_wcscat_s(char16_t* d, size_t size, const char16_t* s) {
    if (!d || size == 0) return kEINVAL;
    size_t dl = 0;
    while (dl < size && d[dl]) ++dl;
    if (dl == size || !s) {
        d[0] = 0;
        return kEINVAL;
    }
    const size_t n = u16len(s);
    if (dl + n >= size) {
        d[0] = 0;
        return kERANGE;
    }
    std::memmove(d + dl, s, (n + 1) * sizeof(char16_t));
    return 0;
}

char16_t* WINAPI w_wcsrchr(const char16_t* s, char16_t c) {
    const char16_t* last = nullptr;
    for (;; ++s) {
        if (*s == c) last = s;
        if (!*s) break;
    }
    return const_cast<char16_t*>(last);
}

}  // namespace
}  // namespace rt::crt

namespace {
using namespace rt::crt;

#define STR_ROWS(dll)                                                                                           \
    SHIM(dll, "_stricmp", w_stricmp), SHIM(dll, "_strnicmp", w_strnicmp), SHIM(dll, "_wcsicoll", w_wcsicoll),    \
    SHIM(dll, "_wcsnicmp", w_wcsnicmp), SHIM(dll, "isalnum", w_isalnum), SHIM(dll, "isalpha", w_isalpha),       \
    SHIM(dll, "isdigit", w_isdigit), SHIM(dll, "islower", w_islower), SHIM(dll, "isspace", w_isspace),          \
    SHIM(dll, "isxdigit", w_isxdigit), SHIM(dll, "strcat", w_strcat), SHIM(dll, "strcat_s", w_strcat_s),        \
    SHIM(dll, "strcmp", w_strcmp), SHIM(dll, "strcpy", w_strcpy), SHIM(dll, "strcpy_s", w_strcpy_s),            \
    SHIM(dll, "strcspn", w_strcspn), SHIM(dll, "strlen", w_strlen), SHIM(dll, "strncat", w_strncat),            \
    SHIM(dll, "strncmp", w_strncmp), SHIM(dll, "strncpy", w_strncpy), SHIM(dll, "strncpy_s", w_strncpy_s),      \
    SHIM(dll, "strpbrk", w_strpbrk), SHIM(dll, "strtok", w_strtok), SHIM(dll, "strtok_s", w_strtok_s),          \
    SHIM(dll, "tolower", w_tolower), SHIM(dll, "toupper", w_toupper), SHIM(dll, "wcscat_s", w_wcscat_s),        \
    SHIM(dll, "wcscmp", w_wcscmp), SHIM(dll, "wcscoll", w_wcscoll), SHIM(dll, "wcslen", w_wcslen),              \
    SHIM(dll, "wcsncpy", w_wcsncpy), SHIM(dll, "strchr", w_strchr), SHIM(dll, "strrchr", w_strrchr),            \
    SHIM(dll, "strstr", w_strstr), SHIM(dll, "wcsrchr", w_wcsrchr)

#define MEM_ROWS(dll)                                                                                      \
    SHIM(dll, "memchr", w_memchr), SHIM(dll, "memcmp", w_memcmp), SHIM(dll, "memcpy", w_memcpy),        \
    SHIM(dll, "memmove", w_memmove), SHIM(dll, "memset", w_memset)

}  // namespace

SHIM_TABLE(crt_string,
    STR_ROWS("ucrtbase.dll"),
    MEM_ROWS("ucrtbase.dll"),
    MEM_ROWS("vcruntime140.dll"),
    SHIM("vcruntime140.dll", "strchr", w_strchr),
    SHIM("vcruntime140.dll", "strrchr", w_strrchr),
    SHIM("vcruntime140.dll", "strstr", w_strstr),
    SHIM("vcruntime140.dll", "wcsrchr", w_wcsrchr),
);
