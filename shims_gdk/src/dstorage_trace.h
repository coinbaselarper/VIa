#pragma once
#include "gdk_env.h"

namespace leonida::dstorage {
inline bool trace_enabled() {
    static const bool enabled = [] {
        return rt::gdk::env_or("LEONIDA_DSTORAGE_TRACE") == "1";
    }();
    return enabled;
}
} // namespace leonida::dstorage
