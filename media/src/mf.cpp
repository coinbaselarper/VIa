// Media Foundation subset the title imports (mfplat.dll, mfreadwrite.dll). The title's media reader thread
// (exe+0x33a45c0) opens a URL with IMFSourceResolver::CreateObjectFromURL and reads it with an IMFSourceReader that
// decodes H.264/WMV video to NV12 and AAC/WMA audio to 16-bit 48 kHz PCM. The package holds no such media (every movie
// is Bink 2 in movies.rpf), so no media source is implemented: the resolver fails with the HRESULT Media Foundation
// gives for the case and a named log line, and the title takes its error path (reader state 5).
#include "media_common.h"
#include "media_exports.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace rt::media {
namespace {

#define API GTA6RT_GUEST_ABI

constexpr Guid kIidIMFAttributes{0x2cd2d921, 0xc447, 0x44a7, {0xa1, 0x3c, 0x4a, 0xda, 0xbf, 0xc2, 0x47, 0xe3}};
constexpr Guid kIidIMFMediaType{0x44ae0fa8, 0xea31, 0x4109, {0x8d, 0x2e, 0x4c, 0xae, 0x49, 0x97, 0xc5, 0x55}};
constexpr Guid kIidIMFSourceResolver{0xfbe5a32d, 0xa497, 0x4b61, {0xbb, 0x85, 0x97, 0xb1, 0xa8, 0x48, 0xa6, 0xe3}};
constexpr Guid kMtMajorType{0x48eba18e, 0xf8c9, 0x4687, {0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f}};
constexpr Guid kMtSubtype{0xf7e34c9a, 0x42e8, 0x4714, {0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5}};
constexpr Guid kMtUserData{0xb6bc765f, 0x4c3b, 0x40a4, {0xbd, 0x51, 0x25, 0x35, 0xb6, 0x6f, 0xe0, 0x9d}};
constexpr Guid kMtFrameRateRangeMin{0xd2e7558c, 0xdc1f, 0x403f, {0x9a, 0x72, 0xd2, 0x8b, 0xb1, 0xeb, 0x3b, 0x5e}};
constexpr Guid kMtFrameRateRangeMax{0xe3371d41, 0xb4cf, 0x4a05, {0xbd, 0x4e, 0x20, 0xb8, 0x8b, 0xb2, 0xc4, 0xd6}};
constexpr Guid kMtCompressed{0x3afd0cee, 0x18f2, 0x4ba5, {0xa1, 0x10, 0x8b, 0xea, 0x50, 0x2e, 0x1f, 0x92}};
constexpr Guid kMtAllSamplesIndependent{0xc9173739, 0x5e56, 0x461c, {0xb7, 0x13, 0x46, 0xfb, 0x99, 0x5c, 0xb9, 0x5f}};

std::atomic<bool> g_logged_startup{false}, g_logged_shutdown{false}, g_logged_repr{false}, g_logged_foreign{false},
    g_logged_async{false}, g_logged_reader{false}, g_logged_unbalanced{false};
std::atomic<int32_t> g_startups{0};
std::atomic<FileProbe> g_probe{nullptr};

std::string narrow(const char16_t* s) {
    std::string out;
    for (; s && *s; ++s) out.push_back(*s < 0x80 ? static_cast<char>(*s) : '?');
    return out;
}

size_t wlen(const char16_t* s) {
    size_t n = 0;
    while (s[n]) ++n;
    return n;
}

void guest_add_ref(uint64_t unk) {
    if (unk) guest_method(unk, 1);
}
void guest_release(uint64_t unk) {
    if (unk) guest_method(unk, 2);
}

// ---- attribute store ----

struct Item {
    Guid key{};
    uint16_t vt = 0;
    uint64_t u = 0;
    double d = 0;
    Guid g{};
    std::u16string str;
    std::vector<uint8_t> blob;
};

bool same_value(const Item& a, const Item& b) {
    if (a.vt != b.vt) return false;
    switch (a.vt) {
    case kVtUi4:
    case kVtUi8:
    case kVtUnknown: return a.u == b.u;
    case kVtR8: return a.d == b.d;
    case kVtClsid: return a.g == b.g;
    case kVtLpwstr: return a.str == b.str;
    case kVtVectorUi1: return a.blob == b.blob;
    default: return false;
    }
}

struct Attributes {
    const void* const* vtbl;
    std::atomic<uint32_t> refs{1};
    std::recursive_mutex mu;
    std::vector<Item> items;
    bool media_type = false;

