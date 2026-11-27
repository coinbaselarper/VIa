#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

#include <gta6rt/nt_layout.h>

#include "loader_internal.h"

using ldr::Image;

namespace {

uint32_t section_prot(uint32_t ch) {
    uint32_t p = GTA6RT_PROT_READ;
    if (ch & IMAGE_SCN_MEM_WRITE) p |= GTA6RT_PROT_WRITE;
    if (ch & IMAGE_SCN_MEM_EXECUTE) p |= GTA6RT_PROT_EXEC;
    return p;
}

const IMAGE_DATA_DIRECTORY* dir(const Image* img, unsigned index) {
    static const IMAGE_DATA_DIRECTORY none{};
    if (index >= img->data_dir_count || index >= IMAGE_NUMBEROF_DIRECTORY_ENTRIES) return &none;
    return &img->dirs[index];
}

gta6rt_status bad(const std::string& name, const char* what) {
    ldr::log_error("%s: %s", name.c_str(), what);
    return GTA6RT_E_BAD_FORMAT;
}

constexpr uint64_t kGranularity = GTA6RT_GUEST_ALLOCATION_GRANULARITY;

// Windows hands out image bases on the 64 KiB allocation granularity; guest code that rounds an address down to find
// its module relies on it. The OS layer may only guarantee its own page size, so a misaligned base is moved up.
gta6rt_status reserve_any_aligned(const gta6rt_loader_host_ops& o, size_t span, void** out) {
    void* base = nullptr;
    auto st = o.vm_reserve(nullptr, span, GTA6RT_VM_IMAGE, &base);
    if (st != GTA6RT_OK || (reinterpret_cast<uint64_t>(base) & (kGranularity - 1)) == 0) {
        *out = base;
        return st;
    }
    const uint64_t cand = (reinterpret_cast<uint64_t>(base) + kGranularity - 1) & ~(kGranularity - 1);
    o.vm_release(base);
    void* moved = nullptr;
    if (o.vm_reserve(reinterpret_cast<void*>(cand), span, GTA6RT_VM_FIXED | GTA6RT_VM_IMAGE, &moved) == GTA6RT_OK) {
        *out = moved;
        return GTA6RT_OK;
    }
    ldr::log_warn("cannot place an image on the 64 KiB granularity; using a page-aligned base");
    return o.vm_reserve(nullptr, span, GTA6RT_VM_IMAGE, out);
}

gta6rt_status reserve_image(Image* img, const gta6rt_map_options* opt, const bool has_relocs) {
    const auto& o = ldr::ops();
    const size_t page = o.vm_page_size();
    const size_t span = (static_cast<size_t>(img->size) + page - 1) & ~(page - 1);
    const uint32_t flags = opt ? opt->flags : 0;
    const bool force = (flags & GTA6RT_MAP_FORCE_RELOCATE) != 0;
    const bool allow = (flags & (GTA6RT_MAP_ALLOW_RELOCATE | GTA6RT_MAP_FORCE_RELOCATE)) != 0;
    const uint64_t want = (opt && opt->base_override) ? opt->base_override : img->preferred_base;

    void* base = nullptr;
    gta6rt_status st = GTA6RT_E_ADDRESS_IN_USE;
    if (!force || (opt && opt->base_override && opt->base_override != img->preferred_base)) {
        st = o.vm_reserve(reinterpret_cast<void*>(want), span, GTA6RT_VM_FIXED | GTA6RT_VM_IMAGE, &base);
    }
    if (st != GTA6RT_OK) {
        if (!allow) {
            (st == GTA6RT_E_ADDRESS_IN_USE ? ldr::log_debug : ldr::log_error)(
                "%s: base 0x%llx unavailable (%s) and relocation not allowed", img->name.c_str(),
                static_cast<unsigned long long>(want), gta6rt_status_name(st));
            return st;
        }
        if (!has_relocs) {
            ldr::log_error("%s: preferred base unavailable and the image has no relocations", img->name.c_str());
            return GTA6RT_E_BAD_FORMAT;
        }
        void* held = nullptr;
        for (int attempt = 0; attempt < 2; ++attempt) {
            st = reserve_any_aligned(o, span, &base);
            if (st != GTA6RT_OK) break;
            if (!force || reinterpret_cast<uint64_t>(base) != img->preferred_base) break;
            held = base;
            base = nullptr;
        }
        if (held) o.vm_release(held);
        if (st != GTA6RT_OK) {
            ldr::log_error("%s: cannot reserve %zu bytes (%s)", img->name.c_str(), span, gta6rt_status_name(st));
            return st;
        }
    }
    st = o.vm_commit(base, span, GTA6RT_PROT_READ | GTA6RT_PROT_WRITE);
    if (st != GTA6RT_OK) {
        o.vm_release(base);
        ldr::log_error("%s: cannot commit %zu bytes (%s)", img->name.c_str(), span, gta6rt_status_name(st));
        return st;
    }
    img->base = static_cast<uint8_t*>(base);
    return GTA6RT_OK;
}

gta6rt_status map_common(const uint8_t* file, size_t fsize, const char* name, const char* path,
                         const gta6rt_map_options* opt, gta6rt_image** out) {
    if (!file || !out || !name) return GTA6RT_E_INVALID_ARG;
    *out = nullptr;
    const std::string nm = name;
    if (fsize < sizeof(IMAGE_DOS_HEADER)) return bad(nm, "file too small for a DOS header");
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return bad(nm, "not an MZ image");
    if (dos->e_lfanew < 0 || static_cast<uint64_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > fsize)
        return bad(nm, "the NT headers are outside the file");
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(file + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return bad(nm, "bad NT signature");
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return bad(nm, "not an x86-64 PE");
    if (nt->FileHeader.SizeOfOptionalHeader < offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory) ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return bad(nm, "not a PE32+ image");
    const auto& oh = nt->OptionalHeader;
    const uint64_t sec_off = static_cast<uint64_t>(dos->e_lfanew) + 4 + sizeof(IMAGE_FILE_HEADER) +
                             nt->FileHeader.SizeOfOptionalHeader;
    const uint64_t headers_end =
        sec_off + static_cast<uint64_t>(nt->FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
    if (headers_end > fsize) return bad(nm, "the section table runs past the end of the file");
    if (!oh.SizeOfImage || oh.SizeOfHeaders < headers_end || oh.SizeOfHeaders > oh.SizeOfImage ||
        oh.SizeOfHeaders > fsize)
        return bad(nm, "SizeOfHeaders and SizeOfImage do not fit the headers and file");
    if (oh.AddressOfEntryPoint && oh.AddressOfEntryPoint >= oh.SizeOfImage)
        return bad(nm, "the entry point is outside the image");

    auto img = std::make_unique<Image>();
    img->name = nm;
    img->path = path ? path : "";
    img->preferred_base = oh.ImageBase;
    img->size = oh.SizeOfImage;
    img->headers_size = oh.SizeOfHeaders;
    img->entry_rva = oh.AddressOfEntryPoint;
    img->machine = nt->FileHeader.Machine;
    img->subsystem = oh.Subsystem;
    img->characteristics = nt->FileHeader.Characteristics;
    img->dll_characteristics = oh.DllCharacteristics;
    img->timestamp = nt->FileHeader.TimeDateStamp;
    img->stack_reserve = oh.SizeOfStackReserve;
    img->stack_commit = oh.SizeOfStackCommit;
    img->file_size = fsize;
    img->data_dir_count = std::min<uint32_t>(oh.NumberOfRvaAndSizes, IMAGE_NUMBEROF_DIRECTORY_ENTRIES);
    const uint32_t dirs_avail =
        (nt->FileHeader.SizeOfOptionalHeader - offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory)) /
        sizeof(IMAGE_DATA_DIRECTORY);
    img->data_dir_count = std::min(img->data_dir_count, dirs_avail);
    for (uint32_t i = 0; i < img->data_dir_count; ++i) img->dirs[i] = oh.DataDirectory[i];

    const auto* sec = reinterpret_cast<const IMAGE_SECTION_HEADER*>(file + sec_off);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& s = sec[i];
        ldr::Section d{};
        std::memcpy(d.name, s.Name, 8);
        d.name[8] = 0;
        d.rva = s.VirtualAddress;
        d.vsize = s.Misc.VirtualSize;
        d.raw_size = s.SizeOfRawData;
        d.raw_offset = s.PointerToRawData;
        d.characteristics = s.Characteristics;
        if (d.rva < oh.SizeOfHeaders) return bad(nm, "a section overlaps the headers");
        const uint64_t extent = d.vsize ? d.vsize : d.raw_size;
        if (d.rva > oh.SizeOfImage || extent > oh.SizeOfImage - d.rva) return bad(nm, "a section runs past the image");
        const uint64_t n = std::min<uint64_t>(d.raw_size, extent);
        if (n && (d.raw_offset > fsize || n > fsize - d.raw_offset))
            return bad(nm, "a section reads past the end of the file");
        img->sections.push_back(d);
    }

    const bool has_relocs = dir(img.get(), IMAGE_DIRECTORY_ENTRY_BASERELOC)->Size != 0 &&
                            !(img->characteristics & IMAGE_FILE_RELOCS_STRIPPED);
    if (auto st = reserve_image(img.get(), opt, has_relocs); st != GTA6RT_OK) return st;

    std::memcpy(img->base, file, oh.SizeOfHeaders);
    for (const auto& d : img->sections) {
        const uint64_t extent = d.vsize ? d.vsize : d.raw_size;
        const uint64_t n = std::min<uint64_t>(d.raw_size, extent);
        if (n) std::memcpy(img->base + d.rva, file + d.raw_offset, n);
    }

    img->relocated = (reinterpret_cast<uint64_t>(img->base) == img->preferred_base);
    ldr::registry_add(img.get());
    ldr::log_debug("%s: mapped at %p size 0x%x (preferred 0x%llx%s)", img->name.c_str(),
                   static_cast<void*>(img->base), img->size, static_cast<unsigned long long>(img->preferred_base),
                   img->relocated ? "" : ", needs relocation");
    *out = img.release();
    return GTA6RT_OK;
}

}  // namespace

