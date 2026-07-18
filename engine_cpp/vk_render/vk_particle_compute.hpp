#pragma once
/*
 * vk_particle_compute.hpp — GPU-side particle simulation via Vulkan compute.
 *
 * Replaces the CPU loop in systems.hpp's ParticleSystem::update() for
 * emitters that have many particles (> ~512). The CPU path is kept as a
 * fallback and is used for any emitter whose particle count is below
 * kGpuThreshold, or when the engine is built without Vulkan compute support.
 *
 * Data layout
 * -----------
 * Each particle is a GpuParticle (32 bytes, std430 aligned).  Two SSBO
 * buffers (ping-pong) hold the current and next frame's particles.  A
 * uniform buffer holds per-emitter simulation constants (dt, gravity, etc.)
 * that change every frame.
 *
 * The compute shader (particle.comp) dispatches one workgroup per particle,
 * advancing age/position, writing survivors to the next buffer, and
 * atomically incrementing a counter so we know how many particles survived.
 *
 * Integration with systems.hpp
 * ----------------------------
 * ParticleSystem::update() checks for a GpuParticleEmitter pointer on the
 * entity. If present, it hands off to GpuParticleCompute::tick() instead of
 * the CPU loop. Rendering reads back the particle count/positions through
 * GpuParticleCompute::readback_particles(), which maps the result buffer.
 *
 * Compute shader source is in vk_render/shaders/particle.comp.
 * Compile with:
 *   glslc shaders/particle.comp -o shaders/particle.comp.spv
 */

#include "vk_context.hpp"
#include "vk_buffer.hpp"
#include <vector>
#include <string>
#include <stdexcept>
#include <cstring>
#include <cmath>

namespace vkr {

// ─── GPU particle struct (must match particle.comp layout) ──────────────────
struct GpuParticle {
    float x, y;        // position (world pixels)
    float vx, vy;      // velocity (pixels/sec)
    float age;         // seconds alive
    float lifetime;    // seconds to live
    int   frame;       // task 12: atlas frame index, assigned at spawn time
    float _pad0;        // std430 padding to 32 bytes
};

// ─── Per-emitter constants uploaded each tick ────────────────────────────────
struct ParticleUBO {
    float dt;
    float gravity_x;
    float gravity_y;
    uint32_t max_particles;
};

// ─── GpuParticleCompute ──────────────────────────────────────────────────────
// One instance per emitter (managed by ParticleSystem when it detects a
// "gpu_particles" flag on a ParticleEmitter component).
class GpuParticleCompute {
public:
    static constexpr uint32_t kGpuThreshold = 512; // below this, use CPU path
    static constexpr uint32_t kMaxParticles = 65536;

    GpuParticleCompute(Context& ctx, const std::string& shader_dir)
        : _ctx(ctx)
    {
        _create_descriptor_layout();
        _create_pipeline_layout();
        _create_pipeline(shader_dir);
        _create_buffers();
        _create_descriptor_sets();
    }

    ~GpuParticleCompute() {
        vkDeviceWaitIdle(_ctx.device);
        _buf_a.destroy(_ctx.allocator);
        _buf_b.destroy(_ctx.allocator);
        _counter_buf.destroy(_ctx.allocator);
        _ubo.destroy(_ctx.allocator);
        if (_pool)     vkDestroyDescriptorPool(_ctx.device, _pool, nullptr);
        if (_pipeline) vkDestroyPipeline(_ctx.device, _pipeline, nullptr);
        if (_layout)   vkDestroyPipelineLayout(_ctx.device, _layout, nullptr);
        if (_set_layout) vkDestroyDescriptorSetLayout(_ctx.device, _set_layout, nullptr);
    }

    GpuParticleCompute(const GpuParticleCompute&) = delete;
    GpuParticleCompute& operator=(const GpuParticleCompute&) = delete;

    // Spawn new particles (called by emitter logic on CPU — only emission is
    // CPU-side; simulation is GPU-side). appends to the ping buffer's mapped
    // region; caller must call tick() after to advance them.
    void spawn(const std::vector<GpuParticle>& new_particles) {
        if (_live_count + new_particles.size() > kMaxParticles) return;
        GpuParticle* ptr = (GpuParticle*)_buf_a.mapped_ptr() + _live_count;
        std::memcpy(ptr, new_particles.data(), new_particles.size() * sizeof(GpuParticle));
        _live_count += (uint32_t)new_particles.size();
    }

    // Submit a compute dispatch that simulates one frame. Call this before
    // recording any render commands — a pipeline barrier in the render pass
    // ensures the result is visible to the vertex/fragment stage if particles
    // are rendered directly from GPU buffers.
    void tick(VkCommandBuffer cmd, float dt,
              float gravity_x = 0.f, float gravity_y = 980.f) {
        // Reset counter to 0 (alive particle count after this tick).
        uint32_t zero = 0;
        std::memcpy(_counter_buf.mapped_ptr(), &zero, sizeof(uint32_t));

        // Upload UBO.
        ParticleUBO ubo{ dt, gravity_x, gravity_y, _live_count };
        std::memcpy(_ubo.mapped_ptr(), &ubo, sizeof(ubo));

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                 _layout, 0, 1, &_desc_set, 0, nullptr);

        uint32_t groups = (_live_count + 63) / 64;
        if (groups > 0) vkCmdDispatch(cmd, groups, 1, 1);

        // Barrier: compute write → compute/host read (for spawn next frame /
        // CPU readback).
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
            0, 1, &barrier, 0, nullptr, 0, nullptr);

