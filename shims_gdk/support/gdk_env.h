#pragma once

#include <gta6rt/os.h>

#include <cstdlib>
#include <string>

namespace rt::gdk {

inline bool env_get(const char* name, std::string& out) {
    if (const char* v = std::getenv(name)) {
        out = v;
        return true;
    }
    if (const char* v = gta6rt_env_get(name)) {
        out = v;
        return true;
    }
    return false;
}

inline std::string env_or(const char* name, const std::string& fallback = {}) {
    std::string v;
    return env_get(name, v) ? v : fallback;
}

// The value when it is set, not empty and shorter than `capacity` (a buffer of that size with its terminator); else empty.
inline std::string env_bounded(const char* name, size_t capacity) {
    std::string v;
    return env_get(name, v) && !v.empty() && v.size() < capacity ? v : std::string();
}

inline bool env_is_set(const char* name) {
    std::string v;
    return env_get(name, v);
}

// BCP 47 name of the host's user locale ("en-US"), from the POSIX locale variables; empty when unset or "C".
inline std::string host_locale_name() {
    for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        std::string v;
        if (!env_get(var, v) || v.empty()) continue;
        v = v.substr(0, v.find_first_of(".@"));
        if (v == "C" || v == "POSIX") return {};
        for (char& c : v)
            if (c == '_') c = '-';
        return v;
    }
    return {};
}

}  // namespace rt::gdk
