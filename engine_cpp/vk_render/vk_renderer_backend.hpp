#pragma once
/*
 * vk_renderer_backend.hpp — Per-frame Vulkan frame loop.
 *
 * This is the glue layer between the raw Vulkan objects (Context, Swapchain,
 * SpriteBatch) and the engine's RenderSystem. It owns:
 *   - Context + Swapchain (the display pipeline)
 *   - Per-frame command buffers (one per kMaxFramesInFlight)
 *   - SpriteBatch (pipeline + dynamic vertex/index buffers)
 *   - The shader directory path
 *
 * Public API mirrors what SDL_Renderer used to provide implicitly:
 *
 *   begin_frame()  -> VkCommandBuffer   (was SDL_RenderClear implicitly)
 *   end_frame()                         (was SDL_RenderPresent)
 *   batch()        -> SpriteBatch&      (was SDL_RenderCopyEx etc.)
 *   current_extent()                    (was SDL_GetRendererOutputSize)
 *   frame_index()                       (needed by SpriteBatch::begin_frame)
 *
 * Offscreen rendering (for RenderSystem::render_to_bytes and the editor
 * viewport) goes through RenderTarget (vk_render_target.hpp) — this class
 * just provides the command buffer and the SpriteBatch to fill.
 *
 * Window resize / swapchain recreation is handled automatically inside
 * begin_frame / end_frame — callers do not need to track VK_SUBOPTIMAL or
 * VK_ERROR_OUT_OF_DATE_KHR themselves, the same way SDL_RenderPresent just
 * handled window events silently.
 */

#include "vk_context.hpp"
#include "vk_swapchain.hpp"
#include "vk_sprite_batch.hpp"
#include "vk_post_process.hpp"
#include <SDL2/SDL.h>
#include <array>
#include <memory>
#include <string>

namespace vkr {

class RendererBackend {
public:
    RendererBackend(SDL_Window* window, const std::string& shader_dir, bool vsync = false)
        : _window(window)
    {
        _ctx      = std::make_unique<Context>(window, "GameEngine2DPro");
        _swap     = std::make_unique<Swapchain>(*_ctx, window, vsync);

        // PostProcessPipeline owns its own offscreen scene render pass + framebuffer.
        // SpriteBatch must be built against that render pass (not the swapchain render
        // pass) so its pipelines are compatible with the UNORM scene attachment.
        _post_proc = std::make_unique<vkr::PostProcessPipeline>(
            *_ctx, _swap->extent, _swap->image_format, shader_dir);
        _batch = std::make_unique<SpriteBatch>(*_ctx, _post_proc->scene_render_pass(), shader_dir);
        _shader_dir = shader_dir;
        create_command_buffers();
    }

    ~RendererBackend() {
        if (_ctx && _ctx->device) vkDeviceWaitIdle(_ctx->device);
        // SpriteBatch, Swapchain, Context destroyed in reverse order by unique_ptr
    }

    RendererBackend(const RendererBackend&) = delete;
    RendererBackend& operator=(const RendererBackend&) = delete;

    Context&     ctx()   { return *_ctx; }
    SpriteBatch& batch() { return *_batch; }
    uint32_t     frame_index() const { return _frame_index; }
    VkExtent2D   current_extent() const { return _swap->extent; }

    // Exposed for ImGui_ImplVulkan_Init() (editor_main.cpp), which renders
    // its own UI directly into this same swapchain render pass/framebuffers
    // — ImGui needs to know the render pass to build a pipeline compatible
    // with it, and the image count to size its per-frame upload buffers.
    VkRenderPass swapchain_render_pass() const { return _swap->render_pass; }
    uint32_t     swapchain_image_count() const { return _swap->image_count(); }
    VkFramebuffer swapchain_framebuffer(uint32_t image_index) const { return _swap->framebuffers[image_index]; }

    // ── begin_frame ───────────────────────────────────────────────────────────
    // Acquires a swapchain image, waits for the matching in-flight fence,
    // begins recording the command buffer for this frame, and begins the
    // swapchain render pass with the given clear color.
    //
    // Returns the command buffer to record into, or VK_NULL_HANDLE when the
    // window is minimized (caller should skip rendering entirely that frame).
    VkCommandBuffer begin_frame(float r = 30.f/255, float g = 30.f/255,
                                float b = 30.f/255, float a = 1.f) {
        // Skip rendering while minimized — Vulkan forbids a zero-extent swap
        int w, h;
        SDL_GetWindowSize(_window, &w, &h);
        if (w == 0 || h == 0) return VK_NULL_HANDLE;

        uint32_t fi = _frame_index % kMaxFramesInFlight;
        vkWaitForFences(_ctx->device, 1, &_swap->in_flight[fi], VK_TRUE, UINT64_MAX);

        uint32_t image_idx;
        VkResult result = vkAcquireNextImageKHR(_ctx->device, _swap->swapchain, UINT64_MAX,
            _swap->image_available[fi], VK_NULL_HANDLE, &image_idx);

        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            recreate_swapchain();
            return begin_frame(r, g, b, a); // retry
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            vk_check(result, "vkAcquireNextImageKHR");

        _image_index = image_idx;
        vkResetFences(_ctx->device, 1, &_swap->in_flight[fi]);

        VkCommandBuffer cmd = _cmds[fi];
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk_check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer (frame)");

        // SpriteBatch needs to know the current frame slot to pick its
        // vertex/index buffer and the viewport size for push constants.
        _batch->begin_frame(fi, _swap->extent);

        // Two-pass post-process: sprites render into PostProcessPipeline's
        // owned offscreen scene render pass. end_frame composites to swapchain.
        _post_proc->begin_scene_pass(cmd);

        return cmd;
    }

