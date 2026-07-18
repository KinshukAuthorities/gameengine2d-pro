#pragma once
/*
 * vk_context.hpp — Vulkan instance/device/queue/allocator bootstrap.
 *
 * This is the part of a Vulkan backend that has no real per-game-engine
 * personality: pick a GPU, open a logical device, get queues, set up an
 * allocator. Every engine writes roughly this same file once. It is
 * intentionally kept separate from anything sprite/2D-specific (that lives
 * in vk_renderer_backend.hpp) so this class could be reused unmodified by
 * a totally different rendering scheme later.
 *
 * Scope (mirrors what RenderSystem's old SDL2 ctor used to do implicitly):
 *   - VkInstance, with validation layers in debug builds
 *   - VkSurfaceKHR (created from the SDL window via SDL_Vulkan_CreateSurface)
 *   - VkPhysicalDevice selection (prefers discrete GPU, requires graphics+
 *     present queue support and swapchain extension)
 *   - VkDevice + graphics/present queues (commonly the same queue for 2D)
 *   - VmaAllocator (Vulkan Memory Allocator — avoids hand-rolled
 *     vkAllocateMemory bookkeeping, which is where most Vulkan memory bugs
 *     come from)
 *   - One-shot command buffer helper, used for texture uploads etc.
 *
 * NOT in scope here: swapchain (vk_swapchain.hpp), pipelines/sprite
 * batching (vk_renderer_backend.hpp).
 */

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

#define VMA_VULKAN_VERSION 1001000  // Vulkan 1.1 baseline — broad device support
#include "vk_mem_alloc.h"

#include <vulkan/vulkan.h>

#include <vector>
#include <string>
#include <stdexcept>
#include <iostream>
#include <cstring>
#include <set>
#include <optional>
#include <algorithm>

namespace vkr {

#ifdef NDEBUG
static constexpr bool kEnableValidation = false;
#else
static constexpr bool kEnableValidation = true;
#endif

inline void vk_check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        throw std::runtime_error(std::string("Vulkan error in ") + what + ": " + std::to_string((int)r));
    }
}

// ─── Queue family indices ────────────────────────────────────────────────────
struct QueueFamilyIndices {
    std::optional<uint32_t> graphics;
    std::optional<uint32_t> present;
    bool is_complete() const { return graphics.has_value() && present.has_value(); }
};

// ─── Debug messenger (validation layers) ─────────────────────────────────────
inline VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*user*/)
{
    // Only surface warnings/errors — verbose/info spam isn't actionable here.
    if (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) {
        std::cerr << "[Vulkan] " << data->pMessage << "\n";
    }
    return VK_FALSE;
}

