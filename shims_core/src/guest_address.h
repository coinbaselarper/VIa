#pragma once

#include <string>

#include <gta6rt/abi.h>

namespace rt::shims {

// The guest-callable address of dll!name: a thunk for code (native or frame), the storage for data.
// With serve_stub, an unserved name gets the named logging thunk and *was_stub is set; without it GTA6RT_E_NOT_FOUND.
gta6rt_status guest_address(const std::string& dll_lower, const std::string& name, gta6rt_gva* out, bool serve_stub,
                            bool* was_stub = nullptr);

}  // namespace rt::shims
