#pragma once
/*
 * vk_swapchain.hpp — swapchain + per-frame sync primitives.
 *
 * Owns the chain of presentable images, their views, and the render pass
 * used to draw directly into them (the "present" target — equivalent to
 * what SDL_RenderPresent used to flip for you automatically). Also owns
 * the in-flight sync objects (semaphores/fences) needed to safely pipeline
 * multiple frames without stomping on a swapchain image the GPU/compositor
 * hasn't finished with yet.
 *
 * 2D sprite rendering needs no depth buffer — draw order (the engine's
 * existing layer/order-in-layer sort) is what SDL2's painter's-algorithm
 * blits relied on too, so this matches the old behavior exactly: one
 * color attachment, no depth attachment.
 *
 * Recreation: call recreate() on SDL_WINDOWEVENT_RESIZED (and whenever
 * acquire/present report VK_ERROR_OUT_OF_DATE_KHR / VK_SUBOPTIMAL_KHR).
 */

#include "vk_context.hpp"
#include <vector>
#include <algorithm>
#include <limits>

namespace vkr {

static constexpr int kMaxFramesInFlight = 2;

struct SwapchainSupport {
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> present_modes;
};

inline SwapchainSupport query_swapchain_support(VkPhysicalDevice dev, VkSurfaceKHR surface) {
    SwapchainSupport s;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev, surface, &s.capabilities);

    uint32_t fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surface, &fmt_count, nullptr);
    s.formats.resize(fmt_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surface, &fmt_count, s.formats.data());

    uint32_t mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(dev, surface, &mode_count, nullptr);
    s.present_modes.resize(mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(dev, surface, &mode_count, s.present_modes.data());

    return s;
}

class Swapchain {
public:
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat       image_format{};
    VkExtent2D     extent{};
    std::vector<VkImage>     images;
    std::vector<VkImageView> image_views;
    VkRenderPass   render_pass = VK_NULL_HANDLE; // draws straight into swapchain images
    std::vector<VkFramebuffer> framebuffers;

    // Per-frame-in-flight sync (indexed by current_frame, NOT by swapchain image)
    std::vector<VkSemaphore> image_available;
    std::vector<VkSemaphore> render_finished;
    std::vector<VkFence>     in_flight;

    Swapchain(Context& ctx, SDL_Window* window, bool vsync)
        : _ctx(ctx), _window(window), _vsync(vsync)
    {
        create_swapchain();
        create_image_views();
        create_render_pass();
        create_framebuffers();
        create_sync_objects();
    }

    ~Swapchain() { cleanup(); }

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    // Call after SDL_WINDOWEVENT_RESIZED or when acquire/present return
    // VK_ERROR_OUT_OF_DATE_KHR / VK_SUBOPTIMAL_KHR.
    void recreate() {
        // Window may be minimized (0x0) — Vulkan forbids a zero-extent
        // swapchain, so the caller (Renderer::begin_frame) should poll
        // and skip rendering entirely until it's non-zero again.
        vkDeviceWaitIdle(_ctx.device);

        cleanup_swapchain_only();
        create_swapchain();
        create_image_views();
        create_render_pass();
        create_framebuffers();
    }

    uint32_t image_count() const { return (uint32_t)images.size(); }

private:
    Context&    _ctx;
    SDL_Window* _window;
    bool        _vsync;

    void cleanup_swapchain_only() {
        for (auto fb : framebuffers) vkDestroyFramebuffer(_ctx.device, fb, nullptr);
        framebuffers.clear();
        if (render_pass) vkDestroyRenderPass(_ctx.device, render_pass, nullptr);
        render_pass = VK_NULL_HANDLE;
        for (auto v : image_views) vkDestroyImageView(_ctx.device, v, nullptr);
        image_views.clear();
        if (swapchain) vkDestroySwapchainKHR(_ctx.device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }

    void cleanup() {
        cleanup_swapchain_only();
        for (auto s : image_available) vkDestroySemaphore(_ctx.device, s, nullptr);
        for (auto s : render_finished)  vkDestroySemaphore(_ctx.device, s, nullptr);
        for (auto f : in_flight)        vkDestroyFence(_ctx.device, f, nullptr);
    }

    static VkSurfaceFormatKHR choose_format(const std::vector<VkSurfaceFormatKHR>& formats) {
        for (auto& f : formats)
            if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                return f;
        return formats[0];
    }

    VkPresentModeKHR choose_present_mode(const std::vector<VkPresentModeKHR>& modes) const {
        if (!_vsync) {
            // Uncapped, matching the old engine's SDL_RenderSetVSync(0)
            // default — mailbox avoids tearing if available, falls back to
            // the always-guaranteed FIFO (which is effectively vsync-on)
            // since IMMEDIATE isn't supported on every platform/driver.
            for (auto m : modes) if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
            for (auto m : modes) if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) return m;
        }
        return VK_PRESENT_MODE_FIFO_KHR; // always available per spec
    }

