#pragma once
/*
 * vk_light_ubo.hpp — per-frame GPU Light2D UBO for the Sprite-Lit pipeline.
 *
 * Replaces the old "additively-blended fill circle" Light2D approach with a
 * proper UBO that sprite_lit.frag reads once per frame.  Layout:
 *
 *   set 1, binding 0 — LightUBO    (up to kMaxLights GpuLight entries)
 *   set 2, binding 0 — CameraUBO   (camera position + pixels-per-unit for
 *                                   world-space reconstruction in sprite_lit.vert)
 *
 * Usage (called from RenderSystem every frame):
 *
 *   LightUBOManager mgr(ctx, device);          // init once
 *
 *   // Before draw():
 *   mgr.begin_frame(frame_index);
 *   mgr.push_light({ world_x, world_y }, radius, intensity, {r,g,b,1});
 *   mgr.set_camera(cam_x, cam_y, pixels_per_unit, vp_w, vp_h);
 *   mgr.upload(frame_index);
 *
 *   // During flush():
 *   vkCmdBindDescriptorSets(cmd, ..., set=1, mgr.light_set(frame_index));
 *   vkCmdBindDescriptorSets(cmd, ..., set=2, mgr.camera_set(frame_index));
 */

#include "vk_context.hpp"
#include "vk_buffer.hpp"
#include "vk_swapchain.hpp" // kMaxFramesInFlight
#include <algorithm>
#include <array>
#include <cstring>

namespace vkr {

// ─── GPU-side struct mirrors sprite_lit.frag's GpuLight ─────────────────────
// std140 packing: vec2 aligns to 8, float to 4, vec4 to 16. Each GpuLight
// is 2+2 pad+1+1 pad (8 bytes) + 16 bytes = 32 bytes per light.
struct alignas(16) GpuLight {
    float position[2];
    float radius;
    float intensity;
    float color[4]; // RGBA, A unused
};
static_assert(sizeof(GpuLight) == 32, "GpuLight must be 32 bytes (std140)");

static constexpr int kMaxLights = 16;

struct LightUBOData {
    GpuLight lights[kMaxLights];
    int32_t  light_count;
    int32_t  _pad[3];
    // RGB scene ambient plus intensity in A. This lives after the 16-byte
    // count block, preserving std140 alignment for every light entry.
    float    ambient[4];
};
static_assert(sizeof(LightUBOData) == kMaxLights * 32 + 32,
              "LightUBOData std140 layout mismatch");

// ─── GPU-side camera data (sprite_lit.vert set=2) ───────────────────────────
struct alignas(16) CameraUBOData {
    float camera_world_pos[2];
    float pixels_per_unit;
    float viewport_w;
    float viewport_h;
    float _pad[3];
};
static_assert(sizeof(CameraUBOData) == 32, "CameraUBOData must be 32 bytes");

// ─── LightUBOManager ─────────────────────────────────────────────────────────
class LightUBOManager {
public:
    // Descriptor set layouts — expose so SpriteBatch can include them in its
    // pipeline layout (set 1 and set 2 must be declared in the layout used
    // by sprite_lit pipelines).
    VkDescriptorSetLayout light_set_layout  = VK_NULL_HANDLE;
    VkDescriptorSetLayout camera_set_layout = VK_NULL_HANDLE;

    LightUBOManager() = default;

    void init(VkDevice device, VmaAllocator allocator) {
        _device    = device;
        _allocator = allocator;

        _create_layouts();
        _create_pool();

        for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
            // Light UBO
            _light_buf[i].create(allocator, sizeof(LightUBOData),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VMA_MEMORY_USAGE_AUTO_PREFER_HOST, /*mapped=*/true);

            // Camera UBO
            _camera_buf[i].create(allocator, sizeof(CameraUBOData),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VMA_MEMORY_USAGE_AUTO_PREFER_HOST, /*mapped=*/true);

            _allocate_sets(i);
            _write_descriptors(i);
        }
    }