namespace ldr {

bool range_ok(const Image* img, uint64_t rva, uint64_t len) { return rva <= img->size && len <= img->size - rva; }

const IMAGE_NT_HEADERS64* nt_headers(const Image* img) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(img->base);
    return reinterpret_cast<const IMAGE_NT_HEADERS64*>(img->base + dos->e_lfanew);
}

gta6rt_status parse_exports(Image* img) {
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    if (img->exports_parsed) return GTA6RT_OK;
    const auto* d = dir(img, IMAGE_DIRECTORY_ENTRY_EXPORT);
    if (!d->Size) {
        img->exports_parsed = true;
        return GTA6RT_OK;
    }
    if (!range_ok(img, d->VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY))) return bad(img->name, "export directory outside the image");
    const auto* ed = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(img->base + d->VirtualAddress);
    if (!range_ok(img, ed->AddressOfFunctions, uint64_t(ed->NumberOfFunctions) * 4) ||
        !range_ok(img, ed->AddressOfNames, uint64_t(ed->NumberOfNames) * 4) ||
        !range_ok(img, ed->AddressOfNameOrdinals, uint64_t(ed->NumberOfNames) * 2))
        return bad(img->name, "an export table runs past the image");
    const auto* funcs = reinterpret_cast<const uint32_t*>(img->base + ed->AddressOfFunctions);
    const auto* names = reinterpret_cast<const uint32_t*>(img->base + ed->AddressOfNames);
    const auto* ords = reinterpret_cast<const uint16_t*>(img->base + ed->AddressOfNameOrdinals);
    const uint64_t db = d->VirtualAddress, de = db + d->Size;

    auto read_string = [&](uint32_t rva, std::string& s) {
        if (rva >= img->size) return false;
        const char* p = reinterpret_cast<const char*>(img->base + rva);
        const void* end = std::memchr(p, 0, img->size - rva);
        if (!end) return false;
        s.assign(p, static_cast<const char*>(end));
        return true;
    };
    auto make = [&](uint32_t rva, uint32_t ordinal, Export& e) {
        e.rva = rva;
        e.ordinal = ordinal;
        if (rva >= db && rva < de) {
            if (!read_string(rva, e.forwarder)) return false;
            ++img->export_forwarders;
        } else if (rva >= img->size) {
            return false;
        }
        return true;
    };
    img->export_forwarders = 0;
    for (uint32_t i = 0; i < ed->NumberOfFunctions; ++i) {
        if (!funcs[i]) continue;
        Export e;
        if (!make(funcs[i], ed->Base + i, e)) return bad(img->name, "an export points outside the image");
        img->exports_by_ordinal[ed->Base + i] = std::move(e);
    }
    for (uint32_t i = 0; i < ed->NumberOfNames; ++i) {
        if (ords[i] >= ed->NumberOfFunctions) return bad(img->name, "an export name points past the address table");
        std::string n;
        if (!read_string(names[i], n)) return bad(img->name, "an export name is outside the image");
        auto it = img->exports_by_ordinal.find(ed->Base + ords[i]);
        if (it == img->exports_by_ordinal.end()) continue;
        it->second.named = true;
        img->exports_by_name[n] = it->second;
    }
    img->exports_parsed = true;
    return GTA6RT_OK;
}

