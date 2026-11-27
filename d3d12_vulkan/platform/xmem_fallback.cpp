#include "xmem.h"

// shims_core's xmem does not track physical-page aliases for the D3D12 layer yet; until it defines this, a
// geometry address has no aliases. A strong definition there replaces this one.
namespace rt::xmem {
__attribute__((weak)) void geometry_aliases(std::uintptr_t, std::vector<std::uintptr_t>&) {}
}  // namespace rt::xmem