inline VkResult create_debug_messenger(VkInstance instance,
                                        const VkDebugUtilsMessengerCreateInfoEXT* ci,
                                        VkDebugUtilsMessengerEXT* out) {
    auto fn = (PFN_vkCreateDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return fn(instance, ci, nullptr, out);
}

inline void destroy_debug_messenger(VkInstance instance, VkDebugUtilsMessengerEXT m) {
    if (!m) return;
    auto fn = (PFN_vkDestroyDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (fn) fn(instance, m, nullptr);
}

// ─── Context ──────────────────────────────────────────────────────────────────
// Owns everything that exists for the lifetime of the app, independent of
// window size or swapchain recreation.
class Context {
public:
    VkInstance       instance        = VK_NULL_HANDLE;
    VkSurfaceKHR     surface         = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice         device          = VK_NULL_HANDLE;
    VkQueue          graphics_queue  = VK_NULL_HANDLE;
    VkQueue          present_queue   = VK_NULL_HANDLE;
    QueueFamilyIndices queue_families;
    VmaAllocator     allocator       = VK_NULL_HANDLE;
    VkCommandPool    command_pool    = VK_NULL_HANDLE; // for transient upload buffers

    VkPhysicalDeviceProperties device_props{};

    Context(SDL_Window* window, const char* app_name) {
        create_instance(window, app_name);
        if (kEnableValidation) setup_debug_messenger();
        create_surface(window);
        pick_physical_device();
        create_logical_device();
        create_allocator();
        create_command_pool();

        std::cout << "[Vulkan] Device: " << device_props.deviceName
                  << " | API: "
                  << VK_API_VERSION_MAJOR(device_props.apiVersion) << "."
                  << VK_API_VERSION_MINOR(device_props.apiVersion) << "."
                  << VK_API_VERSION_PATCH(device_props.apiVersion)
                  << " | Validation: " << (_validation_active ? "ON" : "OFF") << "\n";
    }

    ~Context() {
        if (device) vkDeviceWaitIdle(device);
        if (command_pool) vkDestroyCommandPool(device, command_pool, nullptr);
        if (allocator) vmaDestroyAllocator(allocator);
        if (device) vkDestroyDevice(device, nullptr);
        if (kEnableValidation) destroy_debug_messenger(instance, debug_messenger);
        if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
    }

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    // ── One-shot command buffer helper ──────────────────────────────────────
    // Used for buffer→image copies (texture uploads) and image layout
    // transitions — the Vulkan equivalent of "just call SDL_UpdateTexture".
    VkCommandBuffer begin_one_shot() const {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandPool = command_pool;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd;
        vk_check(vkAllocateCommandBuffers(device, &ai, &cmd), "allocate one-shot cmd");

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        return cmd;
    }

    void end_one_shot(VkCommandBuffer cmd) const {
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(graphics_queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(graphics_queue); // simple & correct; uploads aren't hot-path
        vkFreeCommandBuffers(device, command_pool, 1, &cmd);
    }

    uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props) const {
        VkPhysicalDeviceMemoryProperties mem_props;
        vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);
        for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
            if ((type_filter & (1u << i)) &&
                (mem_props.memoryTypes[i].propertyFlags & props) == props)
                return i;
        }
        throw std::runtime_error("find_memory_type: no suitable memory type");
    }

    static QueueFamilyIndices query_queue_families(VkPhysicalDevice dev, VkSurfaceKHR surf) {
        QueueFamilyIndices indices;
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, families.data());

        for (uint32_t i = 0; i < count; ++i) {
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) indices.graphics = i;
            VkBool32 present_support = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surf, &present_support);
            if (present_support) indices.present = i;
            if (indices.is_complete()) break;
        }
        return indices;
    }