        // Swap ping-pong: buf_b now has survivors, becomes new buf_a for next tick.
        std::swap(_buf_a, _buf_b);
        // Read back live count after GPU done (needs queue sync — use fence in
        // production; for simplicity we read after device idle at end of frame).
        _pending_readback = true;
    }

    // Call after queue idle (or behind a fence) to update _live_count from GPU.
    void resolve_count() {
        if (!_pending_readback) return;
        std::memcpy(&_live_count, _counter_buf.mapped_ptr(), sizeof(uint32_t));
        _pending_readback = false;
        // Re-bind descriptor sets to point at the swapped buffers.
        _update_descriptor_sets();
    }

    // CPU readback of particle positions for rendering (returns a view into
    // mapped host-visible buffer — valid until next tick()).
    const GpuParticle* particles() const {
        return (const GpuParticle*)_buf_a.mapped_ptr();
    }
    uint32_t live_count() const { return _live_count; }

    // Direct GPU buffer access for rendering without readback (bind as SSBO
    // in the sprite/particle render pipeline).
    VkBuffer particle_buffer() const { return _buf_a.buffer; }

private:
    void _create_descriptor_layout() {
        // Bindings: 0=src SSBO (ping), 1=dst SSBO (pong), 2=counter, 3=UBO
        VkDescriptorSetLayoutBinding bindings[4]{};
        for (int i = 0; i < 3; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        bindings[3].binding = 3;
        bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[3].descriptorCount = 1;
        bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 4;
        ci.pBindings = bindings;
        vk_check(vkCreateDescriptorSetLayout(_ctx.device, &ci, nullptr, &_set_layout),
                  "vkCreateDescriptorSetLayout (particle compute)");
    }

    void _create_pipeline_layout() {
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 1;
        ci.pSetLayouts = &_set_layout;
        vk_check(vkCreatePipelineLayout(_ctx.device, &ci, nullptr, &_layout),
                  "vkCreatePipelineLayout (particle compute)");
    }

    void _create_pipeline(const std::string& shader_dir) {
        std::string path = shader_dir + "/particle.comp.spv";
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) throw std::runtime_error("Cannot open particle compute shader: " + path);
        fseek(f, 0, SEEK_END);
        size_t sz = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        std::vector<char> code(sz);
        fread(code.data(), 1, sz, f);
        fclose(f);

        VkShaderModuleCreateInfo sm_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sm_ci.codeSize = sz;
        sm_ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule mod;
        vk_check(vkCreateShaderModule(_ctx.device, &sm_ci, nullptr, &mod),
                  "vkCreateShaderModule (particle.comp)");

        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = mod;
        stage.pName  = "main";

        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage  = stage;
        ci.layout = _layout;
        vk_check(vkCreateComputePipelines(_ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &_pipeline),
                  "vkCreateComputePipelines (particle)");

        vkDestroyShaderModule(_ctx.device, mod, nullptr);
    }

    void _create_buffers() {
        VkDeviceSize particle_sz = (VkDeviceSize)kMaxParticles * sizeof(GpuParticle);
        uint32_t flags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

        // Both ping and pong need STORAGE; host-visible for spawn() + readback.
        _buf_a.create(_ctx.allocator, particle_sz, flags, VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
        _buf_b.create(_ctx.allocator, particle_sz, flags, VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);

        _counter_buf.create(_ctx.allocator, sizeof(uint32_t),
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
        _ubo.create(_ctx.allocator, sizeof(ParticleUBO),
                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
    }

    void _create_descriptor_sets() {
        VkDescriptorPoolSize sizes[2] = {
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
        };
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = 1;
        pci.poolSizeCount = 2;
        pci.pPoolSizes = sizes;
        vk_check(vkCreateDescriptorPool(_ctx.device, &pci, nullptr, &_pool),
                  "vkCreateDescriptorPool (particle compute)");

        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = _pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &_set_layout;
        vk_check(vkAllocateDescriptorSets(_ctx.device, &ai, &_desc_set),
                  "vkAllocateDescriptorSets (particle compute)");

        _update_descriptor_sets();
    }

    void _update_descriptor_sets() {
        VkDescriptorBufferInfo src_info {_buf_a.buffer, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo dst_info {_buf_b.buffer, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo cnt_info {_counter_buf.buffer, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo ubo_info {_ubo.buffer,       0, VK_WHOLE_SIZE};

        VkWriteDescriptorSet writes[4]{};
        auto fill = [&](int i, VkDescriptorType type, VkDescriptorBufferInfo* info) {
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = _desc_set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = type;
            writes[i].pBufferInfo = info;
        };
        fill(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &src_info);
        fill(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &dst_info);
        fill(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &cnt_info);
        fill(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  &ubo_info);
        vkUpdateDescriptorSets(_ctx.device, 4, writes, 0, nullptr);
    }

    Context& _ctx;
    VkDescriptorSetLayout _set_layout = VK_NULL_HANDLE;
    VkPipelineLayout      _layout     = VK_NULL_HANDLE;
    VkPipeline            _pipeline   = VK_NULL_HANDLE;
    VkDescriptorPool      _pool       = VK_NULL_HANDLE;
    VkDescriptorSet       _desc_set   = VK_NULL_HANDLE;

    AllocatedBuffer _buf_a;      // ping (src this tick, dst for spawn)
    AllocatedBuffer _buf_b;      // pong (dst this tick)
    AllocatedBuffer _counter_buf; // atomic survive count
    AllocatedBuffer _ubo;

    uint32_t _live_count = 0;
    bool _pending_readback = false;
};

} // namespace vkr