    // ── end_frame ─────────────────────────────────────────────────────────────
    // Flushes the batch, ends the render pass, submits the command buffer,
    // and presents. Handles VK_SUBOPTIMAL / OUT_OF_DATE automatically.
    void end_frame(VkCommandBuffer cmd) {
        if (cmd == VK_NULL_HANDLE) return; // minimized

        // ── Pass 1: flush sprites, end the scene render pass ───────────────
        _batch->flush(cmd);
        vkCmdEndRenderPass(cmd); // ends _post_proc->scene_render_pass()
        // Scene image finalLayout = SHADER_READ_ONLY_OPTIMAL, ready for sampling.

        // ── Pass 2: composite (bloom + tonemap + grade + vignette) → swapchain ─

        VkClearValue clear{};
        clear.color = {{0.f, 0.f, 0.f, 1.f}};
        VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rbi.renderPass        = _swap->render_pass;
        rbi.framebuffer       = _swap->framebuffers[_image_index];
        rbi.renderArea.extent = _swap->extent;
        rbi.clearValueCount   = 1;
        rbi.pClearValues      = &clear;
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        _post_proc->flush_composite(cmd);
        vkCmdEndRenderPass(cmd);
        vk_check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer (frame)");

        uint32_t fi = _frame_index % kMaxFramesInFlight;
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount   = 1;
        si.pWaitSemaphores      = &_swap->image_available[fi];
        si.pWaitDstStageMask    = &wait_stage;
        si.commandBufferCount   = 1;
        si.pCommandBuffers      = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores    = &_swap->render_finished[fi];
        vk_check(vkQueueSubmit(_ctx->graphics_queue, 1, &si, _swap->in_flight[fi]),
                  "vkQueueSubmit (frame)");

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores    = &_swap->render_finished[fi];
        pi.swapchainCount     = 1;
        pi.pSwapchains        = &_swap->swapchain;
        pi.pImageIndices      = &_image_index;

        VkResult result = vkQueuePresentKHR(_ctx->present_queue, &pi);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
            recreate_swapchain();
        else
            vk_check(result, "vkQueuePresentKHR");

        _frame_index++;
    }

    // ── One-shot command buffer (for texture uploads, readbacks) ─────────────
    VkCommandBuffer begin_one_shot() { return _ctx->begin_one_shot(); }
    void            end_one_shot(VkCommandBuffer cmd) { _ctx->end_one_shot(cmd); }

    // ── Wait for all in-flight work to finish ─────────────────────────────────
    // Must be called before destroying any resource that may be in use on
    // the GPU (e.g. on hot-reload or shutdown).
    void wait_idle() { vkDeviceWaitIdle(_ctx->device); }

private:
    SDL_Window* _window;
    std::string _shader_dir;
    std::unique_ptr<Context>    _ctx;
    std::unique_ptr<Swapchain>  _swap;
    std::unique_ptr<SpriteBatch> _batch;
    std::unique_ptr<vkr::PostProcessPipeline> _post_proc; // offscreen scene + composite to swapchain

    // Per-frame command buffers (one per kMaxFramesInFlight)
    std::array<VkCommandBuffer, kMaxFramesInFlight> _cmds{};
    VkCommandPool _frame_pool = VK_NULL_HANDLE; // separate from ctx.command_pool

    uint64_t _frame_index = 0;
    uint32_t _image_index = 0;

    void create_command_buffers() {
        // Use a separate command pool for frame command buffers (reset per
        // frame) vs ctx.command_pool (used for one-shot uploads which are
        // smaller and infrequent).
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = *_ctx->queue_families.graphics;
        vk_check(vkCreateCommandPool(_ctx->device, &pci, nullptr, &_frame_pool),
                  "vkCreateCommandPool (frame)");

        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool        = _frame_pool;
        ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = (uint32_t)_cmds.size();
        vk_check(vkAllocateCommandBuffers(_ctx->device, &ai, _cmds.data()),
                  "vkAllocateCommandBuffers (frame)");
    }

    void recreate_swapchain() {
        // Wait for minimized state to pass before recreating
        int w = 0, h = 0;
        while (w == 0 || h == 0) {
            SDL_GetWindowSize(_window, &w, &h);
            SDL_WaitEvent(nullptr); // yield instead of busy-spin
        }
        vkDeviceWaitIdle(_ctx->device);
        _swap->recreate();
        // Resize the post-process scene image to match the new swapchain extent.
        // SpriteBatch pipelines are render-pass-compatible, no rebuild needed.
        _post_proc->resize(_swap->extent);
    }
};

} // namespace vkr