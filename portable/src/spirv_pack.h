#pragma once
// A read-only memory-mapped pack of a corpus's .spv files (d3d12_vulkan/tools/build_spirv_pack.py), so a module is
// read without opening its file. Layout, little-endian:
//   header: "LSPVPAK1", u32 count, u32 manifest_bytes, u64 table_offset, u64 names_offset, then the manifest text
//   table:  count x {u64 name_offset, u32 name_bytes, u32 reserved, u64 data_offset, u64 size, i64 mtime_ns}, by name
//   names:  the modules' paths relative to the corpus, '/'-separated
// The manifest names each index file with its size and mtime ("path size mtime_ns\n"); a pack whose manifest does not
// match the corpus is not used. A module whose own size or mtime differs from its table row is read from its file.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#if defined(__APPLE__) || defined(__linux__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#define LEONIDA_SPIRV_PACK_SUPPORTED 1
#endif

namespace leonida {

class SpirvPack {
public:
    static constexpr const char* index_files[] = {"index.json",       "fspx/index.json",  "fspx_compute/index.json",
                                                  "fspx_gfx/index.json", "bvh/index.json", "exe/index.json",
                                                  "library/index.json"};

#if defined(LEONIDA_SPIRV_PACK_SUPPORTED)
    static std::shared_ptr<SpirvPack> open(const std::filesystem::path& corpus, std::string* note) {
        const char* env = std::getenv("LEONIDA_SPIRV_PACK");
        if (env && !std::strcmp(env, "0")) return {};
        const std::filesystem::path path = env && *env ? std::filesystem::path(env) : corpus / "spirv.pack";
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return {};
        struct stat st {};
        if (fstat(fd, &st) != 0 || st.st_size < 32) {
            ::close(fd);
            return {};
        }
        void* map = mmap(nullptr, std::size_t(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);
        if (map == MAP_FAILED) return {};
        auto pack = std::shared_ptr<SpirvPack>(new SpirvPack(static_cast<const std::uint8_t*>(map),
                                                             std::size_t(st.st_size), corpus));
        if (!pack->valid(note)) {
            if (note) *note = path.string() + ": " + *note + "; module files are read";
            return {};
        }
        if (note) {
            char line[64];
            std::snprintf(line, sizeof line, ": %u modules mapped", pack->count_);
            *note = path.string() + line;
        }
        return pack;
    }

    ~SpirvPack() { munmap(const_cast<std::uint8_t*>(base_), size_); }

    // The module at `relative` (a path below the corpus), or nothing when the pack lacks it or its file changed.
    std::optional<std::vector<std::uint32_t>> words(std::string_view relative) const {
        const Row* row = find(relative);
        if (!row) return std::nullopt;
        const std::size_t i = std::size_t(row - rows());
        std::uint8_t state = checked_[i].load(std::memory_order_acquire);
        if (state == 0) {
            struct stat st {};
            const std::string file = (corpus_ / std::filesystem::path(std::string(relative))).string();
            const bool same = ::stat(file.c_str(), &st) == 0 && std::uint64_t(st.st_size) == row->size &&
                              mtime_ns(st) == row->mtime_ns;
            state = same ? 1 : 2;
            checked_[i].store(state, std::memory_order_release);
        }
        if (state != 1) return std::nullopt;
        const std::uint8_t* data = base_ + row->data_offset;
        std::uint32_t magic = 0;
        if (row->size < 20 || row->size % 4) return std::vector<std::uint32_t>{};
        std::memcpy(&magic, data, 4);
        if (magic != 0x07230203) return std::vector<std::uint32_t>{};
        std::vector<std::uint32_t> out(std::size_t(row->size / 4));
        std::memcpy(out.data(), data, std::size_t(row->size));
        return out;
    }

    static std::int64_t mtime_ns(const struct stat& st) {
#if defined(__APPLE__)
        return std::int64_t(st.st_mtimespec.tv_sec) * 1000000000 + st.st_mtimespec.tv_nsec;
#else
        return std::int64_t(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
#endif
    }

    static std::string manifest(const std::filesystem::path& corpus) {
        std::string text;
        for (const char* name : index_files) {
            struct stat st {};
            if (::stat((corpus / name).c_str(), &st) != 0) continue;
            text += std::string(name) + " " + std::to_string(std::uint64_t(st.st_size)) + " " +
                    std::to_string(mtime_ns(st)) + "\n";
        }
        return text;
    }
#else
    static std::shared_ptr<SpirvPack> open(const std::filesystem::path&, std::string*) { return {}; }
    std::optional<std::vector<std::uint32_t>> words(std::string_view) const { return std::nullopt; }
#endif

private:
    struct Row {
        std::uint64_t name_offset;
        std::uint32_t name_bytes;
        std::uint32_t reserved;
        std::uint64_t data_offset;
        std::uint64_t size;
        std::int64_t mtime_ns;
    };
    static_assert(sizeof(Row) == 40);

#if defined(LEONIDA_SPIRV_PACK_SUPPORTED)
    SpirvPack(const std::uint8_t* base, std::size_t size, std::filesystem::path corpus)
        : base_(base), size_(size), corpus_(std::move(corpus)) {}

    bool valid(std::string* note) {
        const auto fail = [&](const char* why) {
            if (note) *note = why;
            return false;
        };
        if (std::memcmp(base_, "LSPVPAK1", 8)) return fail("not a SPIR-V pack");
        std::uint32_t manifest_bytes = 0;
        std::memcpy(&count_, base_ + 8, 4);
        std::memcpy(&manifest_bytes, base_ + 12, 4);
        std::memcpy(&table_offset_, base_ + 16, 8);
        std::memcpy(&names_offset_, base_ + 24, 8);
        if (32 + std::uint64_t(manifest_bytes) > size_ || table_offset_ % 8 ||
            table_offset_ + std::uint64_t(count_) * sizeof(Row) > size_ || names_offset_ > size_)
            return fail("truncated");
        const std::string_view stored(reinterpret_cast<const char*>(base_ + 32), manifest_bytes);
        if (stored != manifest(corpus_)) return fail("built from other index files (stale)");
        for (std::uint32_t i = 0; i < count_; ++i) {
            const Row& r = rows()[i];
            if (names_offset_ + r.name_offset + r.name_bytes > size_ || r.data_offset + r.size > size_)
                return fail("a row points outside the file");
        }
        checked_ = std::make_unique<std::atomic<std::uint8_t>[]>(count_);
        return true;
    }

    const Row* rows() const { return reinterpret_cast<const Row*>(base_ + table_offset_); }
    std::string_view name(const Row& r) const {
        return {reinterpret_cast<const char*>(base_ + names_offset_ + r.name_offset), r.name_bytes};
    }
    const Row* find(std::string_view relative) const {
        const Row* begin = rows();
        const Row* end = begin + count_;
        const Row* it = std::lower_bound(begin, end, relative,
                                         [&](const Row& r, std::string_view key) { return name(r) < key; });
        return it != end && name(*it) == relative ? it : nullptr;
    }

    const std::uint8_t* base_ = nullptr;
    std::size_t size_ = 0;
    std::filesystem::path corpus_;
    std::uint32_t count_ = 0;
    std::uint64_t table_offset_ = 0, names_offset_ = 0;
    std::unique_ptr<std::atomic<std::uint8_t>[]> checked_;
#endif
};

}  // namespace leonida