    void destroy() {
        for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
            _light_buf[i].destroy(_allocator);
            _camera_buf[i].destroy(_allocator);
        }
        if (_pool) { vkDestroyDescriptorPool(_device, _pool, nullptr); _pool = VK_NULL_HANDLE; }
        if (light_set_layout)  { vkDestroyDescriptorSetLayout(_device, light_set_layout,  nullptr); }
        if (camera_set_layout) { vkDestroyDescriptorSetLayout(_device, camera_set_layout, nullptr); }
    }

    // ── Per-frame API ─────────────────────────────────────────────────────────

    void begin_frame() {
        _pending = LightUBOData{};
        set_ambient(1.f, 1.f, 1.f, .15f);
    }

    void push_light(float world_x, float world_y,
                    float radius, float intensity,
                    float r, float g, float b) {
        if (_pending.light_count >= kMaxLights) return;
        int idx = _pending.light_count++;
        GpuLight& L = _pending.lights[idx];
        L.position[0] = world_x;
        L.position[1] = world_y;
        L.radius      = radius;
        L.intensity    = intensity;
        L.color[0]    = r;
        L.color[1]    = g;
        L.color[2]    = b;
        L.color[3]    = 1.f;
    }

    void set_ambient(float r, float g, float b, float intensity) {
        _pending.ambient[0] = std::clamp(r, 0.f, 1.f);
        _pending.ambient[1] = std::clamp(g, 0.f, 1.f);
        _pending.ambient[2] = std::clamp(b, 0.f, 1.f);
        _pending.ambient[3] = std::clamp(intensity, 0.f, 4.f);
    }

    void set_camera(float cam_x, float cam_y,
                    float pixels_per_unit,
                    float viewport_w, float viewport_h) {
        _pending_cam.camera_world_pos[0] = cam_x;
        _pending_cam.camera_world_pos[1] = cam_y;
        _pending_cam.pixels_per_unit     = pixels_per_unit;
        _pending_cam.viewport_w          = viewport_w;
        _pending_cam.viewport_h          = viewport_h;
    }

    // Upload both UBOs to the current frame's buffers.  Call after all
    // push_light() calls and set_camera(), before recording draw commands.
    //
    // The buffers are host-mapped, but the memory type selected by VMA is not
    // guaranteed to be host-coherent on every GPU. Flush the written ranges so
    // the GPU sees the new light data immediately on all devices.
    void upload(uint32_t frame_index) {
        uint32_t fi = frame_index % kMaxFramesInFlight;

        void* light_dst = _light_buf[fi].mapped_ptr();
        void* cam_dst   = _camera_buf[fi].mapped_ptr();
        if (!light_dst || !cam_dst) {
            return;
        }

        std::memcpy(light_dst, &_pending, sizeof(LightUBOData));
        std::memcpy(cam_dst,   &_pending_cam, sizeof(CameraUBOData));

        vmaFlushAllocation(_allocator, _light_buf[fi].allocation, 0, sizeof(LightUBOData));
        vmaFlushAllocation(_allocator, _camera_buf[fi].allocation, 0, sizeof(CameraUBOData));
    }

    // Descriptor sets to bind during flush (set 1 and set 2)
    VkDescriptorSet light_set (uint32_t frame_index) const { return _light_sets [frame_index % kMaxFramesInFlight]; }
    VkDescriptorSet camera_set(uint32_t frame_index) const { return _camera_sets[frame_index % kMaxFramesInFlight]; }

    int light_count() const { return _pending.light_count; }

private:
    VkDevice      _device    = VK_NULL_HANDLE;
    VmaAllocator  _allocator = VK_NULL_HANDLE;
    VkDescriptorPool _pool   = VK_NULL_HANDLE;

    std::array<AllocatedBuffer,    kMaxFramesInFlight> _light_buf{};
    std::array<AllocatedBuffer,    kMaxFramesInFlight> _camera_buf{};
    std::array<VkDescriptorSet,    kMaxFramesInFlight> _light_sets{};
    std::array<VkDescriptorSet,    kMaxFramesInFlight> _camera_sets{};

    LightUBOData  _pending{};
    CameraUBOData _pending_cam{};

    void _create_layouts() {
        auto make_layout = [this](VkDescriptorSetLayout& out_layout) {
            VkDescriptorSetLayoutBinding b{};
            b.binding         = 0;
            b.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b.descriptorCount = 1;
            b.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            ci.bindingCount = 1;
            ci.pBindings    = &b;
            vk_check(vkCreateDescriptorSetLayout(_device, &ci, nullptr, &out_layout),
                     "vkCreateDescriptorSetLayout (LightUBO)");
        };
        make_layout(light_set_layout);
        make_layout(camera_set_layout);
    }

    void _create_pool() {
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                  kMaxFramesInFlight * 2u}; // 2 UBOs per frame
        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets       = kMaxFramesInFlight * 2;
        ci.poolSizeCount = 1;
        ci.pPoolSizes    = &size;
        vk_check(vkCreateDescriptorPool(_device, &ci, nullptr, &_pool),
                 "vkCreateDescriptorPool (LightUBO)");
    }

    void _allocate_sets(uint32_t fi) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool     = _pool;
        ai.descriptorSetCount = 1;

        ai.pSetLayouts = &light_set_layout;
        vk_check(vkAllocateDescriptorSets(_device, &ai, &_light_sets[fi]),
                 "vkAllocateDescriptorSets (light)");

        ai.pSetLayouts = &camera_set_layout;
        vk_check(vkAllocateDescriptorSets(_device, &ai, &_camera_sets[fi]),
                 "vkAllocateDescriptorSets (camera)");
    }

    void _write_descriptors(uint32_t fi) {
        // Light UBO
        VkDescriptorBufferInfo light_info{};
        light_info.buffer = _light_buf[fi].buffer;
        light_info.offset = 0;
        light_info.range  = sizeof(LightUBOData);

        VkWriteDescriptorSet w_light{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w_light.dstSet          = _light_sets[fi];
        w_light.dstBinding      = 0;
        w_light.descriptorCount = 1;
        w_light.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w_light.pBufferInfo     = &light_info;

        // Camera UBO
        VkDescriptorBufferInfo cam_info{};
        cam_info.buffer = _camera_buf[fi].buffer;
        cam_info.offset = 0;
        cam_info.range  = sizeof(CameraUBOData);

        VkWriteDescriptorSet w_cam{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w_cam.dstSet          = _camera_sets[fi];
        w_cam.dstBinding      = 0;
        w_cam.descriptorCount = 1;
        w_cam.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w_cam.pBufferInfo     = &cam_info;

        VkWriteDescriptorSet writes[] = { w_light, w_cam };
        vkUpdateDescriptorSets(_device, 2, writes, 0, nullptr);
    }
};

} // namespace vkr
