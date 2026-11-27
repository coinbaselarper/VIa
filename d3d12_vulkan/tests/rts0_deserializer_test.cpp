// The layer's RTS0 reader (platform/rts0_deserializer.cpp): synthetic round trips through the layer's RTS0 writer at
// versions 1.0, 1.1 and 1.2, malformed input, and, when RTS0_TEST_DXBC_DIR names a folder of DXBC containers (the
// FidelityFX DLL's 29), each container's own RTS0: deserialized, written again, and compared with the stored payload.
#include <d3d12.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <string>
#include <vector>

#include "rts0_deserializer.h"
#include "rts0_serializer.h"

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

bool find_rts0(const std::vector<uint8_t>& c, size_t& at, size_t& len) {
    if (c.size() < 32 || std::memcmp(c.data(), "DXBC", 4) != 0) return false;
    uint32_t count;
    std::memcpy(&count, c.data() + 28, 4);
    for (uint32_t i = 0; i < count && 32 + 4 * i + 4 <= c.size(); ++i) {
        uint32_t off, n;
        std::memcpy(&off, c.data() + 32 + 4 * i, 4);
        if (off + 8 > c.size()) return false;
        std::memcpy(&n, c.data() + off + 4, 4);
        if (std::memcmp(c.data() + off, "RTS0", 4) == 0 && off + 8 + n <= c.size()) {
            at = off + 8;
            len = n;
            return true;
        }
    }
    return false;
}

std::vector<uint8_t> payload_of(ID3DBlob* blob) {
    std::vector<uint8_t> c(static_cast<uint8_t*>(blob->GetBufferPointer()),
                           static_cast<uint8_t*>(blob->GetBufferPointer()) + blob->GetBufferSize());
    size_t at = 0, len = 0;
    if (!find_rts0(c, at, len)) return {};
    return std::vector<uint8_t>(c.begin() + at, c.begin() + at + len);
}

