#pragma once

#include "shims.h"

#include <gta6rt/abi.h>

namespace rt::gdk {
// Registers every table with gta6rt_shims_register_dll as CODE_NATIVE exports, on any arch (the tests call it natively).
gta6rt_status register_all();
// Registers the classes CoCreateInstance serves (the audio device enumerator) with shims_core's COM shim.
void register_com_classes();
void for_each_entry(void (*fn)(void* user, const ::rt::shims::Entry& entry), void* user);
}
