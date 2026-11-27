#pragma once

#include <vulkan/vulkan.h>

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace leonida {

// Only optimal-tiled, non-sparse images go in a page. A slot is returned after the owning
// image is destroyed by the device's normal GPU retirement queue.
struct TitleImagePage {
    VkDevice device{};
    VkDeviceMemory memory{};
    PFN_vkFreeMemory free_memory{};
    std::uint32_t type = 0, capacity = 0, allocated = 0;
    std::mutex mutex;
    std::vector<std::uint32_t> returned;
    std::uint64_t requested = 0;  // port/: bytes the live images asked for (their stride is the slot)

    std::uint32_t live() {
        std::lock_guard lock(mutex);
        return allocated - std::uint32_t(returned.size());
    }

    ~TitleImagePage() {
        if (memory) free_memory(device, memory, nullptr);
    }

    bool take(std::uint32_t& slot, std::uint64_t bytes) {
        std::lock_guard lock(mutex);
        if (!returned.empty()) {
            slot = returned.back();
            returned.pop_back();
        } else if (allocated < capacity) {
            slot = allocated++;
        } else {
            return false;
        }
        requested += bytes;
        return true;
    }

    void release(std::uint32_t slot, std::uint64_t bytes) {
        std::lock_guard lock(mutex);
        returned.push_back(slot);  // the page reserves capacity at creation
        requested -= bytes;
    }
};

struct PooledTitleImage {
    PooledTitleImage() = default;
    PooledTitleImage(const PooledTitleImage&) = delete;
    PooledTitleImage& operator=(const PooledTitleImage&) = delete;

    std::shared_ptr<TitleImagePage> page;
    std::uint32_t slot = 0;
    std::uint64_t bytes = 0;
    VkImage image{};
    PFN_vkDestroyImage destroy_image{};
    // port/: an image bound into memory it does not own a slot of (a render-target alias extent, rt_alias.h).
    VkDevice device{};
    std::shared_ptr<void> keep;

    ~PooledTitleImage() {
        if (image) destroy_image(page ? page->device : device, image, nullptr);
        if (page) page->release(slot, bytes);
    }
};

class TitleImagePool {
public:
    // The caller has already excluded dedicated-only images. All allocations in one page have
    // identical stride, compatible memory types and non-overlapping, individually aligned ranges.
    template<class Allocate>
    std::unique_ptr<PooledTitleImage> bind(VkDevice device, VkImage image, const VkMemoryRequirements& req,
                                           PFN_vkBindImageMemory bind_image, PFN_vkDestroyImage destroy_image,
                                           PFN_vkFreeMemory free_memory, Allocate allocate) {
        constexpr VkDeviceSize largest_slot = VkDeviceSize{2} << 20;
        const VkDeviceSize needed = std::max(req.size, req.alignment);
        if (!needed || needed > largest_slot || !req.memoryTypeBits) return {};
        // Eight size classes per power of two keep padding below one eighth (besides required alignment).
        const VkDeviceSize quantum = std::max(req.alignment, std::max(VkDeviceSize{1}, std::bit_floor(needed) / 8));
        const VkDeviceSize stride = (needed + quantum - 1) & ~(quantum - 1);
        // port/: on MoltenVK a page is an MTLHeap of its full size for as long as one slot lives, so under the
        // title's streaming churn pages stay mostly empty (s3l5-r7 at the world phase: 227 pages, 1,055 MiB, for
        // 476 MiB of images). Smaller pages bound that: 2 MiB, or 64 slots of a small class
        // (LEONIDA_TITLE_POOL_PAGE_KB; 8192 was the old size).
        static const VkDeviceSize page_cap = [] {
            const char* v = std::getenv("LEONIDA_TITLE_POOL_PAGE_KB");
            const unsigned long long kb = v && *v ? std::strtoull(v, nullptr, 10) : 2048ull;
            return VkDeviceSize(std::clamp<unsigned long long>(kb, 64, 65536)) << 10;
        }();
        // port/: the fullest page with room takes the image, so the emptiest pages drain and are freed, and a new page
        // holds a quarter of what its class has live (4 to 64 slots): a class with few images keeps a small tail of
        // empty slots instead of a 2 MiB page (LEONIDA_TITLE_POOL_FIT=0: first page with room, 64 slots).
        static const bool fit = [] {
            const char* v = std::getenv("LEONIDA_TITLE_POOL_FIT");
            return !v || std::strcmp(v, "0");
        }();
        auto allocation = std::make_unique<PooledTitleImage>();
        allocation->bytes = req.size;
        std::lock_guard lock(mutex_);
        auto& pages = classes_[stride];
        std::erase_if(pages, [](const auto& page) { return page.expired(); });
        std::uint64_t class_live = 0;
        for (;;) {
            std::shared_ptr<TitleImagePage> best;
            std::uint32_t best_live = 0;
            class_live = 0;
            for (const auto& weak : pages) {
                auto page = weak.lock();
                if (!page) continue;
                const std::uint32_t live = page->live();
                class_live += live;
                if (!(req.memoryTypeBits & (1u << page->type)) || live >= page->capacity) continue;
                if (!best || (fit && live > best_live)) best = std::move(page), best_live = live;
                if (!fit && best) break;
            }
            if (!best) break;
            if (best->take(allocation->slot, allocation->bytes)) {
                allocation->page = std::move(best);
                break;
            }
        }
        if (!allocation->page) {
            const std::uint64_t slots =
                fit ? std::clamp<std::uint64_t>(std::bit_ceil(std::max<std::uint64_t>(1, class_live / 4)), 4, 64) : 64;
            const VkDeviceSize page_size = std::max(stride, std::min<VkDeviceSize>(page_cap, slots * stride));
            auto page = std::make_shared<TitleImagePage>();
            page->device = device;
            page->free_memory = free_memory;
            page->capacity = static_cast<std::uint32_t>(page_size / stride);
            page->returned.reserve(page->capacity);
            page->memory = allocate(page_size, page->type);
            if (!page->memory) return {};
            pages.push_back(page);
            page->take(allocation->slot, allocation->bytes);
            allocation->page = std::move(page);
        }
        if (bind_image(device, image, allocation->page->memory, stride * allocation->slot) != VK_SUCCESS) return {};
        allocation->destroy_image = destroy_image;
        allocation->image = image;
        return allocation;
    }

    struct Stats {
        std::uint64_t pages = 0, page_bytes = 0, live_slots = 0, live_bytes = 0, requested_bytes = 0, empty_pages = 0;
    };
    Stats stats() {
        Stats s;
        std::lock_guard lock(mutex_);
        for (const auto& [stride, pages] : classes_)
            for (const auto& weak : pages)
                if (auto page = weak.lock()) {
                    std::lock_guard page_lock(page->mutex);
                    ++s.pages;
                    s.page_bytes += std::uint64_t(page->capacity) * stride;
                    const std::uint64_t live = page->allocated - page->returned.size();
                    s.live_slots += live;
                    s.live_bytes += live * stride;
                    s.requested_bytes += page->requested;
                    s.empty_pages += live == 0;
                }
        return s;
    }

private:
    std::mutex mutex_;
    std::map<VkDeviceSize, std::vector<std::weak_ptr<TitleImagePage>>> classes_;
};

}  // namespace leonida
