#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "loader_internal.h"

using ldr::Image;

namespace {

constexpr uint32_t kMaxTlsImages = 64;
constexpr uint32_t kNoIndex = 0xFFFFFFFFu;

struct TlsImage {
    Image* img = nullptr;
    const uint8_t* tmpl = nullptr;
    uint64_t raw = 0;
    uint64_t total = 0;
    uint32_t align = 16;
};

std::mutex g_mutex;
TlsImage* g_images[kMaxTlsImages];

const IMAGE_TLS_DIRECTORY64* tls_dir(const Image* img) {
    if (img->data_dir_count <= IMAGE_DIRECTORY_ENTRY_TLS) return nullptr;
    const auto& d = img->dirs[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!d.Size || !d.VirtualAddress) return nullptr;
    if (!ldr::range_ok(img, d.VirtualAddress, sizeof(IMAGE_TLS_DIRECTORY64))) return nullptr;
    const auto* t = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(img->base + d.VirtualAddress);
    if (!t->StartAddressOfRawData && !t->EndAddressOfRawData && !t->AddressOfIndex && !t->AddressOfCallBacks &&
        !t->SizeOfZeroFill)
        return nullptr;
    return t;
}

bool va_to_rva(const Image* img, uint64_t va, uint64_t* rva) {
    const uint64_t b = reinterpret_cast<uint64_t>(img->base);
    if (va < b || va - b >= img->size) return false;
    *rva = va - b;
    return true;
}

void* make_block(uint32_t, uint32_t, void* user) {
    const TlsImage& ti = *static_cast<const TlsImage*>(user);
    void* p = nullptr;
    const size_t a = std::max<uint32_t>(ti.align, 16);
    const size_t n = std::max<uint64_t>(ti.total, 1);
    if (posix_memalign(&p, a, (n + a - 1) & ~(a - 1))) return nullptr;
    std::memset(p, 0, n);
    if (ti.raw) std::memcpy(p, ti.tmpl, ti.raw);
    return p;
}

void free_block(void* block, void*) { std::free(block); }

}  // namespace

namespace ldr {

gta6rt_status tls_assign(Image* img, uint32_t* out_index) {
    if (img->tls_index != kNoIndex) {
        if (out_index) *out_index = img->tls_index;
        return GTA6RT_OK;
    }
    if (!img->relocated) {
        log_error("%s: tls_assign_index before relocate", img->name.c_str());
        return GTA6RT_E_INVALID_ARG;
    }
    const auto* t = tls_dir(img);
    if (!t) return GTA6RT_E_NOT_FOUND;
    if (t->EndAddressOfRawData < t->StartAddressOfRawData) {
        log_error("%s: TLS template end precedes its start", img->name.c_str());
        return GTA6RT_E_BAD_FORMAT;
    }
    const uint64_t raw = t->EndAddressOfRawData - t->StartAddressOfRawData;
    const uint64_t total = raw + t->SizeOfZeroFill;
    if (total > 0xFFFFFFFFull) {
        log_error("%s: TLS block is too large", img->name.c_str());
        return GTA6RT_E_BAD_FORMAT;
    }
    uint64_t raw_rva = 0, index_rva = 0;
    if (raw && (!va_to_rva(img, t->StartAddressOfRawData, &raw_rva) || !range_ok(img, raw_rva, raw))) {
        log_error("%s: the TLS template is outside the image", img->name.c_str());
        return GTA6RT_E_BAD_FORMAT;
    }
    if (t->AddressOfIndex) {
        if (!va_to_rva(img, t->AddressOfIndex, &index_rva) || !range_ok(img, index_rva, 4) ||
            index_rva < img->headers_size) {
            log_error("%s: the TLS index is outside the image or in the headers", img->name.c_str());
            return GTA6RT_E_BAD_FORMAT;
        }
    }
    uint32_t align = 16;
    if (const uint32_t c = (t->Characteristics >> 20) & 0xF) align = 1u << (c - 1);

    auto* ti = new TlsImage;
    ti->img = img;
    ti->tmpl = raw ? img->base + raw_rva : nullptr;
    ti->raw = raw;
    ti->total = total;
    ti->align = align;

    uint32_t idx = kNoIndex;
    {
        std::lock_guard l(g_mutex);
        // Index 0 is reserved for the executable: MSVC /GA code reads ThreadLocalStoragePointer[0] without _tls_index.
        if (!img->is_dll() && !g_images[0]) idx = 0;
        else
            for (uint32_t i = 1; i < kMaxTlsImages; ++i)
                if (!g_images[i]) {
                    idx = i;
                    break;
                }
        if (idx != kNoIndex) g_images[idx] = ti;
    }
    if (idx == kNoIndex) {
        delete ti;
        log_error("%s: all %u static TLS slots are in use", img->name.c_str(), kMaxTlsImages);
        return GTA6RT_E_NO_MEMORY;
    }
    if (auto st = ops().tls_register_image(idx, make_block, free_block, ti); st != GTA6RT_OK) {
        std::lock_guard l(g_mutex);
        g_images[idx] = nullptr;
        delete ti;
        log_error("%s: registering TLS index %u failed (%s)", img->name.c_str(), idx, gta6rt_status_name(st));
        return st;
    }
    img->tls_index = idx;
    if (index_rva) {
        const uint32_t v = idx;
        std::memcpy(img->base + index_rva, &v, 4);
    }
    if (out_index) *out_index = idx;
    log_debug("%s: static TLS index %u, template %llu bytes + %u zero fill, align %u", img->name.c_str(), idx,
              static_cast<unsigned long long>(raw), t->SizeOfZeroFill, align);
    return GTA6RT_OK;
}

void tls_release(Image* img) {
    if (img->tls_index == kNoIndex) return;
    const uint32_t idx = img->tls_index;
    img->tls_index = kNoIndex;
    ops().tls_unregister_image(idx);
    std::lock_guard l(g_mutex);
    delete g_images[idx];
    g_images[idx] = nullptr;
}

}  // namespace ldr

