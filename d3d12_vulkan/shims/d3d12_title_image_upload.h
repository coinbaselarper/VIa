#pragma once

#include "xg_xs_layout.h"
#include "../src/vulkan_backend.h"
#include "../src/gpu_ledger.h"

namespace rt::d3d12 {

// A ring of upload buffers and command buffers per device, used under the title-texture registry lock.
// A whole image (including arrays and compressed mip tails) is detiled into the mapped buffer of the next slot.
// The transfer never re-enters the D3D12 queue's pre-submit callbacks.
//
// A write no longer waits for its own transfer: the caller (a title thread creating a slot-70 view) waited for the
// GPU at every texture fill, behind all earlier work on the queue, with tracker_mutex_ and submit_mutex_ held, so
// every other submission waited too (speed-26: a title thread 68-88 % inside slot 70). A slot's buffer is reused,
// resized or freed only after its last transfer completed (the slot's timeline value). The image needs no CPU wait:
// later submissions on the queue are ordered after the transfer, other queues wait for it on the GPU, and a retired
// image is destroyed through the deferred queue, after later submissions retire.
class TitleImageUpload {
public:
    VkDeviceSize capacity() const {
        VkDeviceSize total = 0;
        for (const auto& slot : slots_) total += slot.capacity;
        return total;
    }

    template<class Device>
    void release(Device& d) {
        auto& vk = d.context->vk;
        wait_all(d);
        for (auto& slot : slots_) release_buffer(d, slot);
        if (pool_) vk.vkDestroyCommandPool(d.info.device, pool_, nullptr);
        if (ready_) vk.vkDestroySemaphore(d.info.device, ready_, nullptr);
        pool_ = VK_NULL_HANDLE;
        for (auto& slot : slots_) slot.command = VK_NULL_HANDLE;
        ready_ = VK_NULL_HANDLE;
    }

    // port/: every transfer submitted so far is done. The ring submits on its own timeline, outside the D3D12
    // queues' retire tickets the deferred deletion queue waits for, so a title image is released only after this.
    template<class Device>
    bool wait_idle(Device& d) {
        for (auto& slot : slots_)
            if (!wait_slot(d, slot)) return false;
        return true;
    }

