#pragma once
/*
 * vk_buffer.hpp — small VMA-backed buffer/image RAII wrappers.
 *
 * Vulkan has no equivalent of "just call SDL_CreateTexture and forget about
 * it" — every buffer/image needs explicit memory allocation, and getting
 * that wrong (leaking, double-freeing, wrong usage flags) is the single
 * most common source of Vulkan bugs. VMA handles the allocation bookkeeping;
 * these wrappers just make sure create/destroy are always paired via RAII,
 * the same guarantee the old engine's std::unique_ptr<TextureCache> gave
 * for SDL_Texture* (see ~RenderSystem, ~TextureCache::clear in the SDL2
 * version).
 */

#include "vk_context.hpp"

namespace vkr {

// ─── AllocatedBuffer ─────────────────────────────────────────────────────────
// Used for: vertex/index buffers (per-frame dynamic, sprite batch),
// staging buffers (CPU->GPU texture upload), and the readback buffer
// (GPU->CPU, replaces SDL_RenderReadPixels for render_to_bytes()).
struct AllocatedBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo info{};
    VkDeviceSize size = 0;

    void create(VmaAllocator allocator, VkDeviceSize sz, VkBufferUsageFlags usage,
                VmaMemoryUsage mem_usage, bool mapped = false) {
        size = sz;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = sz;
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo aci{};
        aci.usage = mem_usage;
        if (mapped) aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                 VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

        vk_check(vmaCreateBuffer(allocator, &bci, &aci, &buffer, &allocation, &info),
                  "vmaCreateBuffer");
    }

    void destroy(VmaAllocator allocator) {
        if (buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(allocator, buffer, allocation);
            buffer = VK_NULL_HANDLE;
            allocation = VK_NULL_HANDLE;
        }
    }

    void* mapped_ptr() const { return info.pMappedData; }
};

// ─── AllocatedImage ───────────────────────────────────────────────────────────
// Used for: every loaded texture (replaces SDL_Texture for game art), and
// offscreen render targets (replaces SDL_CreateTexture(...TARGET...) — the
// editor viewport panel and ViewportRenderer::render_to_bytes both render
// into one of these instead of an SDL render-target texture).
struct AllocatedImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    uint32_t mip_levels = 1;

    void create(VmaAllocator allocator, VkDevice device,
                uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                uint32_t mip_lvls = 1) {
        w = std::max(1u, w);
        h = std::max(1u, h);
        width = w; height = h; format = fmt; mip_levels = mip_lvls;

        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = fmt;
        ici.extent = { w, h, 1 };
        ici.mipLevels = mip_lvls;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = usage;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

        vk_check(vmaCreateImage(allocator, &ici, &aci, &image, &allocation, nullptr),
                  "vmaCreateImage");

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = fmt;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_lvls, 0, 1 };
        vk_check(vkCreateImageView(device, &vci, nullptr, &view), "vkCreateImageView (texture)");
    }

    void destroy(VmaAllocator allocator, VkDevice device) {
        if (view != VK_NULL_HANDLE) { vkDestroyImageView(device, view, nullptr); view = VK_NULL_HANDLE; }
        if (image != VK_NULL_HANDLE) { vmaDestroyImage(allocator, image, allocation); image = VK_NULL_HANDLE; }
    }
};

// Transitions an image's layout using a pipeline barrier. Vulkan requires
// images to be in the right layout for whatever the GPU is about to do with
// them (e.g. TRANSFER_DST before copying pixels in, SHADER_READ_ONLY before
// sampling) — SDL_Texture hid this completely; this is the explicit
// equivalent of what SDL did for you on every SDL_UpdateTexture/RenderCopy.
inline void transition_image_layout(VkCommandBuffer cmd, VkImage image,
                                     VkImageLayout old_layout, VkImageLayout new_layout,
                                     VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
                                     VkAccessFlags src_access, VkAccessFlags dst_access,
                                     uint32_t mip_level = 0, uint32_t mip_count = 1) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip_level, mip_count, 0, 1 };
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;

    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// Generates the full mip chain for `image` using GPU-side vkCmdBlitImage
// (hardware-accelerated downsampling). The image must have been created with
// VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT and
// mip_levels matching what was set in AllocatedImage::create(). After this
// call every mip level is in SHADER_READ_ONLY_OPTIMAL layout.
inline void generate_mipmaps(VkCommandBuffer cmd, VkImage image,
                               int32_t w, int32_t h, uint32_t mip_levels) {
    for (uint32_t i = 1; i < mip_levels; ++i) {
        // Transition mip i-1 from TRANSFER_DST (written by previous blit or
        // initial copy) to TRANSFER_SRC so we can blit from it.
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.image = image;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, i-1, 1, 0, 1 };
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        int32_t src_w = std::max(1, w >> (i-1));
        int32_t src_h = std::max(1, h >> (i-1));
        int32_t dst_w = std::max(1, w >> i);
        int32_t dst_h = std::max(1, h >> i);

        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i-1, 0, 1 };
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {src_w, src_h, 1};
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1 };
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {dst_w, dst_h, 1};

        vkCmdBlitImage(cmd,
            image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_LINEAR);

        // Transition mip i-1 to SHADER_READ_ONLY — it won't be written again.
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
    // Transition the final mip level (still in TRANSFER_DST after the last blit).
    VkImageMemoryBarrier last{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    last.image = image;
    last.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    last.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    last.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip_levels-1, 1, 0, 1 };
    last.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    last.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    last.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    last.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &last);
}

} // namespace vkr