const Export* find_export(Image* img, const char* name, uint32_t ordinal) {
    if (parse_exports(img) != GTA6RT_OK) return nullptr;
    if (name) {
        auto it = img->exports_by_name.find(name);
        return it == img->exports_by_name.end() ? nullptr : &it->second;
    }
    auto it = img->exports_by_ordinal.find(ordinal);
    return it == img->exports_by_ordinal.end() ? nullptr : &it->second;
}

gta6rt_status exception_table(Image* img, const gta6rt_runtime_function** table, uint32_t* count) {
    const auto* d = dir(img, IMAGE_DIRECTORY_ENTRY_EXCEPTION);
    const uint32_t n = d->Size / sizeof(gta6rt_runtime_function);
    *table = nullptr;
    *count = 0;
    if (!n) return GTA6RT_E_NOT_FOUND;
    if (!range_ok(img, d->VirtualAddress, uint64_t(n) * sizeof(gta6rt_runtime_function)))
        return bad(img->name, "the exception directory runs past the image");
    const auto* t = reinterpret_cast<const gta6rt_runtime_function*>(img->base + d->VirtualAddress);
    uint32_t prev_end = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const auto& f = t[i];
        if (f.end_rva < f.begin_rva || f.end_rva > img->size) {
            ldr::log_error("%s: unwind entry %u covers code outside the image", img->name.c_str(), i);
            return GTA6RT_E_BAD_FORMAT;
        }
        if (f.begin_rva < prev_end) {
            ldr::log_error("%s: unwind entry %u is out of order or overlaps the one before it", img->name.c_str(), i);
            return GTA6RT_E_BAD_FORMAT;
        }
        const uint32_t info = f.unwind_info_rva & ~1u;
        if (info > img->size || img->size - info < 4) {
            ldr::log_error("%s: unwind entry %u has unwind data outside the image", img->name.c_str(), i);
            return GTA6RT_E_BAD_FORMAT;
        }
        prev_end = f.end_rva;
    }
    *table = t;
    *count = n;
    return GTA6RT_OK;
}

}  // namespace ldr