    VkExtent2D choose_extent(const VkSurfaceCapabilitiesKHR& caps) const {
        if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max())
            return caps.currentExtent;
        int w, h;
        SDL_Vulkan_GetDrawableSize(_window, &w, &h);
        VkExtent2D ext{ (uint32_t)std::max(w, 1), (uint32_t)std::max(h, 1) };
        ext.width  = std::clamp(ext.width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
        ext.height = std::clamp(ext.height, caps.minImageExtent.height, caps.maxImageExtent.height);
        return ext;
    }

    void create_swapchain() {
        SwapchainSupport support = query_swapchain_support(_ctx.physical_device, _ctx.surface);
        VkSurfaceFormatKHR format = choose_format(support.formats);
        VkPresentModeKHR mode = choose_present_mode(support.present_modes);
        VkExtent2D ext = choose_extent(support.capabilities);

        uint32_t img_count = support.capabilities.minImageCount + 1;
        if (support.capabilities.maxImageCount > 0)
            img_count = std::min(img_count, support.capabilities.maxImageCount);

        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface = _ctx.surface;
        ci.minImageCount = img_count;
        ci.imageFormat = format.format;
        ci.imageColorSpace = format.colorSpace;
        ci.imageExtent = ext;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        uint32_t qfi[] = { *_ctx.queue_families.graphics, *_ctx.queue_families.present };
        if (_ctx.queue_families.graphics != _ctx.queue_families.present) {
            ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            ci.queueFamilyIndexCount = 2;
            ci.pQueueFamilyIndices = qfi;
        } else {
            ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }

        ci.preTransform = support.capabilities.currentTransform;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode = mode;
        ci.clipped = VK_TRUE;
        ci.oldSwapchain = VK_NULL_HANDLE;

        vk_check(vkCreateSwapchainKHR(_ctx.device, &ci, nullptr, &swapchain), "vkCreateSwapchainKHR");

        image_format = format.format;
        extent = ext;

        uint32_t actual_count = 0;
        vkGetSwapchainImagesKHR(_ctx.device, swapchain, &actual_count, nullptr);
        images.resize(actual_count);
        vkGetSwapchainImagesKHR(_ctx.device, swapchain, &actual_count, images.data());
    }

    void create_image_views() {
        image_views.resize(images.size());
        for (size_t i = 0; i < images.size(); ++i) {
            VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            ci.image = images[i];
            ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            ci.format = image_format;
            ci.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                               VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
            ci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vk_check(vkCreateImageView(_ctx.device, &ci, nullptr, &image_views[i]), "vkCreateImageView (swapchain)");
        }
    }

    void create_render_pass() {
        VkAttachmentDescription color{};
        color.format = image_format;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;   // mirrors RenderSystem::clear()
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_ref;

        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = 0;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ci.attachmentCount = 1;
        ci.pAttachments = &color;
        ci.subpassCount = 1;
        ci.pSubpasses = &subpass;
        ci.dependencyCount = 1;
        ci.pDependencies = &dep;

        vk_check(vkCreateRenderPass(_ctx.device, &ci, nullptr, &render_pass), "vkCreateRenderPass (swapchain)");
    }

    void create_framebuffers() {
        framebuffers.resize(image_views.size());
        for (size_t i = 0; i < image_views.size(); ++i) {
            VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            ci.renderPass = render_pass;
            ci.attachmentCount = 1;
            ci.pAttachments = &image_views[i];
            ci.width = extent.width;
            ci.height = extent.height;
            ci.layers = 1;
            vk_check(vkCreateFramebuffer(_ctx.device, &ci, nullptr, &framebuffers[i]), "vkCreateFramebuffer (swapchain)");
        }
    }

    void create_sync_objects() {
        image_available.resize(kMaxFramesInFlight);
        render_finished.resize(kMaxFramesInFlight);
        in_flight.resize(kMaxFramesInFlight);

        VkSemaphoreCreateInfo sem_ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fence_ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT; // first wait must not block forever

        for (int i = 0; i < kMaxFramesInFlight; ++i) {
            vk_check(vkCreateSemaphore(_ctx.device, &sem_ci, nullptr, &image_available[i]), "vkCreateSemaphore");
            vk_check(vkCreateSemaphore(_ctx.device, &sem_ci, nullptr, &render_finished[i]), "vkCreateSemaphore");
            vk_check(vkCreateFence(_ctx.device, &fence_ci, nullptr, &in_flight[i]), "vkCreateFence");
        }
    }
};

} // namespace vkr