void synthetic(D3D_ROOT_SIGNATURE_VERSION version) {
    const std::string tag = "v" + std::to_string(int(version));
    D3D12_DESCRIPTOR_RANGE1 ranges[3] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 1, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 3, 1, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND},
        {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 7, 2, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 9}};
    D3D12_DESCRIPTOR_RANGE1 sampler_range{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 2, 0, 0,
                                          D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0};
    D3D12_ROOT_PARAMETER1 p[6]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[0].DescriptorTable = {3, ranges};
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants = {5, 1, 12};
    p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p[2].Descriptor = {2, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC};
    p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    p[3].Descriptor = {8, 3, D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
    p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[4].Descriptor = {6, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    p[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[5].DescriptorTable = {1, &sampler_range};
    D3D12_STATIC_SAMPLER_DESC1 s1{};
    s1.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    s1.AddressU = s1.AddressV = s1.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    s1.MaxLOD = 1000.0f;
    s1.ShaderRegister = 4;
    s1.RegisterSpace = 2;
    s1.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    s1.Flags = D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES;
    D3D12_STATIC_SAMPLER_DESC s0{};
    std::memcpy(&s0, &s1, sizeof s0);

    D3D12_DESCRIPTOR_RANGE r0[3];
    for (int i = 0; i < 3; ++i)
        r0[i] = {ranges[i].RangeType, ranges[i].NumDescriptors, ranges[i].BaseShaderRegister, ranges[i].RegisterSpace,
                 ranges[i].OffsetInDescriptorsFromTableStart};
    D3D12_DESCRIPTOR_RANGE sr0{sampler_range.RangeType, 2, 0, 0, 0};
    D3D12_ROOT_PARAMETER q[6]{};
    for (int i = 0; i < 6; ++i) {
        q[i].ParameterType = p[i].ParameterType;
        q[i].ShaderVisibility = p[i].ShaderVisibility;
    }
    q[0].DescriptorTable = {3, r0};
    q[1].Constants = p[1].Constants;
    q[2].Descriptor = {2, 0};
    q[3].Descriptor = {8, 3};
    q[4].Descriptor = {6, 0};
    q[5].DescriptorTable = {1, &sr0};

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC d{};
    d.Version = version;
    const auto flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if (version == D3D_ROOT_SIGNATURE_VERSION_1_0) d.Desc_1_0 = {6, q, 1, &s0, flags};
    if (version == D3D_ROOT_SIGNATURE_VERSION_1_1) d.Desc_1_1 = {6, p, 1, &s0, flags};
    if (version == D3D_ROOT_SIGNATURE_VERSION_1_2) d.Desc_1_2 = {6, p, 1, &s1, flags};
    ID3DBlob* blob = nullptr;
    check(rt::d3d12_vulkan::serialize_root_signature_rts0(&d, &blob, nullptr) == S_OK && blob, tag + " serialize");
    if (!blob) return;

    ID3D12RootSignatureDeserializer* legacy = nullptr;
    check(rt::d3d12_vulkan::create_root_signature_deserializer_rts0(blob->GetBufferPointer(), blob->GetBufferSize(),
                                                                    __uuidof(ID3D12RootSignatureDeserializer),
                                                                    reinterpret_cast<void**>(&legacy)) == S_OK &&
              legacy,
          tag + " legacy deserializer");
    if (legacy) {
        const D3D12_ROOT_SIGNATURE_DESC* g = legacy->GetRootSignatureDesc();
        check(g && g->NumParameters == 6 && g->NumStaticSamplers == 1 && g->Flags == flags, tag + " counts");
        check(g->pParameters[0].DescriptorTable.NumDescriptorRanges == 3 &&
                  g->pParameters[0].DescriptorTable.pDescriptorRanges[1].RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV &&
                  g->pParameters[0].DescriptorTable.pDescriptorRanges[1].RegisterSpace == 1 &&
                  g->pParameters[0].DescriptorTable.pDescriptorRanges[1].OffsetInDescriptorsFromTableStart ==
                      D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND &&
                  g->pParameters[0].DescriptorTable.pDescriptorRanges[2].OffsetInDescriptorsFromTableStart == 9,
              tag + " table ranges");
        check(g->pParameters[1].ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS &&
                  g->pParameters[1].Constants.ShaderRegister == 5 && g->pParameters[1].Constants.Num32BitValues == 12 &&
                  g->pParameters[1].ShaderVisibility == D3D12_SHADER_VISIBILITY_PIXEL,
              tag + " constants");
        check(g->pParameters[2].ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV && g->pParameters[2].Descriptor.ShaderRegister == 2 &&
                  g->pParameters[3].ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV &&
                  g->pParameters[3].Descriptor.RegisterSpace == 3 &&
                  g->pParameters[4].ParameterType == D3D12_ROOT_PARAMETER_TYPE_UAV,
              tag + " root descriptors keep their kind");
        check(sizeof(D3D12_ROOT_PARAMETER) == 32 && sizeof(D3D12_DESCRIPTOR_RANGE) == 20,
              "struct sizes the FidelityFX DLL indexes with (32-byte parameters, 20-byte ranges)");
        check(g->pStaticSamplers[0].ShaderRegister == 4 && g->pStaticSamplers[0].RegisterSpace == 2 &&
                  g->pStaticSamplers[0].MaxLOD == 1000.0f,
              tag + " static sampler");
        IUnknown* unk = nullptr;
        check(legacy->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&unk)) == S_OK && unk, tag + " QI IUnknown");
        if (unk) unk->Release();
        check(legacy->Release() == 0, tag + " release");
    }

    ID3D12VersionedRootSignatureDeserializer* versioned = nullptr;
    check(rt::d3d12_vulkan::create_versioned_root_signature_deserializer_rts0(
              blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12VersionedRootSignatureDeserializer),
              reinterpret_cast<void**>(&versioned)) == S_OK &&
              versioned,
          tag + " versioned deserializer");
    if (versioned) {
        const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* u = versioned->GetUnconvertedRootSignatureDesc();
        check(u && u->Version == version, tag + " unconverted version");
        ID3DBlob* again = nullptr;
        check(rt::d3d12_vulkan::serialize_root_signature_rts0(u, &again, nullptr) == S_OK && again, tag + " re-serialize");
        if (again) {
            check(payload_of(again) == payload_of(blob), tag + " round trip is byte-identical");
            again->Release();
        }
        if (version != D3D_ROOT_SIGNATURE_VERSION_1_0) {
            check(u->Desc_1_1.pParameters[2].Descriptor.Flags == D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC &&
                      u->Desc_1_1.pParameters[0].DescriptorTable.pDescriptorRanges[0].Flags ==
                          D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC,
                  tag + " 1.1 flags");
        }
        if (version == D3D_ROOT_SIGNATURE_VERSION_1_2)
            check(u->Desc_1_2.pStaticSamplers[0].Flags == D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES, tag + " 1.2 sampler flags");
        const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* at10 = nullptr;
        check(versioned->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_0, &at10) == S_OK && at10 &&
                  at10->Desc_1_0.NumParameters == 6,
              tag + " at 1.0");
        const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* bad = nullptr;
        check(versioned->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION(9), &bad) == E_INVALIDARG && !bad,
              tag + " unknown version refused");
        versioned->Release();
    }
    blob->Release();
}

