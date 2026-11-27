#include "rts0_serializer.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <type_traits>
#include <vector>

#include "d3d12_xs_rootsig.h"

namespace rt::d3d12_vulkan {
namespace {

class Blob final : public ID3DBlob {
public:
    explicit Blob(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D10Blob)) {
            AddRef();
            *out = static_cast<ID3DBlob*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (!left) delete this;
        return left;
    }
    LPVOID STDMETHODCALLTYPE GetBufferPointer() override { return bytes_.data(); }
    SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return bytes_.size(); }

private:
    std::vector<uint8_t> bytes_;
    std::atomic<ULONG> refs_{1};
};

struct Range {
    uint32_t type, num, base, space, flags, offset;
};
struct Param {
    uint32_t type, visibility;
    uint32_t w[3];
    std::vector<Range> ranges;
};

struct Normal {
    uint32_t version = 1;
    uint32_t flags = 0;
    std::vector<Param> params;
    std::vector<uint32_t> samplers;
};

template <class P, class R>
bool add_params(Normal& n, const P* params, UINT count) {
    if (count && !params) return false;
    for (UINT i = 0; i < count; ++i) {
        const P& p = params[i];
        Param q{static_cast<uint32_t>(p.ParameterType), static_cast<uint32_t>(p.ShaderVisibility), {0, 0, 0}, {}};
        switch (p.ParameterType) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
            const auto& t = p.DescriptorTable;
            if (t.NumDescriptorRanges && !t.pDescriptorRanges) return false;
            for (UINT j = 0; j < t.NumDescriptorRanges; ++j) {
                const R& r = t.pDescriptorRanges[j];
                uint32_t flags = 0;
                if constexpr (std::is_same_v<R, D3D12_DESCRIPTOR_RANGE1>) flags = static_cast<uint32_t>(r.Flags);
                q.ranges.push_back(Range{static_cast<uint32_t>(r.RangeType), r.NumDescriptors, r.BaseShaderRegister,
                                         r.RegisterSpace, flags, r.OffsetInDescriptorsFromTableStart});
            }
            break;
        }
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            q.w[0] = p.Constants.ShaderRegister;
            q.w[1] = p.Constants.RegisterSpace;
            q.w[2] = p.Constants.Num32BitValues;
            break;
        case D3D12_ROOT_PARAMETER_TYPE_CBV:
        case D3D12_ROOT_PARAMETER_TYPE_SRV:
        case D3D12_ROOT_PARAMETER_TYPE_UAV:
            q.w[0] = p.Descriptor.ShaderRegister;
            q.w[1] = p.Descriptor.RegisterSpace;
            if constexpr (std::is_same_v<P, D3D12_ROOT_PARAMETER1>) q.w[2] = static_cast<uint32_t>(p.Descriptor.Flags);
            break;
        default: return false;
        }
        n.params.push_back(std::move(q));
    }
    return true;
}

template <class S>
bool add_samplers(Normal& n, const S* samplers, UINT count, bool with_flags) {
    if (count && !samplers) return false;
    for (UINT i = 0; i < count; ++i) {
        uint32_t w[14] = {};
        std::memcpy(w, &samplers[i], 13 * 4);
        if constexpr (std::is_same_v<S, D3D12_STATIC_SAMPLER_DESC1>) w[13] = static_cast<uint32_t>(samplers[i].Flags);
        n.samplers.insert(n.samplers.end(), w, w + (with_flags ? 14 : 13));
    }
    return true;
}

void put(std::vector<uint8_t>& b, size_t at, uint32_t v) { std::memcpy(b.data() + at, &v, 4); }
size_t push(std::vector<uint8_t>& b, uint32_t v) {
    const size_t at = b.size();
    b.resize(at + 4);
    put(b, at, v);
    return at;
}

