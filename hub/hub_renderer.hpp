#pragma once

// Small Vulkan presenter for the Hub.  It deliberately uses only Context and
// Swapchain: the Hub must not start the game/editor render runtime just to
// show a project picker.
#include "vk_render/vk_context.hpp"
#include "vk_render/vk_swapchain.hpp"

#include <SDL2/SDL.h>

#include <array>
#include <vector>

namespace gamehub {

class HubRenderer {
public:
    explicit HubRenderer(SDL_Window* window)
        : _window(window), _context(window, "GameEngine Hub"), _swapchain(_context, window, true) {
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = _context.command_pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = vkr::kMaxFramesInFlight;
        vkr::vk_check(vkAllocateCommandBuffers(_context.device, &allocate, _commands.data()),
                      "allocate Hub command buffers");
        _images_in_flight.assign(_swapchain.image_count(), VK_NULL_HANDLE);
    }

    ~HubRenderer() {
        if (_context.device) vkDeviceWaitIdle(_context.device);
        if (_context.device && _commands[0] != VK_NULL_HANDLE)
            vkFreeCommandBuffers(_context.device, _context.command_pool,
                                 static_cast<uint32_t>(_commands.size()), _commands.data());
    }

    HubRenderer(const HubRenderer&) = delete;
    HubRenderer& operator=(const HubRenderer&) = delete;

    vkr::Context& context() { return _context; }
    vkr::Swapchain& swapchain() { return _swapchain; }

    VkCommandBuffer begin_frame(float r, float g, float b, float a) {
        const uint32_t frame = _frame_index;
        vkWaitForFences(_context.device, 1, &_swapchain.in_flight[frame], VK_TRUE, UINT64_MAX);

        VkResult result = vkAcquireNextImageKHR(_context.device, _swapchain.swapchain, UINT64_MAX,
                                                _swapchain.image_available[frame], VK_NULL_HANDLE,
                                                &_image_index);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) { _needs_recreate = true; return VK_NULL_HANDLE; }
        vkr::vk_check(result, "acquire Hub swapchain image");

        if (_images_in_flight[_image_index] != VK_NULL_HANDLE)
            vkWaitForFences(_context.device, 1, &_images_in_flight[_image_index], VK_TRUE, UINT64_MAX);
        _images_in_flight[_image_index] = _swapchain.in_flight[frame];

        vkResetFences(_context.device, 1, &_swapchain.in_flight[frame]);
        VkCommandBuffer command = _commands[frame];
        vkResetCommandBuffer(command, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkr::vk_check(vkBeginCommandBuffer(command, &begin), "begin Hub command buffer");

        VkClearValue clear{};
        clear.color = {{r, g, b, a}};
        VkRenderPassBeginInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        render.renderPass = _swapchain.render_pass;
        render.framebuffer = _swapchain.framebuffers[_image_index];
        render.renderArea.extent = _swapchain.extent;
        render.clearValueCount = 1;
        render.pClearValues = &clear;
        vkCmdBeginRenderPass(command, &render, VK_SUBPASS_CONTENTS_INLINE);
        return command;
    }

    void end_frame(VkCommandBuffer command) {
        if (command == VK_NULL_HANDLE) return;
        vkCmdEndRenderPass(command);
        vkr::vk_check(vkEndCommandBuffer(command), "end Hub command buffer");

        const uint32_t frame = _frame_index;
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &_swapchain.image_available[frame];
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &_swapchain.render_finished[frame];
        vkr::vk_check(vkQueueSubmit(_context.graphics_queue, 1, &submit, _swapchain.in_flight[frame]),
                      "submit Hub frame");

        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &_swapchain.render_finished[frame];
        present.swapchainCount = 1;
        present.pSwapchains = &_swapchain.swapchain;
        present.pImageIndices = &_image_index;
        const VkResult result = vkQueuePresentKHR(_context.present_queue, &present);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) _needs_recreate = true;
        else vkr::vk_check(result, "present Hub frame");
        _frame_index = (_frame_index + 1) % vkr::kMaxFramesInFlight;
    }

    bool needs_recreate() const { return _needs_recreate; }

    void recreate_if_possible() {
        if (!_needs_recreate) return;
        int width = 0, height = 0;
        SDL_Vulkan_GetDrawableSize(_window, &width, &height);
        if (width == 0 || height == 0) return;
        _swapchain.recreate();
        _images_in_flight.assign(_swapchain.image_count(), VK_NULL_HANDLE);
        _needs_recreate = false;
    }

private:
    SDL_Window* _window = nullptr;
    vkr::Context _context;
    vkr::Swapchain _swapchain;
    std::array<VkCommandBuffer, vkr::kMaxFramesInFlight> _commands{};
    std::vector<VkFence> _images_in_flight;
    uint32_t _frame_index = 0;
    uint32_t _image_index = 0;
    bool _needs_recreate = false;
};

} // namespace gamehub