    Item* find(const Guid& k) {
        for (Item& i : items)
            if (i.key == k) return &i;
        return nullptr;
    }
    void put(Item it) {
        for (Item& i : items)
            if (i.key == it.key) {
                if (i.vt == kVtUnknown) guest_release(i.u);
                i = std::move(it);
                return;
            }
        items.push_back(std::move(it));
    }
    ~Attributes() {
        for (Item& i : items)
            if (i.vt == kVtUnknown) guest_release(i.u);
    }
};

extern const void* const kAttributesVtbl[];
extern const void* const kMediaTypeVtbl[];

Attributes* ours(uint64_t p) {
    if (!p) return nullptr;
    auto* a = reinterpret_cast<Attributes*>(p);
    return (a->vtbl == kAttributesVtbl || a->vtbl == kMediaTypeVtbl) ? a : nullptr;
}

HRESULT to_propvariant(const Item& i, PropVariant* pv) {
    PropVariant v{};
    v.vt = i.vt;
    switch (i.vt) {
    case kVtUi4: v.ulVal = static_cast<uint32_t>(i.u); break;
    case kVtUi8: v.uhVal = i.u; break;
    case kVtR8: v.dblVal = i.d; break;
    case kVtClsid: {
        auto* g = static_cast<Guid*>(std::malloc(sizeof(Guid)));
        if (!g) return kEOutOfMemory;
        *g = i.g;
        v.ptr = reinterpret_cast<uint64_t>(g);
        break;
    }
    case kVtLpwstr: {
        auto* s = static_cast<char16_t*>(std::malloc((i.str.size() + 1) * 2));
        if (!s) return kEOutOfMemory;
        std::memcpy(s, i.str.c_str(), (i.str.size() + 1) * 2);
        v.ptr = reinterpret_cast<uint64_t>(s);
        break;
    }
    case kVtVectorUi1: {
        void* b = std::malloc(i.blob.empty() ? 1 : i.blob.size());
        if (!b) return kEOutOfMemory;
        if (!i.blob.empty()) std::memcpy(b, i.blob.data(), i.blob.size());
        v.caub.cElems = static_cast<uint32_t>(i.blob.size());
        v.caub.pElems = reinterpret_cast<uint64_t>(b);
        break;
    }
    case kVtUnknown:
        guest_add_ref(i.u);
        v.ptr = i.u;
        break;
    default: return kMfEInvalidType;
    }
    std::memcpy(pv, &v, sizeof v);
    return kSOk;
}

HRESULT from_propvariant(const Guid& key, const PropVariant* pv, Item* out) {
    PropVariant v;
    std::memcpy(&v, pv, sizeof v);
    Item i;
    i.key = key;
    i.vt = v.vt;
    switch (v.vt) {
    case kVtUi4: i.u = v.ulVal; break;
    case kVtUi8: i.u = v.uhVal; break;
    case kVtR8: i.d = v.dblVal; break;
    case kVtClsid:
        if (!v.ptr) return kEInvalidArg;
        std::memcpy(&i.g, reinterpret_cast<const void*>(v.ptr), sizeof(Guid));
        break;
    case kVtLpwstr:
        if (!v.ptr) return kEInvalidArg;
        i.str = reinterpret_cast<const char16_t*>(v.ptr);
        break;
    case kVtVectorUi1:
        if (v.caub.cElems && !v.caub.pElems) return kEInvalidArg;
        i.blob.assign(reinterpret_cast<const uint8_t*>(v.caub.pElems),
                      reinterpret_cast<const uint8_t*>(v.caub.pElems) + v.caub.cElems);
        break;
    case kVtUnknown:
        i.u = v.ptr;
        guest_add_ref(i.u);
        break;
    default: return kMfEInvalidType;
    }
    *out = std::move(i);
    return kSOk;
}

template <class F>
HRESULT typed_get(Attributes* a, const Guid* key, uint16_t vt, F&& f) {
    if (!key) return kEInvalidArg;
    std::lock_guard lock(a->mu);
    Item* i = a->find(*key);
    if (!i) return kMfEAttributeNotFound;
    if (i->vt != vt) return kMfEInvalidType;
    return f(*i);
}

template <class F>
HRESULT typed_set(Attributes* a, const Guid* key, uint16_t vt, F&& f) {
    if (!key) return kEInvalidArg;
    Item i;
    i.key = *key;
    i.vt = vt;
    f(i);
    std::lock_guard lock(a->mu);
    a->put(std::move(i));
    return kSOk;
}

API HRESULT a_query_interface(Attributes* a, const Guid* riid, void** ppv) {
    if (!ppv) return kEPointer;
    *ppv = nullptr;
    if (!riid) return kEInvalidArg;
    if (*riid == kIidIUnknown || *riid == kIidIMFAttributes || (a->media_type && *riid == kIidIMFMediaType)) {
        a->refs.fetch_add(1);
        *ppv = a;
        return kSOk;
    }
    return kENoInterface;
}
API uint32_t a_add_ref(Attributes* a) { return a->refs.fetch_add(1) + 1; }
API uint32_t a_release(Attributes* a) {
    const uint32_t n = a->refs.fetch_sub(1) - 1;
    if (n == 0) delete a;
    return n;
}
API HRESULT a_get_item(Attributes* a, const Guid* key, PropVariant* pv) {
    if (!key) return kEInvalidArg;
    std::lock_guard lock(a->mu);
    Item* i = a->find(*key);
    if (!i) return kMfEAttributeNotFound;
    return pv ? to_propvariant(*i, pv) : kSOk;
}
API HRESULT a_get_item_type(Attributes* a, const Guid* key, uint32_t* type) {
    if (!key || !type) return kEInvalidArg;
    std::lock_guard lock(a->mu);
    Item* i = a->find(*key);
    if (!i) return kMfEAttributeNotFound;
    *type = i->vt;
    return kSOk;
}
API HRESULT a_compare_item(Attributes* a, const Guid* key, const PropVariant* value, int32_t* result) {
    if (!key || !value || !result) return kEInvalidArg;
    *result = 0;
    Item probe;
    const HRESULT h = from_propvariant(*key, value, &probe);
    if (h < 0) return h;
    std::lock_guard lock(a->mu);
    Item* i = a->find(*key);
    *result = i && same_value(*i, probe);
    if (probe.vt == kVtUnknown) guest_release(probe.u);
    return kSOk;
}

bool subset(Attributes* small, Attributes* big, const std::vector<Guid>& skip) {
    for (const Item& i : small->items) {
        if (std::find(skip.begin(), skip.end(), i.key) != skip.end()) continue;
        Item* o = big->find(i.key);
        if (!o || !same_value(i, *o)) return false;
    }
    return true;
}

API HRESULT a_compare(Attributes* a, uint64_t theirs, uint32_t match, int32_t* result) {
    if (!result) return kEInvalidArg;
    *result = 0;
    Attributes* t = ours(theirs);
    if (!t) {
        MLOG_ONCE(g_logged_foreign, ERROR, "mf",
                  "IMFAttributes::Compare/CopyAllItems with a store not made by mfplat here: E_NOTIMPL");
        return kENotImpl;
    }
    std::scoped_lock lock(a->mu, t->mu);
    switch (match) {
    case 0: *result = subset(a, t, {}); break;
    case 1: *result = subset(t, a, {}); break;
    case 2: *result = a->items.size() == t->items.size() && subset(a, t, {}); break;
    case 3:
        *result = 1;
        for (const Item& i : a->items) {
            Item* o = t->find(i.key);
            if (o && !same_value(i, *o)) *result = 0;
        }
        break;
    case 4: *result = a->items.size() <= t->items.size() ? subset(a, t, {}) : subset(t, a, {}); break;
    default: return kEInvalidArg;
    }
    return kSOk;
}
API HRESULT a_get_uint32(Attributes* a, const Guid* key, uint32_t* out) {
    if (!out) return kEInvalidArg;
    return typed_get(a, key, kVtUi4, [&](Item& i) {
        *out = static_cast<uint32_t>(i.u);
        return kSOk;
    });
}
API HRESULT a_get_uint64(Attributes* a, const Guid* key, uint64_t* out) {
    if (!out) return kEInvalidArg;
    return typed_get(a, key, kVtUi8, [&](Item& i) {
        *out = i.u;
        return kSOk;
    });
}
API HRESULT a_get_double(Attributes* a, const Guid* key, double* out) {
    if (!out) return kEInvalidArg;
    return typed_get(a, key, kVtR8, [&](Item& i) {
        *out = i.d;
        return kSOk;
    });
}
API HRESULT a_get_guid(Attributes* a, const Guid* key, Guid* out) {
    if (!out) return kEInvalidArg;
    return typed_get(a, key, kVtClsid, [&](Item& i) {
        *out = i.g;
        return kSOk;
    });
}
API HRESULT a_get_string_length(Attributes* a, const Guid* key, uint32_t* out) {
    if (!out) return kEInvalidArg;
    return typed_get(a, key, kVtLpwstr, [&](Item& i) {
        *out = static_cast<uint32_t>(i.str.size());
        return kSOk;
    });
}
API HRESULT a_get_string(Attributes* a, const Guid* key, char16_t* buf, uint32_t cch, uint32_t* len) {
    if (!buf) return kEInvalidArg;
    return typed_get(a, key, kVtLpwstr, [&](Item& i) {
        if (len) *len = static_cast<uint32_t>(i.str.size());
        if (i.str.size() + 1 > cch) return kENotSufficientBuffer;
        std::memcpy(buf, i.str.c_str(), (i.str.size() + 1) * 2);
        return kSOk;
    });
}
API HRESULT a_get_allocated_string(Attributes* a, const Guid* key, uint64_t* out, uint32_t* len) {
    if (!out || !len) return kEInvalidArg;
    *out = 0;
    return typed_get(a, key, kVtLpwstr, [&](Item& i) {
        auto* s = static_cast<char16_t*>(std::malloc((i.str.size() + 1) * 2));
        if (!s) return kEOutOfMemory;
        std::memcpy(s, i.str.c_str(), (i.str.size() + 1) * 2);
        *out = reinterpret_cast<uint64_t>(s);
        *len = static_cast<uint32_t>(i.str.size());
        return kSOk;
    });
}
API HRESULT a_get_blob_size(Attributes* a, const Guid* key, uint32_t* out) {
    if (!out) return kEInvalidArg;
    return typed_get(a, key, kVtVectorUi1, [&](Item& i) {
        *out = static_cast<uint32_t>(i.blob.size());
        return kSOk;
    });
}
API HRESULT a_get_blob(Attributes* a, const Guid* key, uint8_t* buf, uint32_t size, uint32_t* got) {
    if (!buf) return kEInvalidArg;
    return typed_get(a, key, kVtVectorUi1, [&](Item& i) {
        if (i.blob.size() > size) return kENotSufficientBuffer;
        if (!i.blob.empty()) std::memcpy(buf, i.blob.data(), i.blob.size());
        if (got) *got = static_cast<uint32_t>(i.blob.size());
        return kSOk;
    });
}
API HRESULT a_get_allocated_blob(Attributes* a, const Guid* key, uint64_t* out, uint32_t* size) {
    if (!out || !size) return kEInvalidArg;
    *out = 0;
    return typed_get(a, key, kVtVectorUi1, [&](Item& i) {
        void* b = std::malloc(i.blob.empty() ? 1 : i.blob.size());
        if (!b) return kEOutOfMemory;
        if (!i.blob.empty()) std::memcpy(b, i.blob.data(), i.blob.size());
        *out = reinterpret_cast<uint64_t>(b);
        *size = static_cast<uint32_t>(i.blob.size());
        return kSOk;
    });
}
API HRESULT a_get_unknown(Attributes* a, const Guid* key, const Guid* riid, uint64_t* out) {
    if (!out || !riid) return kEInvalidArg;
    *out = 0;
    uint64_t unk = 0;
    const HRESULT h = typed_get(a, key, kVtUnknown, [&](Item& i) {
        unk = i.u;
        guest_add_ref(unk);
        return kSOk;
    });
    if (h < 0) return h;
    const uint64_t args[2] = {reinterpret_cast<uint64_t>(riid), reinterpret_cast<uint64_t>(out)};
    const HRESULT q = static_cast<HRESULT>(guest_method(unk, 0, args, 2));
    guest_release(unk);
    return q;
}
API HRESULT a_set_item(Attributes* a, const Guid* key, const PropVariant* value) {
    if (!key || !value) return kEInvalidArg;
    Item i;
    const HRESULT h = from_propvariant(*key, value, &i);
    if (h < 0) return h;
    std::lock_guard lock(a->mu);
    a->put(std::move(i));
    return kSOk;
}
API HRESULT a_delete_item(Attributes* a, const Guid* key) {
    if (!key) return kEInvalidArg;
    std::lock_guard lock(a->mu);
    for (auto it = a->items.begin(); it != a->items.end(); ++it)
        if (it->key == *key) {
            if (it->vt == kVtUnknown) guest_release(it->u);
            a->items.erase(it);
            break;
        }
    return kSOk;
}
API HRESULT a_delete_all_items(Attributes* a) {
    std::lock_guard lock(a->mu);
    for (Item& i : a->items)
        if (i.vt == kVtUnknown) guest_release(i.u);
    a->items.clear();
    return kSOk;
}
API HRESULT a_set_uint32(Attributes* a, const Guid* key, uint32_t v) {
    return typed_set(a, key, kVtUi4, [&](Item& i) { i.u = v; });
}
API HRESULT a_set_uint64(Attributes* a, const Guid* key, uint64_t v) {
    return typed_set(a, key, kVtUi8, [&](Item& i) { i.u = v; });
}
API HRESULT a_set_double(Attributes* a, const Guid* key, double v) {
    return typed_set(a, key, kVtR8, [&](Item& i) { i.d = v; });
}
API HRESULT a_set_guid(Attributes* a, const Guid* key, const Guid* v) {
    if (!v) return kEInvalidArg;
    return typed_set(a, key, kVtClsid, [&](Item& i) { i.g = *v; });
}
API HRESULT a_set_string(Attributes* a, const Guid* key, const char16_t* v) {
    if (!v) return kEInvalidArg;
    return typed_set(a, key, kVtLpwstr, [&](Item& i) { i.str.assign(v, wlen(v)); });
}
API HRESULT a_set_blob(Attributes* a, const Guid* key, const uint8_t* v, uint32_t size) {
    if (!v && size) return kEInvalidArg;
    return typed_set(a, key, kVtVectorUi1, [&](Item& i) { i.blob.assign(v, v + size); });
}
API HRESULT a_set_unknown(Attributes* a, const Guid* key, uint64_t unk) {
    if (!key) return kEInvalidArg;
    if (!unk) return a_delete_item(a, key);
    guest_add_ref(unk);
    return typed_set(a, key, kVtUnknown, [&](Item& i) { i.u = unk; });
}
API HRESULT a_lock_store(Attributes* a) {
    a->mu.lock();
    return kSOk;
}
API HRESULT a_unlock_store(Attributes* a) {
    a->mu.unlock();
    return kSOk;
}
API HRESULT a_get_count(Attributes* a, uint32_t* out) {
    if (!out) return kEInvalidArg;
    std::lock_guard lock(a->mu);
    *out = static_cast<uint32_t>(a->items.size());
    return kSOk;
}
API HRESULT a_get_item_by_index(Attributes* a, uint32_t index, Guid* key, PropVariant* pv) {
    if (!key) return kEInvalidArg;
    std::lock_guard lock(a->mu);
    if (index >= a->items.size()) return kEInvalidArg;
    *key = a->items[index].key;
    return pv ? to_propvariant(a->items[index], pv) : kSOk;
}
API HRESULT a_copy_all_items(Attributes* a, uint64_t dest) {
    Attributes* d = ours(dest);
    if (!d) {
        MLOG_ONCE(g_logged_foreign, ERROR, "mf",
                  "IMFAttributes::Compare/CopyAllItems with a store not made by mfplat here: E_NOTIMPL");
        return kENotImpl;
    }
    if (d == a) return kSOk;
    std::scoped_lock lock(a->mu, d->mu);
    for (Item& i : d->items)
        if (i.vt == kVtUnknown) guest_release(i.u);
    d->items = a->items;
    for (Item& i : d->items)
        if (i.vt == kVtUnknown) guest_add_ref(i.u);
    return kSOk;
}

// ---- IMFMediaType ----
API HRESULT t_get_major_type(Attributes* a, Guid* out) {
    if (!out) return kEInvalidArg;
    return a_get_guid(a, &kMtMajorType, out);
}
API HRESULT t_is_compressed_format(Attributes* a, int32_t* out) {
    if (!out) return kEInvalidArg;
    std::lock_guard lock(a->mu);
    if (Item* c = a->find(kMtCompressed); c && c->vt == kVtUi4) *out = c->u != 0;
    else if (Item* s = a->find(kMtAllSamplesIndependent); s && s->vt == kVtUi4) *out = s->u == 0;
    else *out = 1;
    return kSOk;
}
API HRESULT t_is_equal(Attributes* a, uint64_t other, uint32_t* flags) {
    if (!other || !flags) return kEInvalidArg;
    *flags = 0;
    Attributes* o = ours(other);
    if (!o) {
        MLOG_ONCE(g_logged_foreign, ERROR, "mf", "IMFMediaType::IsEqual with a type not made by mfplat here: E_NOTIMPL");
        return kENotImpl;
    }
    std::scoped_lock lock(a->mu, o->mu);
    auto eq = [&](const Guid& k) {
        Item* x = a->find(k);
        Item* y = o->find(k);
        return x && y && same_value(*x, *y);
    };
    if (!eq(kMtMajorType)) return kSFalse;
    uint32_t f = 1;
    if (eq(kMtSubtype)) f |= 2;
    Attributes* small = a->items.size() <= o->items.size() ? a : o;
    Attributes* big = small == a ? o : a;
    if (subset(small, big, {kMtUserData, kMtFrameRateRangeMin, kMtFrameRateRangeMax})) f |= 4;
    Item* ux = a->find(kMtUserData);
    Item* uy = o->find(kMtUserData);
    if ((!ux && !uy) || (ux && uy && same_value(*ux, *uy))) f |= 8;
    *flags = f;
    return f == 0xF ? kSOk : kSFalse;
}
API HRESULT t_get_representation(Attributes*, const Guid* repr, uint64_t* out) {
    if (out) *out = 0;
    MLOG_ONCE(g_logged_repr, ERROR, "mf", "IMFMediaType::GetRepresentation({%08X-...}) is not implemented: E_NOTIMPL",
              repr ? repr->data1 : 0);
    return kENotImpl;
}
API HRESULT t_free_representation(Attributes*, const Guid*, uint64_t) {
    MLOG_ONCE(g_logged_repr, ERROR, "mf", "IMFMediaType::FreeRepresentation is not implemented: E_NOTIMPL");
    return kENotImpl;
}

#define ATTRIBUTE_METHODS                                                                                              \
    reinterpret_cast<const void*>(&a_query_interface), reinterpret_cast<const void*>(&a_add_ref),                      \
        reinterpret_cast<const void*>(&a_release), reinterpret_cast<const void*>(&a_get_item),                         \
        reinterpret_cast<const void*>(&a_get_item_type), reinterpret_cast<const void*>(&a_compare_item),               \
        reinterpret_cast<const void*>(&a_compare), reinterpret_cast<const void*>(&a_get_uint32),                       \
        reinterpret_cast<const void*>(&a_get_uint64), reinterpret_cast<const void*>(&a_get_double),                    \
        reinterpret_cast<const void*>(&a_get_guid), reinterpret_cast<const void*>(&a_get_string_length),               \
        reinterpret_cast<const void*>(&a_get_string), reinterpret_cast<const void*>(&a_get_allocated_string),          \
        reinterpret_cast<const void*>(&a_get_blob_size), reinterpret_cast<const void*>(&a_get_blob),                   \
        reinterpret_cast<const void*>(&a_get_allocated_blob), reinterpret_cast<const void*>(&a_get_unknown),           \
        reinterpret_cast<const void*>(&a_set_item), reinterpret_cast<const void*>(&a_delete_item),                     \
        reinterpret_cast<const void*>(&a_delete_all_items), reinterpret_cast<const void*>(&a_set_uint32),              \
        reinterpret_cast<const void*>(&a_set_uint64), reinterpret_cast<const void*>(&a_set_double),                    \
        reinterpret_cast<const void*>(&a_set_guid), reinterpret_cast<const void*>(&a_set_string),                      \
        reinterpret_cast<const void*>(&a_set_blob), reinterpret_cast<const void*>(&a_set_unknown),                     \
        reinterpret_cast<const void*>(&a_lock_store), reinterpret_cast<const void*>(&a_unlock_store),                  \
        reinterpret_cast<const void*>(&a_get_count), reinterpret_cast<const void*>(&a_get_item_by_index),              \
        reinterpret_cast<const void*>(&a_copy_all_items)

const void* const kAttributesVtbl[] = {ATTRIBUTE_METHODS};
const void* const kMediaTypeVtbl[] = {
    ATTRIBUTE_METHODS,
    reinterpret_cast<const void*>(&t_get_major_type),
    reinterpret_cast<const void*>(&t_is_compressed_format),
    reinterpret_cast<const void*>(&t_is_equal),
    reinterpret_cast<const void*>(&t_get_representation),
    reinterpret_cast<const void*>(&t_free_representation),
};
static_assert(sizeof(kAttributesVtbl) / sizeof(void*) == 33);
static_assert(sizeof(kMediaTypeVtbl) / sizeof(void*) == 38);

// ---- IMFSourceResolver ----

struct Resolver {
    const void* const* vtbl;
    std::atomic<uint32_t> refs{1};
};

API HRESULT r_query_interface(Resolver* r, const Guid* riid, void** ppv) {
    if (!ppv) return kEPointer;
    *ppv = nullptr;
    if (!riid) return kEInvalidArg;
    if (*riid == kIidIUnknown || *riid == kIidIMFSourceResolver) {
        r->refs.fetch_add(1);
        *ppv = r;
        return kSOk;
    }
    return kENoInterface;
}
API uint32_t r_add_ref(Resolver* r) { return r->refs.fetch_add(1) + 1; }
API uint32_t r_release(Resolver* r) {
    const uint32_t n = r->refs.fetch_sub(1) - 1;
    if (n == 0) delete r;
    return n;
}

int hex(char16_t c) {
    if (c >= u'0' && c <= u'9') return c - u'0';
    if (c >= u'a' && c <= u'f') return c - u'a' + 10;
    if (c >= u'A' && c <= u'F') return c - u'A' + 10;
    return -1;
}

// A local path for `url`, or the scheme that is not a local file.
bool local_path(const char16_t* url, std::u16string* path, std::string* scheme) {
    std::u16string u(url);
    size_t colon = u.find(u':');
    if (colon != std::u16string::npos && colon > 1) {
        std::string s;
        for (size_t i = 0; i < colon; ++i) s.push_back(static_cast<char>(std::tolower(static_cast<int>(u[i] & 0x7f))));
        if (s != "file") {
            *scheme = s;
            return false;
        }
        std::u16string rest = u.substr(colon + 1);
        while (!rest.empty() && (rest[0] == u'/' || rest[0] == u'\\')) rest.erase(0, 1);
        std::u16string out;
        for (size_t i = 0; i < rest.size(); ++i) {
            if (rest[i] == u'%' && i + 2 < rest.size() && hex(rest[i + 1]) >= 0 && hex(rest[i + 2]) >= 0) {
                out.push_back(static_cast<char16_t>(hex(rest[i + 1]) * 16 + hex(rest[i + 2])));
                i += 2;
            } else {
                out.push_back(rest[i] == u'/' ? u'\\' : rest[i]);
            }
        }
        *path = out;
        return true;
    }
    *path = u;
    return true;
}

API HRESULT r_create_object_from_url(Resolver*, const char16_t* url, uint32_t flags, uint64_t props, uint32_t* type,
                                     uint64_t* object) {
    if (!url || !type || !object) return kEPointer;
    *object = 0;
    *type = 2;  // MF_OBJECT_INVALID
    if (!(flags & (kMfResolutionMediaSource | kMfResolutionByteStream))) return kEInvalidArg;
    (void)props;
    std::u16string path;
    std::string scheme;
    if (!local_path(url, &path, &scheme)) {
        MLOG(WARN, "mf", "CreateObjectFromURL(%s, 0x%x): scheme '%s' has no handler: MF_E_UNSUPPORTED_SCHEME",
             narrow(url).c_str(), flags, scheme.c_str());
        return kMfEUnsupportedScheme;
    }
    const FileProbe probe = g_probe.load();
    if (probe && !probe(path.c_str())) {
        MLOG(WARN, "mf", "CreateObjectFromURL(%s, 0x%x): file not found: HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)",
             narrow(url).c_str(), flags);
        return kEFileNotFound;
    }
    MLOG(ERROR, "mf",
         "CreateObjectFromURL(%s, 0x%x): no media source or byte-stream handler is implemented (the package has no "
         "Media Foundation media): MF_E_UNSUPPORTED_BYTESTREAM_TYPE",
         narrow(url).c_str(), flags);
    return kMfEUnsupportedByteStreamType;
}

API HRESULT r_create_object_from_byte_stream(Resolver*, uint64_t, const char16_t*, uint32_t, uint64_t, uint32_t* type,
                                             uint64_t* object) {
    if (type) *type = 2;
    if (object) *object = 0;
    MLOG(ERROR, "mf", "IMFSourceResolver::CreateObjectFromByteStream: no byte-stream handler is implemented: "
                      "MF_E_UNSUPPORTED_BYTESTREAM_TYPE");
    return kMfEUnsupportedByteStreamType;
}
API HRESULT r_not_implemented_async(Resolver*) {
    MLOG_ONCE(g_logged_async, ERROR, "mf",
              "IMFSourceResolver Begin/End/CancelObjectCreation (asynchronous resolution) is not implemented: E_NOTIMPL");
    return kENotImpl;
}

const void* const kResolverVtbl[] = {
    reinterpret_cast<const void*>(&r_query_interface),
    reinterpret_cast<const void*>(&r_add_ref),
    reinterpret_cast<const void*>(&r_release),
    reinterpret_cast<const void*>(&r_create_object_from_url),
    reinterpret_cast<const void*>(&r_create_object_from_byte_stream),
    reinterpret_cast<const void*>(&r_not_implemented_async),
    reinterpret_cast<const void*>(&r_not_implemented_async),
    reinterpret_cast<const void*>(&r_not_implemented_async),
    reinterpret_cast<const void*>(&r_not_implemented_async),
    reinterpret_cast<const void*>(&r_not_implemented_async),
};
static_assert(sizeof(kResolverVtbl) / sizeof(void*) == 10);

}  // namespace