void malformed() {
    void* out = reinterpret_cast<void*>(1);
    const uint8_t junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    check(rt::d3d12_vulkan::create_root_signature_deserializer_rts0(junk, sizeof junk,
                                                                    __uuidof(ID3D12RootSignatureDeserializer), &out) ==
                  E_INVALIDARG &&
              !out,
          "short input refused");
    uint32_t bad_version[6] = {7, 0, 24, 0, 24, 0};
    check(rt::d3d12_vulkan::create_root_signature_deserializer_rts0(bad_version, sizeof bad_version,
                                                                    __uuidof(ID3D12RootSignatureDeserializer), &out) ==
              E_INVALIDARG,
          "unknown RTS0 version refused");
    uint32_t past_end[9] = {1, 1, 24, 0, 36, 0, 2, 0, 0x1000};
    check(rt::d3d12_vulkan::create_root_signature_deserializer_rts0(past_end, sizeof past_end,
                                                                    __uuidof(ID3D12RootSignatureDeserializer), &out) ==
              E_INVALIDARG,
          "parameter payload past the end refused");
    uint32_t empty[6] = {2, 0, 24, 0, 24, 0};
    ID3D12RootSignatureDeserializer* d = nullptr;
    check(rt::d3d12_vulkan::create_root_signature_deserializer_rts0(empty, sizeof empty,
                                                                    __uuidof(ID3D12RootSignatureDeserializer),
                                                                    reinterpret_cast<void**>(&d)) == S_OK &&
              d && d->GetRootSignatureDesc()->NumParameters == 0,
          "empty bare payload");
    if (d) d->Release();
    check(rt::d3d12_vulkan::create_root_signature_deserializer_rts0(empty, sizeof empty, __uuidof(ID3D12Device), &out) ==
              E_NOINTERFACE,
          "wrong IID refused");
}

void containers(const char* dir) {
    DIR* d = opendir(dir);
    if (!d) {
        std::printf("SKIP containers: cannot open %s\n", dir);
        return;
    }
    int seen = 0, identical = 0, cbvs = 0;
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 5 && n.substr(n.size() - 5) == ".dxbc") names.push_back(n);
    }
    closedir(d);
    for (const std::string& n : names) {
        std::ifstream f(std::string(dir) + "/" + n, std::ios::binary);
        std::vector<uint8_t> c((std::istreambuf_iterator<char>(f)), {});
        size_t at = 0, len = 0;
        if (!find_rts0(c, at, len)) continue;
        ++seen;
        ID3D12VersionedRootSignatureDeserializer* v = nullptr;
        ID3D12RootSignatureDeserializer* l = nullptr;
        const bool ok = rt::d3d12_vulkan::create_versioned_root_signature_deserializer_rts0(
                            c.data(), c.size(), __uuidof(ID3D12VersionedRootSignatureDeserializer),
                            reinterpret_cast<void**>(&v)) == S_OK &&
                        rt::d3d12_vulkan::create_root_signature_deserializer_rts0(
                            c.data(), c.size(), __uuidof(ID3D12RootSignatureDeserializer), reinterpret_cast<void**>(&l)) ==
                            S_OK;
        check(ok, n + " deserializes");
        if (!ok) continue;
        const D3D12_ROOT_SIGNATURE_DESC* g = l->GetRootSignatureDesc();
        std::string shape;
        for (UINT i = 0; i < g->NumParameters; ++i) {
            const auto& p = g->pParameters[i];
            if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV) {
                ++cbvs;
                shape += " cbv(b" + std::to_string(p.Descriptor.ShaderRegister) + ")";
            } else if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
                shape += " table[";
                for (UINT j = 0; j < p.DescriptorTable.NumDescriptorRanges; ++j) {
                    const auto& r = p.DescriptorTable.pDescriptorRanges[j];
                    shape += std::string(j ? "," : "") + "SUCs"[r.RangeType & 3] +
                             std::to_string(r.BaseShaderRegister) + "x" + std::to_string(r.NumDescriptors);
                }
                shape += "]";
            } else {
                shape += " type" + std::to_string(int(p.ParameterType));
            }
        }
        std::printf("INFO %s: RTS0 v%u, %u parameters, %u static samplers:%s\n", n.c_str(),
                    unsigned(v->GetUnconvertedRootSignatureDesc()->Version), g->NumParameters, g->NumStaticSamplers,
                    shape.c_str());
        ID3DBlob* again = nullptr;
        if (rt::d3d12_vulkan::serialize_root_signature_rts0(v->GetUnconvertedRootSignatureDesc(), &again, nullptr) == S_OK) {
            std::vector<uint8_t> stored(c.begin() + at, c.begin() + at + len);
            if (stored.size() >= 24) {
                uint32_t flags;
                std::memcpy(&flags, stored.data() + 20, 4);
                flags &= 0x00FFFFFFu;
                std::memcpy(stored.data() + 20, &flags, 4);
            }
            const bool same = payload_of(again) == stored;
            identical += same;
            check(same, n + " RTS0 written again is the stored payload (Xbox flag byte cleared)");
            again->Release();
        } else {
            check(false, n + " re-serialize");
        }
        v->Release();
        l->Release();
    }
    std::printf("INFO containers: %d with RTS0, %d byte-identical after a read and a write, %d root CBVs\n", seen,
                identical, cbvs);
    check(seen > 0, "containers found");
}

}  // namespace

int main() {
    synthetic(D3D_ROOT_SIGNATURE_VERSION_1_0);
    synthetic(D3D_ROOT_SIGNATURE_VERSION_1_1);
    synthetic(D3D_ROOT_SIGNATURE_VERSION_1_2);
    malformed();
    if (const char* dir = std::getenv("RTS0_TEST_DXBC_DIR")) containers(dir);
    std::printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
