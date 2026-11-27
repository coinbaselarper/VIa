#include <windows.h>
#include <d3d12.h>

#include "bcpack.h"
#include "d3d12_xs_rootsig.h"

#include <cstdio>

int main() {
    const uint8_t blob[32] = {};
    uint32_t sum[4] = {};
    rt::d3d12_xs::dxbc_checksum(blob, sizeof(blob), sum);
    std::printf("portable_link_check: dxbc_checksum[0]=%08x sizeof(D3D12_ROOT_SIGNATURE_DESC)=%zu\n", sum[0],
                sizeof(D3D12_ROOT_SIGNATURE_DESC));
    return 0;
}
