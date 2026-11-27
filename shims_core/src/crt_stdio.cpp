// UCRT stdio imports. The guest's FILE* is the host's: the title only passes it back to these functions.
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "file_system.h"
#include "printf_engine.h"
#include "shims.h"
#include "util.h"

namespace rt::crt {
namespace {

FILE* WINAPI w_acrt_iob_func(uint32_t index) {
    switch (index) {
    case 0: return stdin;
    case 1: return stdout;
    case 2: return stderr;
    default: errno = EINVAL; return nullptr;
    }
}

int g_commode = 0;
int g_fmode = 0;
int* WINAPI w_p_commode() { return &g_commode; }
int WINAPI w_set_fmode(int mode) {
    g_fmode = mode;
    return 0;
}

// MSVC mode strings carry flags the host rejects: t/b/c/n/N/S/R/T/D and ",ccs=...".
std::string host_mode(const char* mode, bool& writes, bool& keeps) {
    std::string out;
    writes = keeps = false;
    for (const char* p = mode; p && *p && *p != ','; ++p) {
        switch (*p) {
        case 'r': out += 'r'; break;
        case 'w': out += 'w'; writes = true; break;
        case 'a': out += 'a'; writes = keeps = true; break;
        case '+': out += '+'; writes = true; keeps = true; break;
        case 'x': out += 'x'; break;
        default: break;
        }
    }
    if (out.empty()) out = "r";
    return out;
}

FILE* open_guest(const std::string& path, const char* mode) {
    bool writes, keeps;
    const std::string hm = host_mode(mode, writes, keeps);
    const fs::HostPath hp = writes ? fs::crt_write_path(path, keeps) : fs::crt_read_path(path);
    if (hp.error) {
        errno = hp.error == 3 || hp.error == 2 ? ENOENT : EACCES;
        return nullptr;
    }
    return ::fopen(hp.path.c_str(), hm.c_str());
}

FILE* WINAPI w_fopen(const char* path, const char* mode) {
    if (!path || !mode) {
        errno = EINVAL;
        return nullptr;
    }
    return open_guest(path, mode);
}

FILE* WINAPI w_wfopen(const char16_t* path, const char16_t* mode) {
    if (!path || !mode) {
        errno = EINVAL;
        return nullptr;
    }
    return open_guest(narrow(path), narrow(mode).c_str());
}

int WINAPI w_fclose(FILE* f) { return ::fclose(f); }
int WINAPI w_fflush(FILE* f) { return ::fflush(f); }
int WINAPI w_fgetc(FILE* f) { return ::fgetc(f); }
int WINAPI w_fputc(int c, FILE* f) { return ::fputc(c, f); }
int WINAPI w_ungetc(int c, FILE* f) { return ::ungetc(c, f); }
size_t WINAPI w_fread(void* buf, size_t size, size_t count, FILE* f) { return ::fread(buf, size, count, f); }
size_t WINAPI w_fwrite(const void* buf, size_t size, size_t count, FILE* f) { return ::fwrite(buf, size, count, f); }
int WINAPI w_fseek(FILE* f, int32_t offset, int origin) { return ::fseek(f, offset, origin); }
int WINAPI w_fseeki64(FILE* f, int64_t offset, int origin) { return ::fseeko(f, offset, origin); }
int32_t WINAPI w_ftell(FILE* f) {
    const long v = ::ftell(f);
    if (v > INT32_MAX) {
        errno = EINVAL;
        return -1;
    }
    return static_cast<int32_t>(v);
}
void WINAPI w_rewind(FILE* f) { ::rewind(f); }
int WINAPI w_fgetpos(FILE* f, int64_t* pos) {
    fpos_t p;
    if (::fgetpos(f, &p) != 0) return -1;
    *pos = static_cast<int64_t>(p);
    return 0;
}
int WINAPI w_fsetpos(FILE* f, const int64_t* pos) {
    fpos_t p = static_cast<fpos_t>(*pos);
    return ::fsetpos(f, &p);
}
int WINAPI w_setvbuf(FILE* f, char* buf, int mode, size_t size) {
    int host_mode = _IOFBF;
    if (mode == 0x0040) host_mode = _IOLBF;
    else if (mode == 0x0004) host_mode = _IONBF;
    else if (mode != 0) {
        errno = EINVAL;
        return -1;
    }
    return ::setvbuf(f, buf, host_mode, size);
}
FILE* WINAPI w_fsopen(const char* path, const char* mode, int) { return w_fopen(path, mode); }
FILE* WINAPI w_wfsopen(const char16_t* path, const char16_t* mode, int) { return w_wfopen(path, mode); }
int WINAPI w_fputs(const char* s, FILE* f) { return ::fputs(s, f) < 0 ? -1 : 0; }
int WINAPI w_ferror(FILE* f) { return ::ferror(f); }
int64_t WINAPI w_ftelli64(FILE* f) { return ::ftello(f); }
// "C" locale, narrow stream: one byte per wide character.
uint16_t WINAPI w_fgetwc(FILE* f) {
    const int c = ::fgetc(f);
    return c == EOF ? 0xffff : static_cast<uint16_t>(c);
}
uint16_t WINAPI w_fputwc(uint16_t c, FILE* f) {
    if (c > 0xff) {
        errno = EILSEQ;
        return 0xffff;
    }
    return ::fputc(c, f) == EOF ? 0xffff : c;
}
uint16_t WINAPI w_ungetwc(uint16_t c, FILE* f) {
    if (c == 0xffff || c > 0xff) return 0xffff;
    return ::ungetc(c, f) == EOF ? 0xffff : c;
}

// msvcp140's basic_filebuf reads and writes the UCRT FILE buffer through these pointers. The host FILE has another
// layout, so each stream gets its own always-empty buffer view: the filebuf then goes through fgetc/fputc/fwrite.
struct EmptyStreamBuffer {
    char* base = nullptr;
    char* ptr = nullptr;
    int count = 0;
};
int WINAPI w_get_stream_buffer_pointers(FILE* f, char*** base, char*** ptr, int** count) {
    static std::mutex m;
    static auto* views = new std::map<FILE*, EmptyStreamBuffer*>;
    if (!f) {
        errno = EINVAL;
        return EINVAL;
    }
    std::lock_guard lock(m);
    EmptyStreamBuffer*& v = (*views)[f];
    if (!v) v = new EmptyStreamBuffer;
    if (base) *base = &v->base;
    if (ptr) *ptr = &v->ptr;
    if (count) *count = &v->count;
    return 0;
}

void WINAPI w_lock_file(FILE* f) { ::flockfile(f); }
void WINAPI w_unlock_file(FILE* f) { ::funlockfile(f); }

constexpr uint64_t kStandardSnprintf = 0x2;
constexpr uint64_t kLegacyNullTermination = 0x1;
constexpr size_t kNoLimit = static_cast<size_t>(-2);

template <class C>
int format_to_wide(const C* fmt, const uint64_t* args, std::u16string& out) {
    if (!fmt) {
        errno = EINVAL;
        return -1;
    }
    if (!format_win64(fmt, SlotReader{args}, out)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

// Stores a UTF-16 result into a guest buffer of `capacity` code units under the vsnprintf-family contract.
int store_wide(std::u16string& text, char16_t* buffer, size_t capacity, size_t max_count, bool safe, uint64_t options) {
    const bool truncate = max_count == static_cast<size_t>(-1);
    const size_t len = text.size();
    if (!buffer) return static_cast<int>(len);
    if (capacity == 0) return -1;
    if (safe) {
        const size_t take = truncate ? (len < capacity - 1 ? len : capacity - 1) : (len < max_count ? len : max_count);
        if (take >= capacity) {
            buffer[0] = 0;
            errno = 34;
            return -1;
        }
        std::memcpy(buffer, text.data(), take * sizeof(char16_t));
        buffer[take] = 0;
        if (truncate && len > capacity - 1) return -1;
        return static_cast<int>(take);
    }
    if (options & kStandardSnprintf) {
        const size_t take = len < capacity - 1 ? len : capacity - 1;
        std::memcpy(buffer, text.data(), take * sizeof(char16_t));
        buffer[take] = 0;
        return static_cast<int>(len);
    }
    if (len < capacity) {
        std::memcpy(buffer, text.data(), len * sizeof(char16_t));
        buffer[len] = 0;
        return static_cast<int>(len);
    }
    std::memcpy(buffer, text.data(), capacity * sizeof(char16_t));
    if (options & kLegacyNullTermination) buffer[capacity - 1] = 0;
    return -1;
}

int store_narrow(const std::u16string& text16, char* buffer, size_t capacity, size_t max_count, bool safe, uint64_t options) {
    const std::string text = narrow(text16);
    const bool truncate = max_count == static_cast<size_t>(-1);
    const size_t len = text.size();
    if (!buffer) return static_cast<int>(len);
    if (capacity == 0) return -1;
    if (safe) {
        const size_t take = truncate ? (len < capacity - 1 ? len : capacity - 1) : (len < max_count ? len : max_count);
        if (take >= capacity) {
            buffer[0] = 0;
            errno = 34;
            return -1;
        }
        std::memcpy(buffer, text.data(), take);
        buffer[take] = 0;
        if (truncate && len > capacity - 1) return -1;
        return static_cast<int>(take);
    }
    if (options & kStandardSnprintf) {
        const size_t take = len < capacity - 1 ? len : capacity - 1;
        std::memcpy(buffer, text.data(), take);
        buffer[take] = 0;
        return static_cast<int>(len);
    }
    if (len < capacity) {
        std::memcpy(buffer, text.data(), len + 1);
        return static_cast<int>(len);
    }
    std::memcpy(buffer, text.data(), capacity);
    if (options & kLegacyNullTermination) buffer[capacity - 1] = 0;
    return -1;
}

int WINAPI w_vsnwprintf_s(uint64_t options, char16_t* buffer, size_t capacity, size_t max_count, const char16_t* fmt,
                          void* locale, const uint64_t* args) {
    (void)locale;
    std::u16string text;
    if (format_to_wide(fmt, args, text) < 0) return -1;
    return store_wide(text, buffer, capacity, max_count, true, options);
}

int WINAPI w_vswprintf(uint64_t options, char16_t* buffer, size_t capacity, const char16_t* fmt, void* locale,
                       const uint64_t* args) {
    (void)locale;
    std::u16string text;
    if (format_to_wide(fmt, args, text) < 0) return -1;
    return store_wide(text, buffer, capacity, static_cast<size_t>(-1), false, options);
}

int WINAPI w_vswprintf_s(uint64_t options, char16_t* buffer, size_t capacity, const char16_t* fmt, void* locale,
                         const uint64_t* args) {
    (void)locale;
    std::u16string text;
    if (format_to_wide(fmt, args, text) < 0) return -1;
    return store_wide(text, buffer, capacity, kNoLimit, true, options);
}

int WINAPI w_vsnprintf_s(uint64_t options, char* buffer, size_t capacity, size_t max_count, const char* fmt, void* locale,
                         const uint64_t* args) {
    (void)locale;
    std::u16string text;
    if (format_to_wide(fmt, args, text) < 0) return -1;
    return store_narrow(text, buffer, capacity, max_count, true, options);
}

int WINAPI w_vsprintf(uint64_t options, char* buffer, size_t capacity, const char* fmt, void* locale,
                      const uint64_t* args) {
    (void)locale;
    std::u16string text;
    if (format_to_wide(fmt, args, text) < 0) return -1;
    return store_narrow(text, buffer, capacity, static_cast<size_t>(-1), false, options);
}

int WINAPI w_vsprintf_s(uint64_t options, char* buffer, size_t capacity, const char* fmt, void* locale,
                        const uint64_t* args) {
    (void)locale;
    std::u16string text;
    if (format_to_wide(fmt, args, text) < 0) return -1;
    return store_narrow(text, buffer, capacity, kNoLimit, true, options);
}

int WINAPI w_vfprintf(uint64_t options, FILE* f, const char* fmt, void* locale, const uint64_t* args) {
    (void)options;
    (void)locale;
    std::u16string text;
    if (!f || format_to_wide(fmt, args, text) < 0) return -1;
    const std::string utf8 = narrow(text);
    if (::fwrite(utf8.data(), 1, utf8.size(), f) != utf8.size()) return -1;
    return static_cast<int>(utf8.size());
}

int WINAPI w_vfwprintf(uint64_t options, FILE* f, const char16_t* fmt, void* locale, const uint64_t* args) {
    (void)options;
    (void)locale;
    std::u16string text;
    if (!f || format_to_wide(fmt, args, text) < 0) return -1;
    const std::string utf8 = narrow(text);
    if (::fwrite(utf8.data(), 1, utf8.size(), f) != utf8.size()) return -1;
    return static_cast<int>(text.size());
}

}  // namespace
}  // namespace rt::crt

namespace {
using namespace rt::crt;
#define SR(n) SHIM("ucrtbase.dll", #n, w_##n)
}  // namespace

SHIM_TABLE(crt_stdio,
    SHIM("ucrtbase.dll", "__acrt_iob_func", w_acrt_iob_func), SHIM("ucrtbase.dll", "__p__commode", w_p_commode),
    SHIM("ucrtbase.dll", "_set_fmode", w_set_fmode), SHIM("ucrtbase.dll", "_fseeki64", w_fseeki64),
    SHIM("ucrtbase.dll", "_wfopen", w_wfopen), SHIM("ucrtbase.dll", "_lock_file", w_lock_file),
    SHIM("ucrtbase.dll", "_unlock_file", w_unlock_file), SR(fopen), SR(fclose), SR(fflush), SR(fgetc), SR(fgetpos),
    SR(fputc), SR(fread), SR(fseek), SR(fsetpos), SR(ftell), SR(fwrite), SR(rewind), SR(setvbuf), SR(ungetc),
    SHIM("ucrtbase.dll", "_fsopen", w_fsopen), SHIM("ucrtbase.dll", "_wfsopen", w_wfsopen), SR(fputs), SR(ferror),
    SHIM("ucrtbase.dll", "_ftelli64", w_ftelli64), SR(fgetwc), SR(fputwc), SR(ungetwc),
    SHIM("ucrtbase.dll", "_get_stream_buffer_pointers", w_get_stream_buffer_pointers),
    SHIM("ucrtbase.dll", "__stdio_common_vsnprintf_s", w_vsnprintf_s),
    SHIM("ucrtbase.dll", "__stdio_common_vsnwprintf_s", w_vsnwprintf_s),
    SHIM("ucrtbase.dll", "__stdio_common_vswprintf", w_vswprintf),
    SHIM("ucrtbase.dll", "__stdio_common_vswprintf_s", w_vswprintf_s),
    SHIM("ucrtbase.dll", "__stdio_common_vsprintf", w_vsprintf),
    SHIM("ucrtbase.dll", "__stdio_common_vsprintf_s", w_vsprintf_s),
    SHIM("ucrtbase.dll", "__stdio_common_vfprintf", w_vfprintf),
    SHIM("ucrtbase.dll", "__stdio_common_vfwprintf", w_vfwprintf),
);
