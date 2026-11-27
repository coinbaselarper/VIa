// The part of shims_core's com_shim.h that shims_gdk calls (the class registry behind CoCreateInstance).
#pragma once

#include <cstdint>

namespace rt::com_shim {

struct Guid {
    uint32_t data1;
    uint16_t data2, data3;
    uint8_t data4[8];
};
static_assert(sizeof(Guid) == 16);

using ClassFactoryFn = int32_t (*)(uint32_t cls_context, const Guid* riid, void** ppv);
void register_class(const Guid& clsid, ClassFactoryFn factory);

}  // namespace rt::com_shim
