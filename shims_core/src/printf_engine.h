// printf for guest format strings and the Win64 va_list (a pointer to 8-byte argument slots). Output is UTF-16;
// the narrow callers convert. Windows semantics where they differ from the host libc: `%p` is 16 upper-case hex
// digits, `l` is 32-bit, `%s` means "same width as the format string" and `%S` the other, `%n` is refused.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "util.h"

namespace rt::crt {

struct SlotReader {
    const uint64_t* p;
    uint64_t next() { return *p++; }
};

template <class C>
class FormatCursor {
public:
    explicit FormatCursor(const C* f) : f_(f) {}
    C peek() const { return f_[0]; }
    C get() { return *f_++; }
    bool done() const { return f_[0] == 0; }
    const C* pos() const { return f_; }

private:
    const C* f_;
};

inline void append_narrow(std::u16string& out, const char* s, int precision) {
    if (!s) {
        out += u"(null)";
        return;
    }
    size_t n = std::strlen(s);
    if (precision >= 0 && static_cast<size_t>(precision) < n) n = static_cast<size_t>(precision);
    out += widen(s, n);
}

inline void append_wide(std::u16string& out, const char16_t* s, int precision) {
    if (!s) {
        out += u"(null)";
        return;
    }
    size_t n = u16len(s);
    if (precision >= 0 && static_cast<size_t>(precision) < n) n = static_cast<size_t>(precision);
    out.append(s, n);
}

inline void pad_into(std::u16string& out, const std::u16string& body, int width, bool left, char16_t fill = u' ') {
    const size_t w = width > 0 ? static_cast<size_t>(width) : 0;
    if (body.size() >= w) {
        out += body;
    } else if (left) {
        out += body;
        out.append(w - body.size(), u' ');
    } else {
        out.append(w - body.size(), fill);
        out += body;
    }
}

template <class C>
bool format_win64(const C* fmt, SlotReader args, std::u16string& out) {
    constexpr bool kWideFmt = sizeof(C) == 2;
    FormatCursor<C> cur(fmt);
    while (!cur.done()) {
        const C c = cur.get();
        if (c != '%') {
            if constexpr (kWideFmt) out.push_back(static_cast<char16_t>(c));
            else {
                const char ch = static_cast<char>(c);
                // collect a run of narrow bytes so multi-byte UTF-8 survives
                std::string run(1, ch);
                while (!cur.done() && cur.peek() != '%') run.push_back(static_cast<char>(cur.get()));
                out += widen(run.c_str(), run.size());
            }
            continue;
        }
        if (cur.peek() == '%') {
            cur.get();
            out.push_back(u'%');
            continue;
        }
        bool left = false, plus = false, space = false, alt = false, zero = false;
        for (;; cur.get()) {
            const C f = cur.peek();
            if (f == '-') left = true;
            else if (f == '+') plus = true;
            else if (f == ' ') space = true;
            else if (f == '#') alt = true;
            else if (f == '0') zero = true;
            else break;
        }
        int width = -1, precision = -1;
        if (cur.peek() == '*') {
            cur.get();
            width = static_cast<int32_t>(args.next());
            if (width < 0) {
                left = true;
                width = -width;
            }
        } else if (cur.peek() >= '0' && cur.peek() <= '9') {
            width = 0;
            while (cur.peek() >= '0' && cur.peek() <= '9') width = width * 10 + (cur.get() - '0');
        }
        if (cur.peek() == '.') {
            cur.get();
            precision = 0;
            if (cur.peek() == '*') {
                cur.get();
                precision = static_cast<int32_t>(args.next());
                if (precision < 0) precision = -1;
            } else {
                while (cur.peek() >= '0' && cur.peek() <= '9') precision = precision * 10 + (cur.get() - '0');
            }
        }
        enum { kDefault, kH, kHH, kL, kLL, kPtr, k32, k64, kW, kLD } len = kDefault;
        for (;;) {
            const C m = cur.peek();
            if (m == 'h') {
                cur.get();
                if (cur.peek() == 'h') { cur.get(); len = kHH; } else len = kH;
            } else if (m == 'l') {
                cur.get();
                if (cur.peek() == 'l') { cur.get(); len = kLL; } else len = kL;
            } else if (m == 'L') {
                cur.get();
                len = kLD;
            } else if (m == 'z' || m == 'j' || m == 't') {
                cur.get();
                len = k64;
            } else if (m == 'w') {
                cur.get();
                len = kW;
            } else if (m == 'I') {
                cur.get();
                if (cur.peek() == '3') { cur.get(); if (cur.peek() == '2') cur.get(); len = k32; }
                else if (cur.peek() == '6') { cur.get(); if (cur.peek() == '4') cur.get(); len = k64; }
                else len = kPtr;
            } else break;
        }
        const C conv = cur.get();
        if (conv == 0) return false;
        std::u16string body;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
            const uint64_t raw = args.next();
            const bool is_signed = conv == 'd' || conv == 'i';
            long long sv = 0;
            unsigned long long uv = 0;
            switch (len) {
            case kHH: sv = static_cast<int8_t>(raw); uv = static_cast<uint8_t>(raw); break;
            case kH: sv = static_cast<int16_t>(raw); uv = static_cast<uint16_t>(raw); break;
            case kLL: case kPtr: case k64: sv = static_cast<int64_t>(raw); uv = raw; break;
            default: sv = static_cast<int32_t>(raw); uv = static_cast<uint32_t>(raw); break;
            }
            char spec[24];
            char* sp = spec;
            *sp++ = '%';
            if (left) *sp++ = '-';
            if (plus) *sp++ = '+';
            if (space) *sp++ = ' ';
            if (alt) *sp++ = '#';
            if (zero && !left && precision < 0) *sp++ = '0';
            *sp++ = '*';
            if (precision >= 0) { *sp++ = '.'; *sp++ = '*'; }
            *sp++ = 'l'; *sp++ = 'l';
            *sp++ = static_cast<char>(conv == 'i' ? 'd' : conv);
            *sp = 0;
            char buf[128];
            int n;
            if (precision >= 0) n = is_signed ? std::snprintf(buf, sizeof buf, spec, width < 0 ? 0 : width, precision, sv)
                                              : std::snprintf(buf, sizeof buf, spec, width < 0 ? 0 : width, precision, uv);
            else n = is_signed ? std::snprintf(buf, sizeof buf, spec, width < 0 ? 0 : width, sv)
                               : std::snprintf(buf, sizeof buf, spec, width < 0 ? 0 : width, uv);
            if (n < 0) return false;
            out += widen(buf, static_cast<size_t>(n < 127 ? n : 127));
            continue;
        }
        case 'p': {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%016llX", static_cast<unsigned long long>(args.next()));
            pad_into(out, widen(buf), width, left);
            continue;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            const uint64_t raw = args.next();
            double d;
            std::memcpy(&d, &raw, 8);
            char spec[24];
            char* sp = spec;
            *sp++ = '%';
            if (left) *sp++ = '-';
            if (plus) *sp++ = '+';
            if (space) *sp++ = ' ';
            if (alt) *sp++ = '#';
            if (zero && !left) *sp++ = '0';
            *sp++ = '*';
            if (precision >= 0) { *sp++ = '.'; *sp++ = '*'; }
            *sp++ = static_cast<char>(conv);
            *sp = 0;
            char buf[512];
            const int n = precision >= 0 ? std::snprintf(buf, sizeof buf, spec, width < 0 ? 0 : width, precision, d)
                                         : std::snprintf(buf, sizeof buf, spec, width < 0 ? 0 : width, d);
            if (n < 0) return false;
            out += widen(buf, static_cast<size_t>(n < 511 ? n : 511));
            continue;
        }
        case 'c': case 'C': {
            const uint64_t raw = args.next();
            bool wide_arg = kWideFmt ? conv == 'c' : conv == 'C';
            if (len == kL || len == kW) wide_arg = true;
            if (len == kH) wide_arg = false;
            if (wide_arg) body.push_back(static_cast<char16_t>(raw));
            else {
                const char ch = static_cast<char>(raw);
                body += widen(&ch, 1);
            }
            pad_into(out, body, width, left);
            continue;
        }
        case 's': case 'S': {
            const uint64_t raw = args.next();
            bool wide_arg = kWideFmt ? conv == 's' : conv == 'S';
            if (len == kL || len == kW) wide_arg = true;
            if (len == kH) wide_arg = false;
            const void* ptr = reinterpret_cast<const void*>(static_cast<uintptr_t>(raw));
            if (wide_arg) append_wide(body, static_cast<const char16_t*>(ptr), precision);
            else append_narrow(body, static_cast<const char*>(ptr), precision);
            pad_into(out, body, width, left);
            continue;
        }
        default:
            return false;
        }
    }
    return true;
}

}  // namespace rt::crt
