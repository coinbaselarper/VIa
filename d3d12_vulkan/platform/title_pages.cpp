#include "title_pages.h"

#include <mach/mach.h>
#include <mach/mach_vm.h>

#include <algorithm>

namespace gta6rt::title_pages {

namespace {

bool page(std::uint64_t va, std::uint64_t& object, std::uint64_t& offset) {
    vm_page_info_basic_data_t info{};
    mach_msg_type_number_t count = VM_PAGE_INFO_BASIC_COUNT;
    if (mach_vm_page_info(mach_task_self(), va, VM_PAGE_INFO_BASIC, reinterpret_cast<vm_page_info_t>(&info), &count) !=
            KERN_SUCCESS ||
        !info.object_id)
        return false;
    object = info.object_id;
    offset = info.offset;
    return true;
}

}  // namespace

bool physical(std::uint64_t address, std::uint64_t size, std::vector<Range>& out) {
    constexpr std::uint64_t small = 4096, slot = 65536;
    out.clear();
    if (!address || !size) return false;
    const std::uint64_t end = address + size;
    for (std::uint64_t va = address; va < end;) {
        const std::uint64_t next = std::min(end, (va + slot) & ~(slot - 1));
        std::uint64_t object = 0, offset = 0;
        if (!page(va & ~(small - 1), object, offset)) return false;
        const std::uint64_t begin = offset + (va & (small - 1));
        if (!out.empty() && out.back().object == object && out.back().end == begin)
            out.back().end = begin + (next - va);
        else
            out.push_back({object, begin, begin + (next - va)});
        va = next;
    }
    std::sort(out.begin(), out.end(), [](const Range& a, const Range& b) {
        return a.object != b.object ? a.object < b.object : a.begin < b.begin;
    });
    std::size_t kept = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (kept && out[kept - 1].object == out[i].object && out[kept - 1].end >= out[i].begin)
            out[kept - 1].end = std::max(out[kept - 1].end, out[i].end);
        else
            out[kept++] = out[i];
    }
    out.resize(kept);
    return true;
}

}  // namespace gta6rt::title_pages