extern "C" {

gta6rt_status gta6rt_loader_map_file(const char* path_utf8, const gta6rt_map_options* opt, gta6rt_image** out) {
    if (!path_utf8 || !out) return GTA6RT_E_INVALID_ARG;
    const void* data = nullptr;
    size_t size = 0;
    auto st = ldr::ops().file_map_readonly(path_utf8, &data, &size);
    if (st != GTA6RT_OK) {
        ldr::log_error("cannot read %s (%s)", path_utf8, gta6rt_status_name(st));
        return st;
    }
    const char* slash = std::strrchr(path_utf8, '/');
    st = map_common(static_cast<const uint8_t*>(data), size, slash ? slash + 1 : path_utf8, path_utf8, opt, out);
    ldr::ops().file_unmap(data, size);
    return st;
}

gta6rt_status gta6rt_loader_map_memory(const void* file, size_t size, const char* name, const gta6rt_map_options* opt,
                                       gta6rt_image** out) {
    return map_common(static_cast<const uint8_t*>(file), size, name, nullptr, opt, out);
}

gta6rt_status gta6rt_loader_info(const gta6rt_image* img, gta6rt_image_info* out) {
    if (!img || !out) return GTA6RT_E_INVALID_ARG;
    out->base = reinterpret_cast<gta6rt_gva>(img->base);
    out->preferred_base = img->preferred_base;
    out->size = img->size;
    out->entry = img->entry_rva ? out->base + img->entry_rva : 0;
    out->stack_reserve = img->stack_reserve;
    out->stack_commit = img->stack_commit;
    out->machine = img->machine;
    out->subsystem = img->subsystem;
    out->characteristics = img->characteristics;
    out->dll_characteristics = img->dll_characteristics;
    out->section_count = static_cast<uint32_t>(img->sections.size());
    out->timestamp = img->timestamp;
    out->path_utf8 = img->path.c_str();
    return GTA6RT_OK;
}

gta6rt_status gta6rt_loader_section(const gta6rt_image* img, uint32_t index, gta6rt_section_info* out) {
    if (!img || !out) return GTA6RT_E_INVALID_ARG;
    if (index >= img->sections.size()) return GTA6RT_E_NOT_FOUND;
    const auto& s = img->sections[index];
    std::memcpy(out->name, s.name, 9);
    out->rva = s.rva;
    out->virtual_size = s.vsize;
    out->raw_size = s.raw_size;
    out->raw_offset = s.raw_offset;
    out->characteristics = s.characteristics;
    return GTA6RT_OK;
}

gta6rt_status gta6rt_loader_relocate(gta6rt_image* img) {
    if (!img) return GTA6RT_E_INVALID_ARG;
    if (img->relocated) return GTA6RT_OK;
    const int64_t delta = reinterpret_cast<int64_t>(img->base) - static_cast<int64_t>(img->preferred_base);
    const auto* d = dir(img, IMAGE_DIRECTORY_ENTRY_BASERELOC);
    if (!d->Size || (img->characteristics & IMAGE_FILE_RELOCS_STRIPPED))
        return bad(img->name, "the image has no relocations and is not at its base");
    if (!ldr::range_ok(img, d->VirtualAddress, d->Size)) return bad(img->name, "the relocation table runs past the image");

    const auto* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(img->base + d->VirtualAddress);
    const auto* end = reinterpret_cast<const uint8_t*>(block) + d->Size;
    uint64_t applied = 0;
    while (reinterpret_cast<const uint8_t*>(block) < end) {
        const size_t remaining = static_cast<size_t>(end - reinterpret_cast<const uint8_t*>(block));
        if (remaining < sizeof(IMAGE_BASE_RELOCATION) || block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) ||
            block->SizeOfBlock > remaining)
            return bad(img->name, "a relocation block does not fit the relocation table");
        const auto* entries = reinterpret_cast<const uint16_t*>(block + 1);
        const size_t count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / 2;
        for (size_t i = 0; i < count; ++i) {
            const uint16_t type = entries[i] >> 12, off = entries[i] & 0xFFF;
            if (type == IMAGE_REL_BASED_ABSOLUTE) continue;
            const uint64_t rva = static_cast<uint64_t>(block->VirtualAddress) + off;
            if (rva < img->headers_size) return bad(img->name, "a relocation lands in the headers");
            if (type == IMAGE_REL_BASED_DIR64) {
                if (!ldr::range_ok(img, rva, 8)) return bad(img->name, "a relocation runs past the image");
                uint64_t v;
                std::memcpy(&v, img->base + rva, 8);
                v += static_cast<uint64_t>(delta);
                std::memcpy(img->base + rva, &v, 8);
            } else if (type == IMAGE_REL_BASED_HIGHLOW) {
                if (!ldr::range_ok(img, rva, 4)) return bad(img->name, "a relocation runs past the image");
                uint32_t v;
                std::memcpy(&v, img->base + rva, 4);
                const int64_t moved = static_cast<int64_t>(v) + delta;
                if (moved < 0 || moved > 0xFFFFFFFFll)
                    return bad(img->name, "a 32-bit (HIGHLOW) relocation does not fit at this base");
                v = static_cast<uint32_t>(moved);
                std::memcpy(img->base + rva, &v, 4);
            } else {
                ldr::log_error("%s: unsupported relocation type %u", img->name.c_str(), type);
                return GTA6RT_E_BAD_FORMAT;
            }
            ++applied;
        }
        block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(reinterpret_cast<const uint8_t*>(block) + block->SizeOfBlock);
    }
    img->reloc_applied = applied;
    img->relocated = true;
    return GTA6RT_OK;
}