void set_file_probe(FileProbe probe) { g_probe.store(probe); }

bool is_media_attributes(uint64_t object) { return ours(object) != nullptr; }

GTA6RT_GUEST_ABI HRESULT MFStartup(uint32_t version, uint32_t flags) {
    if ((version >> 16) != (kMfVersion >> 16) || (version & 0xFFFF) > (kMfVersion & 0xFFFF)) {
        MLOG(ERROR, "mf", "MFStartup(0x%08x, %u): version not supported (have 0x%08x): MF_E_BAD_STARTUP_VERSION", version,
             flags, kMfVersion);
        return kMfEBadStartupVersion;
    }
    const int32_t n = g_startups.fetch_add(1) + 1;
    MLOG_ONCE(g_logged_startup, INFO, "mf", "MFStartup(0x%08x, %s) ok (count %d)", version,
              flags == kMfStartupLite ? "MFSTARTUP_LITE" : flags == kMfStartupFull ? "MFSTARTUP_FULL" : "?", n);
    return kSOk;
}

GTA6RT_GUEST_ABI HRESULT MFShutdown() {
    int32_t n = g_startups.load();
    while (n > 0 && !g_startups.compare_exchange_weak(n, n - 1)) {
    }
    if (n <= 0) MLOG_ONCE(g_logged_unbalanced, WARN, "mf", "MFShutdown without a matching MFStartup");
    MLOG_ONCE(g_logged_shutdown, INFO, "mf", "MFShutdown (count %d)", n > 0 ? n - 1 : 0);
    return kSOk;
}

