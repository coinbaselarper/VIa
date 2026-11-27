// Reduced copy of leonida/runtime/shims/startup_trace.h: only what the GDK shims call (enabled, event, quote, hex, where).
// The state variables keep their names and types so this header and the full one (open/close, the exception handler)
// can be linked into one program.
#pragma once


#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#include "shims.h"

namespace rt::startup_trace {

namespace detail {
inline std::atomic<bool> on{false};
// Intentionally immortal: vectored exception handlers or late teardown paths can still run after normal statics would
// have been destroyed.
inline std::mutex& mutex() {
    static auto* m = new std::mutex;
    return *m;
}
inline FILE* file = nullptr;
inline unsigned long long seq = 0;
inline std::chrono::steady_clock::time_point start;
inline void append_escaped(std::string& out, const char* s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += hex[c >> 4];
                    out += hex[c & 15];
                } else {
                    out += static_cast<char>(c);  // UTF-8 passes through
                }
        }
    }
}
}  // namespace detail

inline bool enabled() noexcept { return detail::on.load(std::memory_order_acquire); }

// A JSON string literal, quotes included.
inline std::string quote(const std::string& s) {
    std::string out = "\"";
    detail::append_escaped(out, s.data(), s.size());
    out += '"';
    return out;
}

inline std::string hex(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

// "module+0xRVA" for a mapped image or a host module, else the bare address.
inline std::string where(uint64_t address) { return shims::module_offset(address); }

// `fields` is the rest of the object, already JSON: `"module":"a.dll","result":1`. Empty is allowed.
inline void event(const char* kind, const std::string& fields = {}) {
    if (!enabled()) return;
    const std::lock_guard lock(detail::mutex());
    if (!detail::file) return;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - detail::start).count();
    std::fprintf(detail::file, "{\"seq\":%llu,\"t\":%.6f,\"kind\":\"%s\"%s%s}\n", ++detail::seq, t, kind,
                 fields.empty() ? "" : ",", fields.c_str());
    std::fflush(detail::file);
}

}  // namespace rt::startup_trace
