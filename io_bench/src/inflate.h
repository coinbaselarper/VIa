#pragma once
#include <zlib.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#ifdef IOB_HAVE_LEONIDA
#include "shader_library.h"
#endif
#ifdef IOB_HAVE_LIBDEFLATE
#include "libdeflate.h"
#endif

namespace iob {

class Inflater {
public:
    explicit Inflater(std::string impl) : impl_(std::move(impl)) {
#ifdef IOB_HAVE_LIBDEFLATE
        ld_ = libdeflate_alloc_decompressor();
#endif
    }
    Inflater(const Inflater&) = delete;
    Inflater& operator=(const Inflater&) = delete;
    ~Inflater() {
        if (zinit_) inflateEnd(&zs_);
#ifdef IOB_HAVE_LIBDEFLATE
        libdeflate_free_decompressor(ld_);
#endif
    }

    // Inflates one zlib stream whose output size is known; fresh=true allocates the output per call like the shim does.
    bool run(const uint8_t* src, size_t n, size_t dst, bool fresh, const uint8_t*& out) {
        if (impl_ == "libz") {
            std::vector<uint8_t>& o = target(fresh, dst);
            if (!zinit_) {
                zs_ = {};
                inflateInit(&zs_);
                zinit_ = true;
            } else {
                inflateReset(&zs_);
            }
            zs_.next_in = const_cast<uint8_t*>(src);
            zs_.avail_in = static_cast<uInt>(n);
            zs_.next_out = o.data();
            zs_.avail_out = static_cast<uInt>(dst);
            const int rc = inflate(&zs_, Z_FINISH);
            out = o.data();
            return rc == Z_STREAM_END && zs_.total_out == dst;
        }
#ifdef IOB_HAVE_LIBDEFLATE
        if (impl_ == "libdeflate") {
            std::vector<uint8_t>& o = target(fresh, dst);
            size_t actual = 0;
            out = o.data();
            return libdeflate_zlib_decompress(ld_, src, n, o.data(), dst, &actual) == LIBDEFLATE_SUCCESS && actual == dst;
        }
#endif
#ifdef IOB_HAVE_LEONIDA
        if (impl_ == "leonida" || impl_ == "leonida-bytewise") {
            last_leonida_ = true;
            std::vector<uint8_t> local;
            std::vector<uint8_t>& o = fresh ? local : lvec_;
            if (fresh) o.reserve(dst);
            leonida::ZlibError why{};
            const bool ok = leonida::zlib_inflate(std::span<const uint8_t>(src, n), o, dst, &why, impl_ == "leonida") && o.size() == dst;
            if (fresh) lvec_ = std::move(local);
            out = lvec_.data();
            return ok;
        }
#endif
        return false;
    }

    std::vector<uint8_t> take() {
        std::vector<uint8_t> v = last_leonida_ ? std::move(lvec_) : std::move(buf_);
        (last_leonida_ ? lvec_ : buf_).clear();
        return v;
    }
    void give(std::vector<uint8_t>&& v) { (last_leonida_ ? lvec_ : buf_) = std::move(v); }

private:
    std::vector<uint8_t>& target(bool fresh, size_t dst) {
        if (fresh) buf_.assign(dst, 0);
        else if (buf_.size() < dst) buf_.resize(dst);
        return buf_;
    }
    std::string impl_;
    std::vector<uint8_t> buf_, lvec_;
    z_stream zs_{};
    bool zinit_ = false;
    bool last_leonida_ = false;
#ifdef IOB_HAVE_LIBDEFLATE
    libdeflate_decompressor* ld_ = nullptr;
#endif
};

}  // namespace iob
