#pragma once

#include <cstdint>

namespace rt::com_shim {

struct Guid {
    uint32_t data1;
    uint16_t data2, data3;
    uint8_t data4[8];
};
static_assert(sizeof(Guid) == 16);

// A class the title may create with CoCreateInstance (for example the audio device enumerator, registered by
// shims_gdk). Returns an HRESULT and stores the interface in *ppv.
using ClassFactoryFn = int32_t (*)(uint32_t cls_context, const Guid* riid, void** ppv);
void register_class(const Guid& clsid, ClassFactoryFn factory);

// Guest COM nesting depth on the calling thread, for tests.
uint32_t thread_depth();

}  // namespace rt::com_shim
