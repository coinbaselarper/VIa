#pragma once

#include <cstdint>
#include <vector>

namespace gta6rt::title_pages {

// Which memory the process's bytes [address, address + size) are: xmem maps one pool slot (64 KiB) at every window
// page that shows it, so two ranges that share VM object pages are the same physical memory, the way the title's
// render targets share it. The ranges come back sorted by object and offset, adjacent ones merged; false when a page
// of the range is not mapped.
struct Range {
    std::uint64_t object = 0, begin = 0, end = 0;
};
bool physical(std::uint64_t address, std::uint64_t size, std::vector<Range>& out);

}  // namespace gta6rt::title_pages
