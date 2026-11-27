#include "rts0_deserializer.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

namespace rt::d3d12_vulkan {
namespace {

struct Reader {
    const uint8_t* p;
    size_t n;
    bool u32(size_t at, uint32_t& v) const {
        if (at > n || n - at < 4) return false;
        std::memcpy(&v, p + at, 4);
        return true;
    }
};

// The RTS0 payload of a DXBC container, or the bytes themselves when they are not one.
bool rts0_payload(const uint8_t* data, size_t size, const uint8_t*& payload, size_t& payload_size) {
    if (size >= 32 && std::memcmp(data, "DXBC", 4) == 0) {
        uint32_t total = 0, count = 0;
        std::memcpy(&total, data + 24, 4);
        std::memcpy(&count, data + 28, 4);
        if (total > size || count > 64 || 32 + size_t(count) * 4 > total) return false;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t at = 0;
            std::memcpy(&at, data + 32 + 4 * i, 4);
            if (at > total || total - at < 8) return false;
            uint32_t len = 0;
            std::memcpy(&len, data + at + 4, 4);
            if (total - at - 8 < len) return false;
            if (std::memcmp(data + at, "RTS0", 4) == 0) {
                payload = data + at + 8;
                payload_size = len;
                return true;
            }
        }
        return false;
    }
    payload = data;
    payload_size = size;
    return size >= 24;
}

constexpr uint32_t kXboxFlagBits = 0xFF000000u;

struct Model {
    uint32_t version = 1;
    D3D12_ROOT_SIGNATURE_FLAGS flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    std::vector<D3D12_ROOT_PARAMETER> params0;
    std::vector<D3D12_ROOT_PARAMETER1> params1;
    std::vector<std::vector<D3D12_DESCRIPTOR_RANGE>> ranges0;
    std::vector<std::vector<D3D12_DESCRIPTOR_RANGE1>> ranges1;
    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers0;
    std::vector<D3D12_STATIC_SAMPLER_DESC1> samplers1;
    D3D12_ROOT_SIGNATURE_DESC desc0{};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC v10{}, v11{}, v12{};
};

bool parse(const uint8_t* data, size_t size, Model& m) {
    const uint8_t* pl = nullptr;
    size_t pn = 0;
    if (!data || !rts0_payload(data, size, pl, pn)) return false;
    const Reader r{pl, pn};
    uint32_t version, nparams, params_at, nsamplers, samplers_at, flags;
    if (!r.u32(0, version) || !r.u32(4, nparams) || !r.u32(8, params_at) || !r.u32(12, nsamplers) ||
        !r.u32(16, samplers_at) || !r.u32(20, flags))
        return false;
    if (version < 1 || version > 3 || nparams > 64 || nsamplers > 2032) return false;
    const bool v11 = version >= 2;
    m.version = version;
    m.flags = D3D12_ROOT_SIGNATURE_FLAGS(flags & ~kXboxFlagBits);
    m.params0.resize(nparams);
    m.params1.resize(nparams);
    m.ranges0.resize(nparams);
    m.ranges1.resize(nparams);
    for (uint32_t i = 0; i < nparams; ++i) {
        uint32_t type, vis, at;
        const size_t entry = size_t(params_at) + 12 * size_t(i);
        if (!r.u32(entry, type) || !r.u32(entry + 4, vis) || !r.u32(entry + 8, at)) return false;
        D3D12_ROOT_PARAMETER& p0 = m.params0[i];
        D3D12_ROOT_PARAMETER1& p1 = m.params1[i];
        p0 = {};
        p1 = {};
        p0.ParameterType = p1.ParameterType = D3D12_ROOT_PARAMETER_TYPE(type);
        p0.ShaderVisibility = p1.ShaderVisibility = D3D12_SHADER_VISIBILITY(vis);
        switch (type) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
            uint32_t count, ranges_at;
            if (!r.u32(at, count) || !r.u32(size_t(at) + 4, ranges_at) || count > 4096) return false;
            const size_t stride = v11 ? 24 : 20;
            for (uint32_t j = 0; j < count; ++j) {
                const size_t q = size_t(ranges_at) + stride * j;
                uint32_t w[6] = {};
                for (size_t k = 0; k < stride / 4; ++k)
                    if (!r.u32(q + 4 * k, w[k])) return false;
                D3D12_DESCRIPTOR_RANGE1 d1{};
                d1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE(w[0]);
                d1.NumDescriptors = w[1];
                d1.BaseShaderRegister = w[2];
                d1.RegisterSpace = w[3];
                if (v11) {
                    d1.Flags = D3D12_DESCRIPTOR_RANGE_FLAGS(w[4]);
                    d1.OffsetInDescriptorsFromTableStart = w[5];
                } else {
                    d1.Flags = w[0] == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER
                                   ? D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE
                                   : D3D12_DESCRIPTOR_RANGE_FLAGS(D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
                                                                  D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);
                    d1.OffsetInDescriptorsFromTableStart = w[4];
                }
                m.ranges1[i].push_back(d1);
                m.ranges0[i].push_back(D3D12_DESCRIPTOR_RANGE{d1.RangeType, d1.NumDescriptors, d1.BaseShaderRegister,
                                                              d1.RegisterSpace,
                                                              d1.OffsetInDescriptorsFromTableStart});
            }
            break;
        }
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: {
            uint32_t reg, space, num;
            if (!r.u32(at, reg) || !r.u32(size_t(at) + 4, space) || !r.u32(size_t(at) + 8, num)) return false;
            p0.Constants = p1.Constants = D3D12_ROOT_CONSTANTS{reg, space, num};
            break;
        }
        case D3D12_ROOT_PARAMETER_TYPE_CBV:
        case D3D12_ROOT_PARAMETER_TYPE_SRV:
        case D3D12_ROOT_PARAMETER_TYPE_UAV: {
            uint32_t reg, space, f = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
            if (!r.u32(at, reg) || !r.u32(size_t(at) + 4, space)) return false;
            if (v11 && !r.u32(size_t(at) + 8, f)) return false;
            p0.Descriptor = D3D12_ROOT_DESCRIPTOR{reg, space};
            p1.Descriptor = D3D12_ROOT_DESCRIPTOR1{reg, space, D3D12_ROOT_DESCRIPTOR_FLAGS(f)};
            break;
        }
        default: return false;
        }
    }
    for (uint32_t i = 0; i < nparams; ++i) {
        if (m.params0[i].ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) continue;
        m.params0[i].DescriptorTable = {UINT(m.ranges0[i].size()), m.ranges0[i].empty() ? nullptr : m.ranges0[i].data()};
        m.params1[i].DescriptorTable = {UINT(m.ranges1[i].size()), m.ranges1[i].empty() ? nullptr : m.ranges1[i].data()};
    }
    const size_t sstride = version == 3 ? 56 : 52;
    for (uint32_t i = 0; i < nsamplers; ++i) {
        const size_t q = size_t(samplers_at) + sstride * i;
        if (q > pn || pn - q < sstride) return false;
        D3D12_STATIC_SAMPLER_DESC1 s1{};
        std::memcpy(&s1, pl + q, 52);
        if (version == 3) std::memcpy(&s1.Flags, pl + q + 52, 4);
        D3D12_STATIC_SAMPLER_DESC s0{};
        std::memcpy(&s0, pl + q, 52);
        m.samplers0.push_back(s0);
        m.samplers1.push_back(s1);
    }
    const UINT np = UINT(nparams), ns = UINT(nsamplers);
    m.desc0 = {np, np ? m.params0.data() : nullptr, ns, ns ? m.samplers0.data() : nullptr, m.flags};
    m.v10.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    m.v10.Desc_1_0 = m.desc0;
    m.v11.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    m.v11.Desc_1_1 = {np, np ? m.params1.data() : nullptr, ns, ns ? m.samplers0.data() : nullptr, m.flags};
    m.v12.Version = D3D_ROOT_SIGNATURE_VERSION_1_2;
    m.v12.Desc_1_2 = {np, np ? m.params1.data() : nullptr, ns, ns ? m.samplers1.data() : nullptr, m.flags};
    return true;
}