extern "C" {

gta6rt_status gta6rt_loader_tls_info(const gta6rt_image* img, gta6rt_tls_info* out) {
    if (!img || !out) return GTA6RT_E_INVALID_ARG;
    const auto* t = tls_dir(img);
    if (!t) return GTA6RT_E_NOT_FOUND;
    if (!img->relocated) return GTA6RT_E_INVALID_ARG;
    out->template_start = t->StartAddressOfRawData;
    out->template_size = t->EndAddressOfRawData >= t->StartAddressOfRawData
                             ? t->EndAddressOfRawData - t->StartAddressOfRawData
                             : 0;
    out->zero_fill = t->SizeOfZeroFill;
    const uint32_t c = (t->Characteristics >> 20) & 0xF;
    out->alignment = c ? 1u << (c - 1) : 0;
    out->index_address = t->AddressOfIndex;
    out->callbacks = nullptr;
    if (t->AddressOfCallBacks) {
        uint64_t rva;
        if (!va_to_rva(img, t->AddressOfCallBacks, &rva) || !ldr::range_ok(img, rva, 8)) return GTA6RT_E_BAD_FORMAT;
        const auto* cb = reinterpret_cast<const gta6rt_gva*>(img->base + rva);
        const uint64_t max = (img->size - rva) / 8;
        uint64_t n = 0;
        while (n < max && cb[n]) ++n;
        if (n == max) return GTA6RT_E_BAD_FORMAT;
        out->callbacks = cb;
    }
    return GTA6RT_OK;
}

gta6rt_status gta6rt_loader_tls_assign_index(gta6rt_image* img, uint32_t* out_index) {
    if (!img) return GTA6RT_E_INVALID_ARG;
    return ldr::tls_assign(img, out_index);
}

uint32_t gta6rt_loader_tls_index(const gta6rt_image* img) { return img ? img->tls_index : kNoIndex; }

gta6rt_status gta6rt_loader_run_tls_callbacks(gta6rt_image* img, uint32_t reason) {
    if (!img) return GTA6RT_E_INVALID_ARG;
    gta6rt_tls_info ti;
    auto st = gta6rt_loader_tls_info(img, &ti);
    if (st == GTA6RT_E_NOT_FOUND || (st == GTA6RT_OK && !ti.callbacks)) {
        if (reason == 1) img->thread_notify = true;
        if (reason == 0) img->thread_notify = false;
        return GTA6RT_OK;
    }
    if (st != GTA6RT_OK) return st;
    for (uint64_t i = 0; ti.callbacks[i]; ++i) {
        uint64_t rva;
        if (!va_to_rva(img, ti.callbacks[i], &rva)) {
            ldr::log_error("%s: TLS callback %llu (0x%llx) is outside the image", img->name.c_str(),
                           static_cast<unsigned long long>(i), static_cast<unsigned long long>(ti.callbacks[i]));
            return GTA6RT_E_BAD_FORMAT;
        }
        gta6rt_guest_args a{};
        a.args[0] = reinterpret_cast<uint64_t>(img->base);
        a.args[1] = reason;
        a.args[2] = 0;
        a.count = 3;
        gta6rt_guest_ret r{};
        if (auto cs = ldr::ops().call_guest(ti.callbacks[i], &a, &r); cs != GTA6RT_OK) return cs;
    }
    if (reason == 1) img->thread_notify = true;
    if (reason == 0) img->thread_notify = false;
    return GTA6RT_OK;
}

}  // extern "C"