std::vector<uint8_t> write_rts0(const Normal& n) {
    const bool v11 = n.version >= 2;
    std::vector<uint8_t> p;
    push(p, n.version);
    push(p, static_cast<uint32_t>(n.params.size()));
    push(p, 24);
    push(p, static_cast<uint32_t>(n.samplers.size() / (n.version == 3 ? 14 : 13)));
    const size_t samplers_at = push(p, 0);
    push(p, n.flags);
    std::vector<size_t> payload_slot;
    for (const Param& q : n.params) {
        push(p, q.type);
        push(p, q.visibility);
        payload_slot.push_back(push(p, 0));
    }
    for (size_t i = 0; i < n.params.size(); ++i) {
        const Param& q = n.params[i];
        put(p, payload_slot[i], static_cast<uint32_t>(p.size()));
        if (q.type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
            push(p, static_cast<uint32_t>(q.ranges.size()));
            const size_t ranges_at = push(p, 0);
            put(p, ranges_at, static_cast<uint32_t>(p.size()));
            for (const Range& r : q.ranges) {
                push(p, r.type);
                push(p, r.num);
                push(p, r.base);
                push(p, r.space);
                if (v11) push(p, r.flags);
                push(p, r.offset);
            }
        } else if (q.type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
            push(p, q.w[0]);
            push(p, q.w[1]);
            push(p, q.w[2]);
        } else {
            push(p, q.w[0]);
            push(p, q.w[1]);
            if (v11) push(p, q.w[2]);
        }
    }
    put(p, samplers_at, static_cast<uint32_t>(p.size()));
    for (uint32_t w : n.samplers) push(p, w);

    std::vector<uint8_t> c(36, 0);
    std::memcpy(c.data(), "DXBC", 4);
    put(c, 20, 1);
    put(c, 28, 1);
    put(c, 32, 36);
    c.insert(c.end(), {'R', 'T', 'S', '0'});
    push(c, static_cast<uint32_t>(p.size()));
    c.insert(c.end(), p.begin(), p.end());
    put(c, 24, static_cast<uint32_t>(c.size()));
    uint32_t hash[4];
    rt::d3d12_xs::dxbc_checksum(c.data(), c.size(), hash);
    std::memcpy(c.data() + 4, hash, sizeof hash);
    return c;
}

HRESULT finish(const Normal& n, bool ok, ID3DBlob** blob, ID3DBlob** error) {
    if (error) *error = nullptr;
    if (!blob) return E_POINTER;
    *blob = nullptr;
    if (!ok) return E_INVALIDARG;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] d3d12_xs: root signatures are serialized by the layer's RTS0 writer (no host d3d12.dll)\n");
    *blob = new (std::nothrow) Blob(write_rts0(n));
    return *blob ? S_OK : E_OUTOFMEMORY;
}

}  // namespace

HRESULT serialize_root_signature_rts0(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc, ID3DBlob** blob, ID3DBlob** error) {
    if (!desc) return E_INVALIDARG;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC d{};
    if (!rt::d3d12_xs::copy_versioned_root_signature_desc_clear_xbox_flags(*desc, d)) return finish({}, false, blob, error);
    Normal n;
    bool ok = false;
    switch (d.Version) {
    case D3D_ROOT_SIGNATURE_VERSION_1_0:
        n.version = 1;
        n.flags = static_cast<uint32_t>(d.Desc_1_0.Flags);
        ok = add_params<D3D12_ROOT_PARAMETER, D3D12_DESCRIPTOR_RANGE>(n, d.Desc_1_0.pParameters, d.Desc_1_0.NumParameters) &&
             add_samplers(n, d.Desc_1_0.pStaticSamplers, d.Desc_1_0.NumStaticSamplers, false);
        break;
    case D3D_ROOT_SIGNATURE_VERSION_1_1:
        n.version = 2;
        n.flags = static_cast<uint32_t>(d.Desc_1_1.Flags);
        ok = add_params<D3D12_ROOT_PARAMETER1, D3D12_DESCRIPTOR_RANGE1>(n, d.Desc_1_1.pParameters, d.Desc_1_1.NumParameters) &&
             add_samplers(n, d.Desc_1_1.pStaticSamplers, d.Desc_1_1.NumStaticSamplers, false);
        break;
    case D3D_ROOT_SIGNATURE_VERSION_1_2:
        n.version = 3;
        n.flags = static_cast<uint32_t>(d.Desc_1_2.Flags);
        ok = add_params<D3D12_ROOT_PARAMETER1, D3D12_DESCRIPTOR_RANGE1>(n, d.Desc_1_2.pParameters, d.Desc_1_2.NumParameters) &&
             add_samplers(n, d.Desc_1_2.pStaticSamplers, d.Desc_1_2.NumStaticSamplers, true);
        break;
    default: break;
    }
    return finish(n, ok, blob, error);
}

HRESULT serialize_root_signature_rts0_legacy(const D3D12_ROOT_SIGNATURE_DESC* desc, D3D_ROOT_SIGNATURE_VERSION version,
                                             ID3DBlob** blob, ID3DBlob** error) {
    if (!desc || version != D3D_ROOT_SIGNATURE_VERSION_1_0) return finish({}, false, blob, error);
    D3D12_ROOT_SIGNATURE_DESC d{};
    rt::d3d12_xs::copy_root_signature_desc_clear_xbox_flags(*desc, d);
    Normal n;
    n.version = 1;
    n.flags = static_cast<uint32_t>(d.Flags);
    const bool ok = add_params<D3D12_ROOT_PARAMETER, D3D12_DESCRIPTOR_RANGE>(n, d.pParameters, d.NumParameters) &&
                    add_samplers(n, d.pStaticSamplers, d.NumStaticSamplers, false);
    return finish(n, ok, blob, error);
}

}  // namespace rt::d3d12_vulkan