gta6rt_status gta6rt_loader_patch_rva(gta6rt_image* img, uint32_t rva, const void* bytes, size_t size) {
    if (!img || !bytes) return GTA6RT_E_INVALID_ARG;
    if (!ldr::range_ok(img, rva, size)) return GTA6RT_E_INVALID_ARG;
    if (!img->finalized) {
        std::memcpy(img->base + rva, bytes, size);
        return GTA6RT_OK;
    }
    return ldr::ops().patch_code(reinterpret_cast<gta6rt_gva>(img->base) + rva, bytes, size);
}

gta6rt_status gta6rt_loader_file_offset_to_rva(const gta6rt_image* img, uint64_t file_offset, uint32_t* out_rva) {
    if (!img || !out_rva) return GTA6RT_E_INVALID_ARG;
    if (file_offset < img->headers_size) {
        *out_rva = static_cast<uint32_t>(file_offset);
        return GTA6RT_OK;
    }
    for (const auto& s : img->sections) {
        if (file_offset >= s.raw_offset && file_offset - s.raw_offset < s.raw_size) {
            *out_rva = s.rva + static_cast<uint32_t>(file_offset - s.raw_offset);
            return GTA6RT_OK;
        }
    }
    return GTA6RT_E_NOT_FOUND;
}

uint32_t gta6rt_loader_rva_section_flags(const gta6rt_image* img, uint32_t rva) {
    if (!img) return 0;
    for (const auto& s : img->sections) {
        const uint32_t ext = s.vsize ? s.vsize : s.raw_size;
        if (rva >= s.rva && rva - s.rva < ext) return s.characteristics;
    }
    return 0;
}

