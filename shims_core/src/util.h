#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include <gta6rt/os.h>

#define RTLOG(level, ...) gta6rt_log(GTA6RT_LOG_##level, "shims", __VA_ARGS__)

namespace rt {

inline std::string lower_ascii(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}

inline char16_t lower_ascii(char16_t c) { return (c >= u'A' && c <= u'Z') ? static_cast<char16_t>(c + 32) : c; }

inline std::u16string widen(const char* utf8, size_t length = static_cast<size_t>(-1)) {
    std::u16string out;
    if (!utf8) return out;
    if (length == static_cast<size_t>(-1)) length = std::strlen(utf8);
    out.reserve(length);
    const auto* p = reinterpret_cast<const uint8_t*>(utf8);
    const auto* end = p + length;
    while (p < end) {
        uint32_t cp;
        int extra;
        if (*p < 0x80) { cp = *p; extra = 0; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; extra = 1; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; extra = 2; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; extra = 3; }
        else { out.push_back(0xFFFD); ++p; continue; }
        ++p;
        bool ok = true;
        for (int i = 0; i < extra; ++i, ++p) {
            if (p >= end || (*p & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (*p & 0x3F);
        }
        if (!ok || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { out.push_back(0xFFFD); continue; }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

inline std::string narrow(const char16_t* w, size_t length = static_cast<size_t>(-1)) {
    std::string out;
    if (!w) return out;
    if (length == static_cast<size_t>(-1)) {
        length = 0;
        while (w[length]) ++length;
    }
    out.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        uint32_t cp = w[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < length && w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (w[i + 1] - 0xDC00);
            ++i;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) { out.push_back(static_cast<char>(0xC0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

inline std::string narrow(const std::u16string& w) { return narrow(w.data(), w.size()); }

inline size_t u16len(const char16_t* s) {
    size_t n = 0;
    while (s && s[n]) ++n;
    return n;
}

inline int u16icmp(const char16_t* a, const char16_t* b) {
    for (;; ++a, ++b) {
        const char16_t x = lower_ascii(*a), y = lower_ascii(*b);
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}

inline bool u16ieq(const char16_t* a, const char16_t* b) { return u16icmp(a, b) == 0; }

}  // namespace rt