    template<class Device>
    bool write(Device& d, leonida::ResourceId id, const leonida::ResourceBinding& image,
               const xg_xs::Texture& layout, const std::uint8_t* tiled, std::vector<std::uint64_t>& columns,
               bool inspect, bool& nonzero) {
        const auto block = leonida::copy_block_format(image, VK_IMAGE_ASPECT_COLOR_BIT);
        const auto mips = layout.layout.mips;
        const auto layers = layout.array;
        if (!block.bytes || block.bytes != layout.format.bytes || !mips || mips > image.mip_levels ||
            !layers || layers > image.array_layers) return false;

        regions_.clear();
        VkDeviceSize bytes = 0;
        for (unsigned layer = 0; layer < layers; ++layer) {
            for (unsigned level = 0; level < mips; ++level) {
                const auto& mip = layout.layout.plane[0].mips[level];
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1};
                copy.imageExtent = {std::max(1u, image.width >> level), std::max(1u, image.height >> level),
                                    image.volume ? std::max(1u, image.depth >> level) : 1u};
                if (mip.width_elements != (copy.imageExtent.width + block.block - 1) / block.block ||
                    mip.height_elements != (copy.imageExtent.height + block.block - 1) / block.block ||
                    (image.volume && mip.depth != copy.imageExtent.depth)) return false;
                // Vulkan requires offsets aligned to four bytes and to the texel block size.
                const auto align = 4u * block.bytes;
                copy.bufferOffset = bytes + (align - bytes % align) % align;
                const VkDeviceSize row = VkDeviceSize(mip.width_elements) * block.bytes;
                const VkDeviceSize slice = row * mip.height_elements;
                if (slice > UINT32_MAX || (UINT64_MAX - copy.bufferOffset) / copy.imageExtent.depth < slice) return false;
                bytes = copy.bufferOffset + slice * copy.imageExtent.depth;
                regions_.push_back(copy);
            }
        }
        Slot& slot = slots_[next_];
        if (!prepare(d, slot, bytes)) return false;
        next_ = (next_ + 1) % kSlots;
        for (const auto& copy : regions_) {
            const auto level = copy.imageSubresource.mipLevel;
            const auto& mip = layout.layout.plane[0].mips[level];
            const auto row = mip.width_elements * block.bytes, slice = row * mip.height_elements;
            auto* dst = slot.mapped + copy.bufferOffset;
            xg_xs::copy_elements<false>(layout, level, copy.imageSubresource.baseArrayLayer, tiled, dst, row, slice, columns);
            if (inspect && !nonzero)
                nonzero = std::any_of(dst, dst + VkDeviceSize(slice) * copy.imageExtent.depth,
                                      [](std::uint8_t c) { return c != 0; });
        }

        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, layers};
        const auto result = d.context->write_staged_title_image(id, range, [&](const leonida::ReplayFunctions& replay,
                                                                               const leonida::LayoutPieces& pieces,
                                                                               const auto& waits) {
            auto& vk = d.context->vk;
            VkResult r = vk.vkResetCommandBuffer(slot.command, 0);
            if (r != VK_SUCCESS) return r;
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if ((r = vk.vkBeginCommandBuffer(slot.command, &begin)) != VK_SUCCESS) return r;
            // All texels of every mip/layer named in range are overwritten. The CPU no longer waits for a transfer,
            // so the barrier orders this one after all earlier work on the queue: earlier reads of the image and an
            // earlier transfer into it, still running.
            leonida::detail::image_barrier(replay, slot.command, image, range, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            vk.vkCmdCopyBufferToImage(slot.command, slot.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      static_cast<std::uint32_t>(regions_.size()), regions_.data());
            for (const auto& piece : pieces)
                leonida::detail::image_barrier(replay, slot.command, image, piece.range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    piece.layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_GENERAL : piece.layout,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            if ((r = vk.vkEndCommandBuffer(slot.command)) != VK_SUCCESS) return r;
            return submit(d, slot, waits);
        });
        return result == VK_SUCCESS;
    }

    // port/: one GPU copy per image from a buffer over the title's own pages (an import, no CPU copy), all in one
    // transfer: linear single-mip 2D images, rows `row_length` texels apart. Uses a slot's command buffer only.
    struct BufferCopy {
        leonida::ResourceId id;
        leonida::ResourceBinding image;
        VkBuffer source;
        VkDeviceSize offset;
        std::uint32_t row_length;
    };
    template<class Device>
    bool copy_buffers(Device& d, const BufferCopy* items, std::size_t count) {
        if (!count || count > 8) return false;
        Slot& slot = slots_[next_];
        if (!prepare(d, slot, 0)) return false;
        next_ = (next_ + 1) % kSlots;
        leonida::ResourceId ids[8];
        for (std::size_t i = 0; i < count; ++i) ids[i] = items[i].id;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const auto result = d.context->write_staged_title_images(ids, count, range, [&](const leonida::ReplayFunctions& replay,
                                                                                          const auto& pieces,
                                                                                          const auto& waits) {
            auto& vk = d.context->vk;
            VkResult r = vk.vkResetCommandBuffer(slot.command, 0);
            if (r != VK_SUCCESS) return r;
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if ((r = vk.vkBeginCommandBuffer(slot.command, &begin)) != VK_SUCCESS) return r;
            for (std::size_t i = 0; i < count; ++i) {
                const auto& item = items[i];
                leonida::detail::image_barrier(replay, slot.command, item.image, range, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy copy{};
                copy.bufferOffset = item.offset;
                copy.bufferRowLength = item.row_length;
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent = {item.image.width, item.image.height, 1};
                vk.vkCmdCopyBufferToImage(slot.command, item.source, item.image.image,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                for (const auto& piece : pieces[i])
                    leonida::detail::image_barrier(replay, slot.command, item.image, piece.range,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        piece.layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_GENERAL : piece.layout,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            }
            if ((r = vk.vkEndCommandBuffer(slot.command)) != VK_SUCCESS) return r;
            return submit(d, slot, waits);
        });
        return result == VK_SUCCESS;
    }

private:
    static constexpr unsigned kSlots = 4;
    struct Slot {
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        std::uint8_t* mapped = nullptr;
        VkDeviceSize capacity = 0;
        VkCommandBuffer command{};
        std::uint64_t value = 0;  // the ready_ value its last transfer signals; 0: none submitted
    };

    // Waits (outside every lock but the registry's) until the slot's last transfer has completed.
    template<class Device>
    bool wait_slot(Device& d, Slot& slot) {
        if (!slot.value || !ready_) return true;
        auto& vk = d.context->vk;
        std::uint64_t reached = 0;
        if (vk.vkGetSemaphoreCounterValue && vk.vkGetSemaphoreCounterValue(d.info.device, ready_, &reached) == VK_SUCCESS &&
            reached >= slot.value) {
            slot.value = 0;
            return true;
        }
        VkSemaphoreWaitInfo done{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        done.semaphoreCount = 1;
        done.pSemaphores = &ready_;
        done.pValues = &slot.value;
        if (vk.vkWaitSemaphores(d.info.device, &done, UINT64_MAX) != VK_SUCCESS) return false;
        slot.value = 0;
        return true;
    }

    template<class Device>
    void wait_all(Device& d) {
        for (auto& slot : slots_) wait_slot(d, slot);
    }

    template<class Device>
    void release_buffer(Device& d, Slot& slot) {
        auto& vk = d.context->vk;
        if (slot.buffer) vk.vkDestroyBuffer(d.info.device, slot.buffer, nullptr);
        if (slot.memory) vk.vkFreeMemory(d.info.device, slot.memory, nullptr);
        slot.buffer = VK_NULL_HANDLE;
        slot.memory = VK_NULL_HANDLE;
        slot.mapped = nullptr;
        slot.capacity = 0;
    }

    template<class Device>
    bool prepare(Device& d, Slot& slot, VkDeviceSize size) {
        auto& vk = d.context->vk;
        const auto dev = d.info.device;
        if (!pool_) {
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.queueFamilyIndex = d.info.queue_family;
            pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            if (vk.vkCreateCommandPool(dev, &pool, nullptr, &pool_) != VK_SUCCESS) return false;
        }
        if (!slot.command) {
            VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            alloc.commandPool = pool_;
            alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            alloc.commandBufferCount = 1;
            if (vk.vkAllocateCommandBuffers(dev, &alloc, &slot.command) != VK_SUCCESS) return false;
        }
        if (!ready_) {
            VkSemaphoreTypeCreateInfo timeline{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            timeline.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &timeline};
            if (vk.vkCreateSemaphore(dev, &info, nullptr, &ready_) != VK_SUCCESS) return false;
            for (const auto* qs : {&d.info.command_queues, &d.info.compute_queues, &d.info.copy_queues})
                for (auto q : *qs)
                    if (q && q != d.info.queue && std::find(consumers_.begin(), consumers_.end(), q) == consumers_.end())
                        consumers_.push_back(q);
        }
        // The slot's buffer and command buffer are rewritten below and in write(): its last transfer must be done.
        if (!wait_slot(d, slot)) return false;
        if (size <= slot.capacity) return true;
        release_buffer(d, slot);
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = size;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (vk.vkCreateBuffer(dev, &buffer, nullptr, &slot.buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        vk.vkGetBufferMemoryRequirements(dev, slot.buffer, &req);
        {
            const leonida::gpu_ledger::Scope ledger(leonida::gpu_ledger::staging);
            slot.memory = d.allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
        }
        void* map = nullptr;
        if (!slot.memory || vk.vkBindBufferMemory(dev, slot.buffer, slot.memory, 0) != VK_SUCCESS ||
            vk.vkMapMemory(dev, slot.memory, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) {
            release_buffer(d, slot);
            return false;
        }
        slot.mapped = static_cast<std::uint8_t*>(map);
        slot.capacity = size;
        return true;
    }

    // Under tracker_mutex_ and submit_mutex_ (write_staged_title_image): submits the transfer and the other queues'
    // GPU-side waits for it, and returns without waiting for it on the CPU.
    template<class Device, class Waits>
    VkResult submit(Device& d, Slot& slot, const Waits& waits) {
        auto& vk = d.context->vk;
        ++value_;
        wait_semaphores_.clear();
        wait_values_.clear();
        for (const auto& [semaphore, value] : waits) {
            wait_semaphores_.push_back(semaphore);
            wait_values_.push_back(value);
        }
        wait_stages_.assign(wait_semaphores_.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timeline.signalSemaphoreValueCount = 1;
        timeline.pSignalSemaphoreValues = &value_;
        timeline.waitSemaphoreValueCount = std::uint32_t(wait_values_.size());
        timeline.pWaitSemaphoreValues = wait_values_.data();
        VkSubmitInfo transfer{VK_STRUCTURE_TYPE_SUBMIT_INFO, &timeline};
        transfer.waitSemaphoreCount = std::uint32_t(wait_semaphores_.size());
        transfer.pWaitSemaphores = wait_semaphores_.data();
        transfer.pWaitDstStageMask = wait_stages_.data();
        transfer.commandBufferCount = 1;
        transfer.pCommandBuffers = &slot.command;
        transfer.signalSemaphoreCount = 1;
        transfer.pSignalSemaphores = &ready_;
        auto r = vk.vkQueueSubmit(d.info.queue, 1, &transfer, VK_NULL_HANDLE);
        if (r != VK_SUCCESS) {
            --value_;
            return r;
        }
        slot.value = value_;
        // GPU writes become visible to later submissions on every other queue. The primary queue has the image barrier.
        VkTimelineSemaphoreSubmitInfo acquire{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        acquire.waitSemaphoreValueCount = 1;
        acquire.pWaitSemaphoreValues = &value_;
        VkPipelineStageFlags stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo wait{VK_STRUCTURE_TYPE_SUBMIT_INFO, &acquire};
        wait.waitSemaphoreCount = 1;
        wait.pWaitSemaphores = &ready_;
        wait.pWaitDstStageMask = &stages;
        for (auto q : consumers_)
            if ((r = vk.vkQueueSubmit(q, 1, &wait, VK_NULL_HANDLE)) != VK_SUCCESS) return r;
        return VK_SUCCESS;
    }

    Slot slots_[kSlots];
    unsigned next_ = 0;
    VkCommandPool pool_{};
    VkSemaphore ready_{};
    std::uint64_t value_ = 0;
    std::vector<VkQueue> consumers_;
    std::vector<VkBufferImageCopy> regions_;
    std::vector<VkSemaphore> wait_semaphores_;
    std::vector<std::uint64_t> wait_values_;
    std::vector<VkPipelineStageFlags> wait_stages_;
};

}  // namespace rt::d3d12