gta6rt_status gta6rt_loader_finalize_protections(gta6rt_image* img) {
    if (!img) return GTA6RT_E_INVALID_ARG;
    if (!img->relocated) return GTA6RT_E_INVALID_ARG;
    const auto& o = ldr::ops();
    const size_t page = o.vm_page_size();
    const size_t span = (static_cast<size_t>(img->size) + page - 1) & ~(page - 1);
    const size_t npages = span / page;
    std::vector<uint8_t> prot(npages, static_cast<uint8_t>(GTA6RT_PROT_READ));
    for (const auto& s : img->sections) {
        const uint64_t ext = std::min<uint64_t>(s.vsize ? s.vsize : s.raw_size, img->size - s.rva);
        if (!ext) continue;
        const uint8_t p = static_cast<uint8_t>(section_prot(s.characteristics));
        const size_t first = s.rva / page, last = (s.rva + ext - 1) / page;
        for (size_t i = first; i <= last && i < npages; ++i) prot[i] |= p;
    }
    size_t i = 0;
    while (i < npages) {
        size_t j = i + 1;
        while (j < npages && prot[j] == prot[i]) ++j;
        auto st = o.vm_protect(img->base + i * page, (j - i) * page, prot[i], nullptr);
        if (st != GTA6RT_OK) {
            ldr::log_error("%s: protect of pages %zu..%zu failed (%s)", img->name.c_str(), i, j, gta6rt_status_name(st));
            return st;
        }
        i = j;
    }
    o.flush_icache(reinterpret_cast<gta6rt_gva>(img->base), img->size);
    img->finalized = true;
    return GTA6RT_OK;
}

gta6rt_status gta6rt_loader_exception_table(const gta6rt_image* img, const void** out_table, uint32_t* out_count) {
    if (!img || !out_table || !out_count) return GTA6RT_E_INVALID_ARG;
    const gta6rt_runtime_function* t = nullptr;
    auto st = ldr::exception_table(const_cast<Image*>(img), &t, out_count);
    *out_table = t;
    return st;
}

gta6rt_gva gta6rt_loader_get_export(const gta6rt_image* cimg, const char* name, uint32_t ordinal) {
    auto* img = const_cast<Image*>(cimg);
    if (!img) return 0;
    const ldr::Export* e = ldr::find_export(img, name, ordinal);
    if (!e) return 0;
    if (!e->forwarder.empty()) return ldr::resolve_forwarder(img, e->forwarder);
    return reinterpret_cast<gta6rt_gva>(img->base) + e->rva;
}

gta6rt_gva gta6rt_loader_entry(const gta6rt_image* img) {
    return img && img->entry_rva ? reinterpret_cast<gta6rt_gva>(img->base) + img->entry_rva : 0;
}

