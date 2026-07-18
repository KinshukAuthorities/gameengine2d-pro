#pragma once
/*
 * vk_texture.hpp — CPU pixels -> GPU-sampled VkImage.
 *
 * This is the direct Vulkan replacement for what texture_system.hpp's
 * TextureCache used to get "for free" from
 * SDL_CreateTextureFromSurface(renderer, surf): upload decoded RGBA8
 * pixels (still produced by SDL_image / the WIC loader / SDL_LoadBMP —
 * none of that decoding code changes) onto the GPU as a sampled image,
 * plus a sampler that encodes the old SDL_ScaleMode (point/bilinear)
 * filtering choice.
 *
 * Upload path (staging buffer -> device-local image) is the standard
 * Vulkan idiom: CPU-visible staging buffer, copy into it, one-shot command
 * buffer copies staging -> image with the layout transitions Vulkan
 * requires (SDL hid all of this inside SDL_UpdateTexture/CreateTextureFromSurface).
 */

#include "vk_context.hpp"
#include "vk_buffer.hpp"
#include <vector>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace vkr {

enum class FilterMode { Point, Bilinear };
enum class WrapMode   { Clamp, Repeat };   // Task 4: applied in create_sampler()

struct Texture {
    AllocatedImage image;
    VkSampler sampler = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;

    bool valid() const { return image.image != VK_NULL_HANDLE; }
};

class TextureUploader {
public:
    explicit TextureUploader(Context& ctx) : _ctx(ctx) {}

    Texture upload(const uint8_t* pixels, uint32_t w, uint32_t h,
                   FilterMode filter, bool gen_mipmaps = true,
                   WrapMode wrap = WrapMode::Clamp, bool srgb = true) {
        Texture tex;
        tex.width = w; tex.height = h;
        VkDeviceSize byte_size = (VkDeviceSize)w * h * 4;

        // Calculate full mip chain depth — log2(max_dim)+1 levels.
        // Only generate mipmaps when requested AND the image is large enough
        // to benefit (1x1 needs no mips); bilinear/trilinear only, not point.
        uint32_t mip_levels = 1;
        if (gen_mipmaps && filter == FilterMode::Bilinear)
            mip_levels = static_cast<uint32_t>(std::floor(std::log2(std::max(w, h)))) + 1;

        // 1. Staging buffer (CPU-visible).
        AllocatedBuffer staging;
        staging.create(_ctx.allocator, byte_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
        std::memcpy(staging.mapped_ptr(), pixels, (size_t)byte_size);

        // 2. Device-local image. Add TRANSFER_SRC usage so vkCmdBlitImage can
        //    read from each mip level while generating the next one.
        VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (mip_levels > 1) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        tex.image.create(_ctx.allocator, _ctx.device, w, h,
                          srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
                          usage, mip_levels);

        VkCommandBuffer cmd = _ctx.begin_one_shot();

        // 3. Transition ALL mip levels to TRANSFER_DST so we can copy into mip 0
        //    and then blit into mips 1..N.
        transition_image_layout(cmd, tex.image.image,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            0, mip_levels);   // all mip levels

        // 4. Copy staging -> mip level 0.
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { w, h, 1 };
        vkCmdCopyBufferToImage(cmd, staging.buffer, tex.image.image,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        if (mip_levels > 1) {
            // 5. Generate mips 1..N by successively blitting from the previous
            //    level. Each blit halves the dimensions; generate_mipmaps()
            //    handles all the layout transitions and the final
            //    SHADER_READ_ONLY transition for every level.
            generate_mipmaps(cmd, tex.image.image, (int32_t)w, (int32_t)h, mip_levels);
        } else {
            // No mips: just transition the single level to SHADER_READ_ONLY.
            transition_image_layout(cmd, tex.image.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        }

        _ctx.end_one_shot(cmd);
        staging.destroy(_ctx.allocator);

        tex.sampler = create_sampler(filter, mip_levels, wrap);
        return tex;
    }

    VkSampler create_sampler(FilterMode filter, uint32_t mip_levels = 1,
                              WrapMode wrap = WrapMode::Clamp) {
        VkFilter f = (filter == FilterMode::Bilinear) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        // Trilinear filtering when we have a real mip chain — smoothly
        // interpolates between mip levels, eliminating the aliasing/shimmering
        // that previously appeared on scaled-down sprites. Point-filter textures
        // keep NEAREST mip mode (pixel-art sprite preservation).
        VkSamplerMipmapMode mip_mode = (filter == FilterMode::Bilinear && mip_levels > 1)
            ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;

        // Task 4: apply the WrapMode from the texture's import settings instead
        // of hard-coding CLAMP_TO_EDGE.  Tiling backgrounds / parallax layers
        // marked "repeat" in their .meta will now actually tile.
        VkSamplerAddressMode addr = (wrap == WrapMode::Repeat)
            ? VK_SAMPLER_ADDRESS_MODE_REPEAT
            : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = f;
        sci.minFilter = f;
        sci.addressModeU = addr;
        sci.addressModeV = addr;
        sci.addressModeW = addr;
        sci.anisotropyEnable = VK_FALSE;
        sci.borderColor = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
        sci.unnormalizedCoordinates = VK_FALSE;
        sci.compareEnable = VK_FALSE;
        sci.mipmapMode = mip_mode;
        sci.minLod = 0.f;
        sci.maxLod = (float)mip_levels;

        VkSampler sampler;
        vk_check(vkCreateSampler(_ctx.device, &sci, nullptr, &sampler), "vkCreateSampler");
        return sampler;
    }

    void destroy(Texture& tex) {
        if (tex.sampler != VK_NULL_HANDLE) { vkDestroySampler(_ctx.device, tex.sampler, nullptr); tex.sampler = VK_NULL_HANDLE; }
        tex.image.destroy(_ctx.allocator, _ctx.device);
    }

private:
    Context& _ctx;
};

} // namespace vkr