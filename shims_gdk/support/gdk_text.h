#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace rt::gdk {

inline void append_utf8(std::string& out, uint32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

inline std::string to_utf8(std::u16string_view in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        uint32_t c = in[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < in.size() && in[i + 1] >= 0xDC00 && in[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (in[i + 1] - 0xDC00);
            ++i;
        } else if (c >= 0xD800 && c < 0xE000) {
            c = 0xFFFD;
        }
        append_utf8(out, c);
    }
    return out;
}

inline std::u16string to_u16(std::string_view in) {
    std::u16string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        const unsigned char b = static_cast<unsigned char>(in[i]);
        uint32_t c = b;
        size_t n = 1;
        if (b >= 0xF0) {
            n = 4;
        } else if (b >= 0xE0) {
            n = 3;
        } else if (b >= 0xC0) {
            n = 2;
        }
        if (n > 1) {
            if (i + n > in.size()) {
                out += u'\xFFFD';
                break;
            }
            c = b & (0xFF >> (n + 1));
            for (size_t k = 1; k < n; ++k) c = (c << 6) | (static_cast<unsigned char>(in[i + k]) & 0x3F);
        }
        i += n;
        if (c >= 0x10000) {
            c -= 0x10000;
            out += static_cast<char16_t>(0xD800 + (c >> 10));
            out += static_cast<char16_t>(0xDC00 + (c & 0x3FF));
        } else {
            out += static_cast<char16_t>(c);
        }
    }
    return out;
}

inline std::u16string u16_of(const char16_t* p) { return p ? std::u16string(p) : std::u16string(); }

}  // namespace rt::gdk