gta6rt_status gta6rt_loader_enumerate_exports(const gta6rt_image* cimg, gta6rt_export_visitor fn, void* user) {
    auto* img = const_cast<Image*>(cimg);
    if (!img || !fn) return GTA6RT_E_INVALID_ARG;
    if (auto st = ldr::parse_exports(img); st != GTA6RT_OK) return st;
    for (const auto& [name, e] : img->exports_by_name)
        if (fn(user, name.c_str(), e.ordinal, e.rva, e.forwarder.empty() ? nullptr : e.forwarder.c_str()))
            return GTA6RT_OK;
    for (const auto& [ord, e] : img->exports_by_ordinal)
        if (!e.named && fn(user, nullptr, ord, e.rva, e.forwarder.empty() ? nullptr : e.forwarder.c_str()))
            return GTA6RT_OK;
    return GTA6RT_OK;
}

gta6rt_status gta6rt_loader_stats(const gta6rt_image* cimg, gta6rt_image_stats* out) {
    auto* img = const_cast<Image*>(cimg);
    if (!img || !out) return GTA6RT_E_INVALID_ARG;
    *out = {};
    out->section_count = static_cast<uint32_t>(img->sections.size());
    out->relocated = reinterpret_cast<uint64_t>(img->base) != img->preferred_base;
    out->reloc_applied = img->reloc_applied;
    if (const auto* d = dir(img, IMAGE_DIRECTORY_ENTRY_BASERELOC); d->Size && ldr::range_ok(img, d->VirtualAddress, d->Size)) {
        const auto* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(img->base + d->VirtualAddress);
        const auto* end = reinterpret_cast<const uint8_t*>(block) + d->Size;
        while (reinterpret_cast<const uint8_t*>(block) + sizeof(IMAGE_BASE_RELOCATION) <= end && block->SizeOfBlock >= 8 &&
               reinterpret_cast<const uint8_t*>(block) + block->SizeOfBlock <= end) {
            const auto* e = reinterpret_cast<const uint16_t*>(block + 1);
            const size_t n = (block->SizeOfBlock - 8) / 2;
            for (size_t i = 0; i < n; ++i) {
                switch (e[i] >> 12) {
                case IMAGE_REL_BASED_ABSOLUTE: ++out->reloc_padding; break;
                case IMAGE_REL_BASED_DIR64: ++out->reloc_dir64; ++out->reloc_entries; break;
                case IMAGE_REL_BASED_HIGHLOW: ++out->reloc_highlow; ++out->reloc_entries; break;
                default: ++out->reloc_entries; break;
                }
            }
            block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(reinterpret_cast<const uint8_t*>(block) + block->SizeOfBlock);
        }
    }
    struct Acc {
        gta6rt_image_stats* s;
        std::unordered_map<std::string, int> dlls;
    } acc{out, {}};
    gta6rt_loader_enumerate_imports(
        img,
        [](void* u, const gta6rt_import_ref* r, uint64_t) {
            auto* a = static_cast<Acc*>(u);
            ++a->s->import_total;
            if (!r->name) ++a->s->import_by_ordinal;
            if (r->delay) ++a->s->import_delay;
            a->dlls[ldr::lower(r->dll)] = 1;
            return 0;
        },
        &acc);
    out->import_dlls = static_cast<uint32_t>(acc.dlls.size());
    if (ldr::parse_exports(img) == GTA6RT_OK) {
        out->export_total = static_cast<uint32_t>(img->exports_by_ordinal.size());
        out->export_forwarders = img->export_forwarders;
    }
    const gta6rt_runtime_function* t;
    uint32_t n = 0;
    if (ldr::exception_table(img, &t, &n) == GTA6RT_OK) out->exception_entries = n;
    gta6rt_tls_info ti;
    out->tls_index = img->tls_index;
    if (gta6rt_loader_tls_info(img, &ti) == GTA6RT_OK) {
        out->tls_present = 1;
        if (ti.callbacks)
            while (ti.callbacks[out->tls_callbacks]) ++out->tls_callbacks;
    }
    return GTA6RT_OK;
}

void gta6rt_loader_unmap(gta6rt_image* img) {
    if (!img) return;
    ldr::package_unmap_hook(img);
    ldr::tls_release(img);
    ldr::registry_remove(img);
    if (img->base) ldr::ops().vm_release(img->base);
    delete img;
}

}  // extern "C"