private:
    VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
    const std::vector<const char*> validation_layers = { "VK_LAYER_KHRONOS_validation" };
    const std::vector<const char*> device_extensions  = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    bool check_validation_support() {
        uint32_t count;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> available(count);
        vkEnumerateInstanceLayerProperties(&count, available.data());
        for (auto layer : validation_layers) {
            bool found = false;
            for (auto& a : available) if (strcmp(layer, a.layerName) == 0) { found = true; break; }
            if (!found) return false;
        }
        return true;
    }

    void create_instance(SDL_Window* window, const char* app_name) {
        VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app_info.pApplicationName = app_name;
        app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        app_info.pEngineName = "GameEngine2DPro";
        app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        app_info.apiVersion = VK_API_VERSION_1_1;

        // SDL tells us exactly which instance extensions the platform's
        // Vulkan/WSI layer needs (VK_KHR_surface + the platform-specific
        // surface extension) — same role SDL played implicitly before by
        // picking the right SDL_Renderer driver.
        unsigned int ext_count = 0;
        SDL_Vulkan_GetInstanceExtensions(window, &ext_count, nullptr);
        std::vector<const char*> extensions(ext_count);
        SDL_Vulkan_GetInstanceExtensions(window, &ext_count, extensions.data());

        bool validation_available = kEnableValidation && check_validation_support();
        if (validation_available) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        } else if (kEnableValidation) {
            std::cerr << "[Vulkan] Validation layers requested but not available "
                          "(install the Vulkan SDK for debug builds) — continuing without them.\n";
        }

        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app_info;
        ci.enabledExtensionCount = (uint32_t)extensions.size();
        ci.ppEnabledExtensionNames = extensions.data();

        VkDebugUtilsMessengerCreateInfoEXT dbg_ci{};
        if (validation_available) {
            ci.enabledLayerCount = (uint32_t)validation_layers.size();
            ci.ppEnabledLayerNames = validation_layers.data();
            fill_debug_ci(dbg_ci);
            ci.pNext = &dbg_ci; // validates instance create/destroy too
        } else {
            ci.enabledLayerCount = 0;
        }

        vk_check(vkCreateInstance(&ci, nullptr, &instance), "vkCreateInstance");
        _validation_active = validation_available;
    }

    static void fill_debug_ci(VkDebugUtilsMessengerCreateInfoEXT& ci) {
        ci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        ci.pfnUserCallback = debug_callback;
    }

    void setup_debug_messenger() {
        if (!_validation_active) return;
        VkDebugUtilsMessengerCreateInfoEXT ci{};
        fill_debug_ci(ci);
        if (create_debug_messenger(instance, &ci, &debug_messenger) != VK_SUCCESS)
            std::cerr << "[Vulkan] Failed to set up debug messenger\n";
    }

    void create_surface(SDL_Window* window) {
        if (!SDL_Vulkan_CreateSurface(window, instance, &surface))
            throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError());
    }

    bool device_supports_extensions(VkPhysicalDevice dev) {
        uint32_t count;
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, available.data());
        std::set<std::string> required(device_extensions.begin(), device_extensions.end());
        for (auto& ext : available) required.erase(ext.extensionName);
        return required.empty();
    }

    int score_device(VkPhysicalDevice dev) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);

        if (!device_supports_extensions(dev)) return -1;
        QueueFamilyIndices q = query_queue_families(dev, surface);
        if (!q.is_complete()) return -1;

        uint32_t fmt_count = 0, mode_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surface, &fmt_count, nullptr);
        vkGetPhysicalDeviceSurfacePresentModesKHR(dev, surface, &mode_count, nullptr);
        if (fmt_count == 0 || mode_count == 0) return -1;

        int score = 0;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 1000;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 500;
        score += (int)(props.limits.maxImageDimension2D / 1000);
        return score;
    }

    void pick_physical_device() {
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        if (count == 0) throw std::runtime_error("No Vulkan-capable GPU found on this system.");
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance, &count, devices.data());

        VkPhysicalDevice best = VK_NULL_HANDLE;
        int best_score = -1;
        for (auto dev : devices) {
            int s = score_device(dev);
            if (s > best_score) { best_score = s; best = dev; }
        }
        if (best == VK_NULL_HANDLE || best_score < 0)
            throw std::runtime_error("No suitable Vulkan GPU (needs graphics+present queue and swapchain support).");

        physical_device = best;
        queue_families = query_queue_families(physical_device, surface);
        vkGetPhysicalDeviceProperties(physical_device, &device_props);
    }

    void create_logical_device() {
        std::set<uint32_t> unique_families = { *queue_families.graphics, *queue_families.present };
        std::vector<VkDeviceQueueCreateInfo> queue_cis;
        float priority = 1.0f;
        for (uint32_t family : unique_families) {
            VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            qci.queueFamilyIndex = family;
            qci.queueCount = 1;
            qci.pQueuePriorities = &priority;
            queue_cis.push_back(qci);
        }

        VkPhysicalDeviceFeatures features{}; // 2D sprite rendering needs nothing exotic

        VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        ci.queueCreateInfoCount = (uint32_t)queue_cis.size();
        ci.pQueueCreateInfos = queue_cis.data();
        ci.pEnabledFeatures = &features;
        ci.enabledExtensionCount = (uint32_t)device_extensions.size();
        ci.ppEnabledExtensionNames = device_extensions.data();
        if (_validation_active) {
            // Per-device validation layers are deprecated but harmless to set
            // on older loaders; modern loaders ignore this field.
            ci.enabledLayerCount = (uint32_t)validation_layers.size();
            ci.ppEnabledLayerNames = validation_layers.data();
        }

        vk_check(vkCreateDevice(physical_device, &ci, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, *queue_families.graphics, 0, &graphics_queue);
        vkGetDeviceQueue(device, *queue_families.present, 0, &present_queue);
    }

    void create_allocator() {
        VmaAllocatorCreateInfo ci{};
        ci.physicalDevice = physical_device;
        ci.device = device;
        ci.instance = instance;
        ci.vulkanApiVersion = VK_API_VERSION_1_1;
        vk_check(vmaCreateAllocator(&ci, &allocator), "vmaCreateAllocator");
    }

    void create_command_pool() {
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ci.queueFamilyIndex = *queue_families.graphics;
        vk_check(vkCreateCommandPool(device, &ci, nullptr, &command_pool), "vkCreateCommandPool (context)");
    }

    bool _validation_active = false;
};

} // namespace vkr
