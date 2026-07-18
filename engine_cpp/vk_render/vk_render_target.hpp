#pragma once
/*
 * vk_render_target.hpp — Offscreen Vulkan render target.
 *
 * Replaces SDL_CreateTexture(..., SDL_TEXTUREACCESS_TARGET, w, h) and the
 * SDL_SetRenderTarget / SDL_RenderReadPixels path used by:
 *
 *   1. RenderSystem::render_to_bytes() — renders a scene frame off-screen and
 *      returns raw RGBA pixels to Python/editor (the ViewportRenderer path).
 *   2. _draw_sprite_tiled / _draw_sprite_sliced rotation — the old code
 *      created a temp SDL_TEXTUREACCESS_TARGET, tiled/sliced into it, then
 *      blitted it rotated. In the Vulkan port this becomes an offscreen
 *      RenderTarget for those same two draw modes.
 *
 * A RenderTarget owns:
 *   - An AllocatedImage used as a VK_IMAGE_USAGE_COLOR_ATTACHMENT for
 *     rendering and VK_IMAGE_USAGE_TRANSFER_SRC_BIT for pixel readback.
 *   - A dedicated VkRenderPass (loads CLEAR, stores COLOR_ATTACHMENT) whose
 *     final layout is VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL so the image
 *     can immediately be used as a texture in the next draw (the
 *     tiled/sliced rotation case) OR transitioned for readback (the
 *     render_to_bytes case).
 *   - A VkFramebuffer wrapping the image view.
 *   - An AllocatedBuffer (host-visible, TRANSFER_DST) for the readback path.
 *
 * Usage — render into it:
 *   target.begin(cmd, clear_color);
 *   batch.flush(cmd);           // SpriteBatch was filled before this call
 *   target.end(cmd);
 *
 * Usage — read pixels back to CPU (render_to_bytes):
 *   target.transition_for_readback(cmd);
 *   target.copy_to_readback_buffer(cmd);
 *   // submit + wait
 *   std::vector<uint8_t> px = target.read_pixels();
 *
 * Usage — sample it as a texture in the next batch (tiled/sliced rotation):
 *   // after end(), layout is already SHADER_READ_ONLY_OPTIMAL — just use
 *   // target.image_view() and an appropriate sampler in a QuadCommand.
 */

#include "vk_context.hpp"
#include "vk_buffer.hpp"
#include <vector>
#include <cstring>

namespace vkr {

class RenderTarget {
public:
    RenderTarget() = default;