template <class I>
class Deserializer : public I {
public:
    explicit Deserializer(std::unique_ptr<Model> m) : m_(std::move(m)) {}
    virtual ~Deserializer() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(I)) {
            AddRef();
            *out = static_cast<I*>(this);
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

protected:
    std::unique_ptr<Model> m_;
    std::atomic<ULONG> refs_{1};
};

class Legacy final : public Deserializer<ID3D12RootSignatureDeserializer> {
public:
    using Deserializer::Deserializer;
    const D3D12_ROOT_SIGNATURE_DESC* STDMETHODCALLTYPE GetRootSignatureDesc() override { return &m_->desc0; }
};

class Versioned final : public Deserializer<ID3D12VersionedRootSignatureDeserializer> {
public:
    using Deserializer::Deserializer;
    HRESULT STDMETHODCALLTYPE GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION v,
                                                            const D3D12_VERSIONED_ROOT_SIGNATURE_DESC** out) override {
        if (!out) return E_INVALIDARG;
        *out = v == D3D_ROOT_SIGNATURE_VERSION_1_0   ? &m_->v10
               : v == D3D_ROOT_SIGNATURE_VERSION_1_1 ? &m_->v11
               : v == D3D_ROOT_SIGNATURE_VERSION_1_2 ? &m_->v12
                                                     : nullptr;
        return *out ? S_OK : E_INVALIDARG;
    }
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* STDMETHODCALLTYPE GetUnconvertedRootSignatureDesc() override {
        return m_->version == 1 ? &m_->v10 : m_->version == 2 ? &m_->v11 : &m_->v12;
    }
};

template <class Impl, class I>
HRESULT create(const void* data, SIZE_T size, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    auto m = std::unique_ptr<Model>(new (std::nothrow) Model);
    if (!m) return E_OUTOFMEMORY;
    if (!parse(static_cast<const uint8_t*>(data), size_t(size), *m)) {
        std::fprintf(stderr, "[rt] d3d12_xs: root signature deserializer: no readable RTS0 in %zu bytes\n", size_t(size));
        return E_INVALIDARG;
    }
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "[rt] d3d12_xs: root signatures are deserialized by the layer's RTS0 reader (no host d3d12.dll)\n");
    Impl* obj = new (std::nothrow) Impl(std::move(m));
    if (!obj) return E_OUTOFMEMORY;
    const HRESULT hr = obj->QueryInterface(iid, out);
    obj->Release();
    return hr;
}

}  // namespace

HRESULT create_root_signature_deserializer_rts0(const void* data, SIZE_T size, REFIID iid, void** out) {
    return create<Legacy, ID3D12RootSignatureDeserializer>(data, size, iid, out);
}

HRESULT create_versioned_root_signature_deserializer_rts0(const void* data, SIZE_T size, REFIID iid, void** out) {
    return create<Versioned, ID3D12VersionedRootSignatureDeserializer>(data, size, iid, out);
}

}  // namespace rt::d3d12_vulkan