GTA6RT_GUEST_ABI int64_t MFGetSystemTime() { return static_cast<int64_t>(now_ns() / 100); }

GTA6RT_GUEST_ABI HRESULT MFCreateAttributes(uint64_t* out, uint32_t initial_size) {
    if (!out) return kEPointer;
    auto* a = new Attributes;
    a->vtbl = kAttributesVtbl;
    a->items.reserve(initial_size);
    *out = reinterpret_cast<uint64_t>(a);
    return kSOk;
}

GTA6RT_GUEST_ABI HRESULT MFCreateMediaType(uint64_t* out) {
    if (!out) return kEPointer;
    auto* a = new Attributes;
    a->vtbl = kMediaTypeVtbl;
    a->media_type = true;
    *out = reinterpret_cast<uint64_t>(a);
    return kSOk;
}

GTA6RT_GUEST_ABI HRESULT MFCreateSourceResolver(uint64_t* out) {
    if (!out) return kEPointer;
    auto* r = new Resolver;
    r->vtbl = kResolverVtbl;
    *out = reinterpret_cast<uint64_t>(r);
    return kSOk;
}

GTA6RT_GUEST_ABI HRESULT MFCreateSourceReaderFromMediaSource(uint64_t source, uint64_t attributes, uint64_t* out) {
    if (!out) return kEPointer;
    *out = 0;
    if (!source) return kEInvalidArg;
    (void)attributes;
    MLOG_ONCE(g_logged_reader, ERROR, "mf",
              "MFCreateSourceReaderFromMediaSource: no source reader is implemented (no media source can exist here): "
              "E_NOTIMPL");
    return kENotImpl;
}

}  // namespace rt::media