    // Call once (or on resize) to allocate GPU resources.
    void create(Context& ctx, uint32_t w, uint32_t h) {
        _ctx = &ctx;
        _w = w; _h = h;

        _image.create(ctx.allocator, ctx.device, w, h,
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

        create_render_pass();
        create_framebuffer();
        create_readback_buffer();
    }

    void destroy() {
        if (!_ctx) return;
        _readback.destroy(_ctx->allocator);
        if (_framebuffer) vkDestroyFramebuffer(_ctx->device, _framebuffer, nullptr);
        if (_render_pass) vkDestroyRenderPass(_ctx->device, _render_pass, nullptr);
        _image.destroy(_ctx->allocator, _ctx->device);
        _framebuffer  = VK_NULL_HANDLE;
        _render_pass  = VK_NULL_HANDLE;
        _ctx = nullptr;
    }

    ~RenderTarget() { destroy(); }

    RenderTarget(const RenderTarget&) = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;

    uint32_t width()  const { return _w; }
    uint32_t height() const { return _h; }

    VkRenderPass  render_pass()  const { return _render_pass; }
    VkImageView   image_view()   const { return _image.view; }
    VkImage       image()        const { return _image.image; }

    // ── Frame control ─────────────────────────────────────────────────────────

    // Begin an offscreen render pass. clear_color is RGBA 0..1.
    void begin(VkCommandBuffer cmd, float r = 0.f, float g = 0.f, float b = 0.f, float a = 0.f) {
        // Transition UNDEFINED/SHADER_READ -> COLOR_ATTACHMENT
        transition_image_layout(cmd, _image.image,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

        VkClearValue clear{};
        clear.color = {{r, g, b, a}};

        VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rbi.renderPass  = _render_pass;
        rbi.framebuffer = _framebuffer;
        rbi.renderArea  = {{0, 0}, {_w, _h}};
        rbi.clearValueCount = 1;
        rbi.pClearValues    = &clear;
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{0.f, 0.f, (float)_w, (float)_h, 0.f, 1.f};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        VkRect2D sc{{0,0},{_w,_h}};
        vkCmdSetScissor(cmd, 0, 1, &sc);
    }

    void end(VkCommandBuffer cmd) {
        vkCmdEndRenderPass(cmd);
        // render pass finalLayout is already SHADER_READ_ONLY_OPTIMAL
        // (see create_render_pass), so the image is immediately usable as
        // a sampled texture after this call — no extra barrier needed for
        // the tiled/sliced-rotation blit case.
    }

    // ── Pixel readback (render_to_bytes path) ─────────────────────────────────

    // Transition from SHADER_READ_ONLY to TRANSFER_SRC so we can copy to the
    // readback buffer. Must be called after end() and before copy_to_readback_buffer().
    void transition_for_readback(VkCommandBuffer cmd) {
        transition_image_layout(cmd, _image.image,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
    }

    void copy_to_readback_buffer(VkCommandBuffer cmd) {
        VkBufferImageCopy region{};
        region.bufferOffset      = 0;
        region.bufferRowLength   = _w;
        region.bufferImageHeight = _h;
        region.imageSubresource  = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent       = {_w, _h, 1};
        vkCmdCopyImageToBuffer(cmd, _image.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            _readback.buffer, 1, &region);
    }

    // Transition back to SHADER_READ_ONLY after readback so the target can
    // be reused next frame.
    void transition_after_readback(VkCommandBuffer cmd) {
        transition_image_layout(cmd, _image.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_SHADER_READ_BIT);
    }

    // Call after GPU work is complete (queue waited/fenced) to get RGBA8 pixels.
    std::vector<uint8_t> read_pixels() const {
        size_t bytes = (size_t)_w * _h * 4;
        std::vector<uint8_t> out(bytes);
        std::memcpy(out.data(), _readback.mapped_ptr(), bytes);
        return out;
    }

private:
    Context*    _ctx = nullptr;
    uint32_t    _w = 0, _h = 0;
    AllocatedImage  _image;
    AllocatedBuffer _readback;
    VkRenderPass    _render_pass  = VK_NULL_HANDLE;
    VkFramebuffer   _framebuffer  = VK_NULL_HANDLE;

    void create_render_pass() {
        VkAttachmentDescription att{};
        att.format         = VK_FORMAT_R8G8B8A8_UNORM;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        // SHADER_READ_ONLY so the image is immediately samplable after the
        // pass ends (needed for the tiled/sliced rotation blit case).
        att.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

        VkSubpassDescription sub{};
        sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments    = &ref;

        // Src dependency: ensure any prior read (sampling) completes before
        // we write as a color attachment.
        VkSubpassDependency dep{};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ci.attachmentCount = 1;
        ci.pAttachments    = &att;
        ci.subpassCount    = 1;
        ci.pSubpasses      = &sub;
        ci.dependencyCount = 1;
        ci.pDependencies   = &dep;

        vk_check(vkCreateRenderPass(_ctx->device, &ci, nullptr, &_render_pass),
                  "vkCreateRenderPass (RenderTarget)");
    }

    void create_framebuffer() {
        VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        ci.renderPass      = _render_pass;
        ci.attachmentCount = 1;
        ci.pAttachments    = &_image.view;
        ci.width           = _w;
        ci.height          = _h;
        ci.layers          = 1;
        vk_check(vkCreateFramebuffer(_ctx->device, &ci, nullptr, &_framebuffer),
                  "vkCreateFramebuffer (RenderTarget)");
    }

    void create_readback_buffer() {
        VkDeviceSize sz = (VkDeviceSize)_w * _h * 4;
        _readback.create(_ctx->allocator, sz,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            true /* mapped */);
    }
};

} // namespace vkr