#pragma once
/*
 * vk_sprite_batch.hpp — the actual replacement for SDL_RenderCopyEx /
 * SDL_RenderFillRect / SDL_RenderDrawLine.
 *
 * Design goals, matched directly against what render_system.hpp's SDL2
 * code relied on:
 *
 *   - Per-draw transform (position/rotation/scale already baked into
 *     screen-space corner positions by the caller, same division of work
 *     as before: render_system.hpp/.cpp computed SDL_Rect dst +
 *     SDL_Point center + angle, this just takes 4 corner positions instead)
 *   - Per-draw tint color + opacity (SDL_SetTextureColorMod/AlphaMod ->
 *     per-vertex color, multiplied in sprite.frag)
 *   - Per-draw source UV rect (SDL_Rect src -> normalized UVs)
 *   - Per-draw blend mode: BLEND (default + Lit), ADD (Additive/Light2D),
 *     NONE (background clear -- not used per-draw), CUTOUT (alpha_cutoff
 *     discard) — these map to the engine's existing material::Shader enum
 *     1:1; SDL_BLENDMODE_BLEND/ADD become real pipeline objects here since
 *     Vulkan bakes blend state into VkPipeline rather than letting you flip
 *     it per-call like SDL_SetRenderDrawBlendMode did.
 *   - Per-draw clip rect (SDL_RenderSetClipRect -> VkRect2D scissor, set
 *     per draw call same as before)
 *   - Render-to-texture (SDL_SetRenderTarget(tex) -> begin a render pass
 *     against an offscreen AllocatedImage instead of the swapchain)
 *   - Untextured fills/lines (SDL_RenderFillRect/DrawLine -> same quad
 *     path with use_texture=0, see sprite.frag)
 *
 * Batching: one giant dynamic vertex/index buffer per frame, refilled by
 * CPU each frame (matches the old renderer's per-draw-call, CPU-driven
 * nature — RenderSystem::draw() built one immediate SDL_RenderCopyEx call
 * per sprite already, so this isn't a regression, just the same workload
 * expressed as appended vertices instead of immediate driver calls). A
 * "batch" flushes (one vkCmdDrawIndexed) whenever the texture, blend mode,
 * or scissor rect changes — exactly the situations that already forced a
 * new draw call under SDL2's immediate-mode API.
 */

#include "vk_context.hpp"
#include "vk_swapchain.hpp"  // for kMaxFramesInFlight — keeps frame pacing consistent
#include "vk_buffer.hpp"
#include "vk_texture.hpp"
#include "vk_light_ubo.hpp"
#include <vector>
#include <array>
#include <algorithm>
#include <unordered_map>
#include <memory>
#include <cstring>
#include <stdexcept>
#include <string>
#include <fstream>

namespace vkr {

enum class BlendMode { Blend, Additive, None };

struct Vertex {
    float pos[2];
    float uv[2];
    float color[4]; // 0..1, already includes opacity/tint multiply
};

struct PushConstants {
    float viewport_w, viewport_h;
    float alpha_cutoff;     // < 0 disables cutout discard
    int32_t use_texture;
    int32_t use_normal_map; // sprite_lit only; ignored by the unlit pipeline
    float light_strength = 1.f; // material light multiplier (offset 20, matches sprite_lit.frag)
};

// One draw call's worth of quad data, queued by RenderSystem and flushed
// in submission order — order matters because it's the engine's painter's-
// algorithm sort order (sorting_layer/order_in_layer), same as SDL2's
// sequential SDL_RenderCopyEx calls were order-dependent.
struct QuadCommand {
    // 4 corners, clockwise from top-left, in target-space pixels (already
    // includes any rotation the caller applied — see render_system.cpp's
    // Vulkan port for where rotation/pivot math now lives, replacing
    // SDL_RenderCopyEx's angle+center params).
    std::array<float,2> p0, p1, p2, p3;
    std::array<float,2> uv0, uv1; // top-left / bottom-right UV (axis-aligned src rect)
    float color[4] = {1,1,1,1};
    VkImageView texture_view = VK_NULL_HANDLE; // VK_NULL_HANDLE = untextured fill/line
    VkSampler   sampler = VK_NULL_HANDLE;
    BlendMode   blend = BlendMode::Blend;
    float       alpha_cutoff = -1.f;
    bool        has_scissor = false;
    VkRect2D    scissor{};
    int         layer = 0; // submission-order group: 0=parallax, 1=world, 2=ui
    //   texture-sort only reorders within same layer so parallax never
    //   draws over world sprites or ui text regardless of texture pointer.

    // ── Sprite-Lit fields (task 6: real 2D lighting) ─────────────────────────
    // When true, this quad is drawn with the sprite_lit pipeline (per-pixel
    // falloff + optional normal-map lighting against the per-frame Light2D
    // UBO) instead of the plain unlit/additive/cutout pipeline. Ignored for
    // BlendMode::Additive (Light2D glow quads never use the lit shader).
    bool        lit = false;
    VkImageView normal_view = VK_NULL_HANDLE; // VK_NULL_HANDLE = no normal map bound
    VkSampler   normal_sampler = VK_NULL_HANDLE;
    float       light_strength = 1.0f; // material light_strength multiplier (offset 20 of PushConstants)

    // ── GPU instancing (task 10) ──────────────────────────────────────────────
    // When true, this quad is routed through the instanced pipeline
    // (sprite_inst.vert + sprite.frag, VK_VERTEX_INPUT_RATE_INSTANCE) instead
    // of the CPU-expanded vertex path.  Only valid for simple/unlit sprites
    // with no scissor, no cutout, no lit pipeline (those all stay on the CPU
    // path which handles every edge case).  Set from SpriteRenderer.gpu_instancing.
    bool        use_instancing = false;

    // ── Custom shader (task 13) ───────────────────────────────────────────────
    // Absolute paths to pre-compiled SPIR-V files.  When both are non-empty,
    // SpriteBatch::flush() fetches (or creates) a pipeline from PipelineCache
    // keyed by (vert_spv, frag_spv, blend), and uses that instead of the
    // built-in blend pipeline.  Set from MaterialAsset::custom_vert/frag_spv.
    // Ignored when use_instancing or lit is true (those have their own pipelines).
    std::string custom_vert_spv;
    std::string custom_frag_spv;
};

// Per-instance data written into the per-frame instance buffer for the
// instanced pipeline.  Matches sprite_inst.vert's instance-rate attribute
// declarations exactly (locations 3-9).
struct InstanceData {
    float screen_pos[2];   // location 3
    float size[2];         // location 4
    float rotation;        // location 5
    float pivot[2];        // location 6
    float uv_rect[4];      // location 7: u0,v0,u1,v1
    float color[4];        // location 8
    float flip[2];         // location 9: +1 or -1
};

// ─── Descriptor set cache ────────────────────────────────────────────────────
// One combined-image-sampler descriptor set per unique (view, sampler) pair
// seen this run. Textures/samplers are long-lived (TextureCache hangs onto
// them for the whole asset's lifetime, same as the old SDL_Texture* cache
// did), so this cache only grows when genuinely new textures/filter modes
// appear — it never needs per-frame eviction.
class DescriptorCache {
public:
    void init(VkDevice device, VkDescriptorSetLayout layout, uint32_t max_textures) {
        _device = device;
        _layout = layout;

        VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, max_textures};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pci.maxSets = max_textures;
        pci.poolSizeCount = 1;
        pci.pPoolSizes = &pool_size;
        vk_check(vkCreateDescriptorPool(device, &pci, nullptr, &_pool), "vkCreateDescriptorPool (sprite)");
    }

    void destroy() {
        if (_pool) vkDestroyDescriptorPool(_device, _pool, nullptr);
        _pool = VK_NULL_HANDLE;
        _cache.clear();
    }

    VkDescriptorSet get(VkImageView view, VkSampler sampler) {
        Key key{view, sampler};
        auto it = _cache.find(key);
        if (it != _cache.end()) return it->second;

        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = _pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &_layout;
        VkDescriptorSet set;
        VkResult r = vkAllocateDescriptorSets(_device, &ai, &set);
        if (r != VK_SUCCESS) {
            // Pool exhausted — grow by recreating with double capacity.
            // Texture counts are bounded by project assets, not per-frame
            // churn, so this should fire at most a handful of times per run.
            grow();
            vk_check(vkAllocateDescriptorSets(_device, &ai, &set), "vkAllocateDescriptorSets (sprite, retry)");
        }

        VkDescriptorImageInfo img_info{sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &img_info;
        vkUpdateDescriptorSets(_device, 1, &write, 0, nullptr);

        _cache[key] = set;
        return set;
    }

    // Drop a cached descriptor set when its backing texture is destroyed
    // (hot-reload in texture_system.hpp swaps the underlying VkImageView on
    // file change) — otherwise this cache would hand back a descriptor
    // pointing at a freed VkImageView, which is a use-after-free on the GPU.
    void invalidate(VkImageView view) {
        for (auto it = _cache.begin(); it != _cache.end(); ) {
            if (it->first.view == view) it = _cache.erase(it);
            else ++it;
        }
    }

private:
    struct Key {
        VkImageView view; VkSampler sampler;
        bool operator==(const Key& o) const { return view == o.view && sampler == o.sampler; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            return std::hash<void*>()((void*)k.view) ^ (std::hash<void*>()((void*)k.sampler) << 1);
        }
    };

    void grow() {
        // Simplest correct approach: leak the old pool (it still validly
        // backs any descriptor sets already bound in in-flight command
        // buffers) and allocate a larger one for everything from now on.
        // Texture-asset counts are small enough in a 2D engine that this
        // path is cold; optimizing it further isn't worth the complexity.
        VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)_cache.size()*2 + 64};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pci.maxSets = pool_size.descriptorCount;
        pci.poolSizeCount = 1;
        pci.pPoolSizes = &pool_size;
        vk_check(vkCreateDescriptorPool(_device, &pci, nullptr, &_pool), "vkCreateDescriptorPool (sprite, grow)");
    }

    VkDevice _device = VK_NULL_HANDLE;
    VkDescriptorSetLayout _layout = VK_NULL_HANDLE;
    VkDescriptorPool _pool = VK_NULL_HANDLE;
    std::unordered_map<Key, VkDescriptorSet, KeyHash> _cache;
};

// ─── Lit descriptor set cache ────────────────────────────────────────────────
// Same idea as DescriptorCache but for sprite_lit's set=0, which has TWO
// bindings (albedo + normal map) instead of one. Kept separate rather than
// generalizing DescriptorCache, since the unlit path (by far the common
// case — only Sprite-Lit materials need this) shouldn't pay for a second
// binding it never uses.
class LitDescriptorCache {
public:
    void init(VkDevice device, VkDescriptorSetLayout layout, uint32_t max_sets) {
        _device = device;
        _layout = layout;

        std::array<VkDescriptorPoolSize,1> pool_sizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, max_sets * 2}
        };
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pci.maxSets = max_sets;
        pci.poolSizeCount = (uint32_t)pool_sizes.size();
        pci.pPoolSizes = pool_sizes.data();
        vk_check(vkCreateDescriptorPool(device, &pci, nullptr, &_pool), "vkCreateDescriptorPool (sprite-lit)");
    }

    void destroy() {
        if (_pool) vkDestroyDescriptorPool(_device, _pool, nullptr);
        _pool = VK_NULL_HANDLE;
        _cache.clear();
    }

    // normal_view/normal_sampler may be VK_NULL_HANDLE — falls back to the
    // white texture (no normal contribution, see sprite_lit.frag's
    // use_normal_map push-constant flag which still gates this off).
    VkDescriptorSet get(VkImageView albedo_view, VkSampler albedo_sampler,
                        VkImageView normal_view, VkSampler normal_sampler,
                        VkImageView fallback_view, VkSampler fallback_sampler) {
        if (normal_view == VK_NULL_HANDLE)  normal_view = fallback_view;
        if (normal_sampler == VK_NULL_HANDLE) normal_sampler = fallback_sampler;

        Key key{albedo_view, albedo_sampler, normal_view, normal_sampler};
        auto it = _cache.find(key);
        if (it != _cache.end()) return it->second;

        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = _pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &_layout;
        VkDescriptorSet set;
        VkResult r = vkAllocateDescriptorSets(_device, &ai, &set);
        if (r != VK_SUCCESS) {
            grow();
            vk_check(vkAllocateDescriptorSets(_device, &ai, &set), "vkAllocateDescriptorSets (sprite-lit, retry)");
        }

        VkDescriptorImageInfo albedo_info{albedo_sampler, albedo_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo normal_info{normal_sampler, normal_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        std::array<VkWriteDescriptorSet,2> writes{};
        writes[0] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[0].dstSet = set; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &albedo_info;
        writes[1] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[1].dstSet = set; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &normal_info;
        vkUpdateDescriptorSets(_device, (uint32_t)writes.size(), writes.data(), 0, nullptr);

        _cache[key] = set;
        return set;
    }

    // Same texture-hot-reload invalidation as DescriptorCache, but a lit set
    // is keyed on BOTH views, so any cached entry referencing the freed view
    // (as albedo OR as normal map) must be dropped.
    void invalidate(VkImageView view) {
        for (auto it = _cache.begin(); it != _cache.end(); ) {
            if (it->first.albedo_view == view || it->first.normal_view == view) it = _cache.erase(it);
            else ++it;
        }
    }

private:
    struct Key {
        VkImageView albedo_view; VkSampler albedo_sampler;
        VkImageView normal_view; VkSampler normal_sampler;
        bool operator==(const Key& o) const {
            return albedo_view == o.albedo_view && albedo_sampler == o.albedo_sampler &&
                   normal_view == o.normal_view && normal_sampler == o.normal_sampler;
        }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            size_t h = std::hash<void*>()((void*)k.albedo_view);
            h ^= std::hash<void*>()((void*)k.albedo_sampler) << 1;
            h ^= std::hash<void*>()((void*)k.normal_view) << 2;
            h ^= std::hash<void*>()((void*)k.normal_sampler) << 3;
            return h;
        }
    };

    void grow() {
        std::array<VkDescriptorPoolSize,1> pool_sizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)_cache.size()*4 + 128}
        };
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pci.maxSets = (uint32_t)_cache.size()*2 + 64;
        pci.poolSizeCount = (uint32_t)pool_sizes.size();
        pci.pPoolSizes = pool_sizes.data();
        vk_check(vkCreateDescriptorPool(_device, &pci, nullptr, &_pool), "vkCreateDescriptorPool (sprite-lit, grow)");
    }

    VkDevice _device = VK_NULL_HANDLE;
    VkDescriptorSetLayout _layout = VK_NULL_HANDLE;
    VkDescriptorPool _pool = VK_NULL_HANDLE;
    std::unordered_map<Key, VkDescriptorSet, KeyHash> _cache;
};

// ─── PipelineCache (task 13) ──────────────────────────────────────────────────
// Stores and lazily creates VkPipeline objects keyed by (vert_spv_path,
// frag_spv_path, BlendMode).  The three built-in blend variants are
// pre-seeded at startup with empty string keys so existing code paths work
// without changes.  Custom shaders are created on first use and live for the
// lifetime of the SpriteBatch (destroyed in its destructor).
//
// Thread safety: none — only called from the render thread (same as the rest
// of SpriteBatch's API).
class PipelineCache {
public:
    struct Key {
        std::string vert_spv;   // "" = use built-in sprite.vert
        std::string frag_spv;   // "" = use built-in sprite.frag
        BlendMode   blend = BlendMode::Blend;
        bool operator==(const Key& o) const {
            return vert_spv == o.vert_spv && frag_spv == o.frag_spv && blend == o.blend;
        }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            size_t h = std::hash<std::string>{}(k.vert_spv);
            h ^= std::hash<std::string>{}(k.frag_spv) + 0x9e3779b9 + (h<<6) + (h>>2);
            h ^= std::hash<int>{}((int)k.blend)       + 0x9e3779b9 + (h<<6) + (h>>2);
            return h;
        }
    };

    void destroy(VkDevice device) {
        for (auto& [k, p] : _cache)
            if (p) vkDestroyPipeline(device, p, nullptr);
        _cache.clear();
    }

    // Seed a pre-built pipeline (used for the three built-in blend variants).
    void seed(const Key& key, VkPipeline pipeline) {
        _cache[key] = pipeline;
    }

    // Fetch a cached pipeline or create it lazily from SPV files on disk.
    // Returns VK_NULL_HANDLE if the SPV files cannot be read.
    VkPipeline get_or_create(VkDevice device,
                              VkRenderPass render_pass,
                              VkPipelineLayout layout,
                              VkSampleCountFlagBits msaa,
                              const Key& key,
                              const std::string& builtin_vert_spv,
                              const std::string& builtin_frag_spv) {
        auto it = _cache.find(key);
        if (it != _cache.end()) return it->second;

        // Resolve actual SPV paths: empty = use built-in
        std::string vert_path = key.vert_spv.empty() ? builtin_vert_spv : key.vert_spv;
        std::string frag_path = key.frag_spv.empty() ? builtin_frag_spv : key.frag_spv;

        VkShaderModule vert = _load_shader(device, vert_path);
        VkShaderModule frag = _load_shader(device, frag_path);
        if (!vert || !frag) {
            if (vert) vkDestroyShaderModule(device, vert, nullptr);
            if (frag) vkDestroyShaderModule(device, frag, nullptr);
            fprintf(stderr, "[PipelineCache] failed to load shaders: %s / %s\n",
                    vert_path.c_str(), frag_path.c_str());
            _cache[key] = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }

        VkPipeline pipe = _build_pipeline(device, render_pass, layout, msaa, vert, frag, key.blend);
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);

        _cache[key] = pipe;
        return pipe;
    }

private:
    std::unordered_map<Key, VkPipeline, KeyHash> _cache;

    static VkShaderModule _load_shader(VkDevice device, const std::string& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return VK_NULL_HANDLE;
        size_t size = (size_t)file.tellg();
        file.seekg(0);
        std::vector<uint32_t> code((size + 3) / 4);
        file.read((char*)code.data(), (std::streamsize)size);
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = size;
        ci.pCode    = code.data();
        VkShaderModule mod = VK_NULL_HANDLE;
        vkCreateShaderModule(device, &ci, nullptr, &mod);
        return mod;
    }

public:
    // Called by SpriteBatch::create_pipelines to seed built-in blend variants.
    static VkPipeline _build_pipeline(VkDevice device,
                                       VkRenderPass render_pass,
                                       VkPipelineLayout layout,
                                       VkSampleCountFlagBits msaa,
                                       VkShaderModule vert,
                                       VkShaderModule frag,
                                       BlendMode blend) {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vert; stages[0].pName = "main";
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = frag; stages[1].pName = "main";

        VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attrs[3] = {
            {0, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(Vertex, pos)},
            {1, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(Vertex, uv)},
            {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)},
        };
        VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vin.vertexBindingDescriptionCount   = 1; vin.pVertexBindingDescriptions   = &binding;
        vin.vertexAttributeDescriptionCount = 3; vin.pVertexAttributeDescriptions = attrs;

        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1; vp.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode    = VK_CULL_MODE_NONE;
        rs.frontFace   = VK_FRONT_FACE_CLOCKWISE;
        rs.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = msaa;

        std::vector<VkDynamicState> dyn_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = (uint32_t)dyn_states.size();
        dyn.pDynamicStates    = dyn_states.data();

        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|
                             VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        switch (blend) {
            case BlendMode::Blend:
                cba.blendEnable         = VK_TRUE;
                cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                cba.colorBlendOp        = VK_BLEND_OP_ADD;
                cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                cba.alphaBlendOp        = VK_BLEND_OP_ADD;
                break;
            case BlendMode::Additive:
                cba.blendEnable         = VK_TRUE;
                cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                cba.colorBlendOp        = VK_BLEND_OP_ADD;
                cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                cba.alphaBlendOp        = VK_BLEND_OP_ADD;
                break;
            case BlendMode::None:
                cba.blendEnable = VK_FALSE;
                break;
        }
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &cba;

        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.stageCount          = 2;
        ci.pStages             = stages;
        ci.pVertexInputState   = &vin;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState      = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState   = &ms;
        ci.pDynamicState       = &dyn;
        ci.pColorBlendState    = &cb;
        ci.layout              = layout;
        ci.renderPass          = render_pass;
        ci.subpass             = 0;

        VkPipeline pipe = VK_NULL_HANDLE;
        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe);
        return pipe;
    }
};

// ─── SpriteBatch ──────────────────────────────────────────────────────────────
// Owns the pipeline cache (built-in blend variants + lazy custom shader
// pipelines) and the per-frame dynamic vertex/index buffers, and turns a
// queue of QuadCommands into the minimum number of vkCmdDrawIndexed calls.
// ── Frame render stats (for in-editor perf overlay) ──────────────────────────
// Filled in by SpriteBatch::flush() each frame; read back by RenderSystem and
// surfaced in the editor. Not used for any rendering decisions — purely
// diagnostic, so it's safe to add/remove fields without touching draw logic.
struct FrameStats {
    uint32_t draw_calls       = 0; // total vkCmdDrawIndexed calls this frame (regular + instanced batches)
    uint32_t regular_quads    = 0; // quads drawn via the per-vertex CPU-expand path
    uint32_t instanced_quads  = 0; // quads drawn via the GPU-instanced path
    uint32_t instanced_batches = 0; // number of instanced vkCmdDrawIndexed calls (subset of draw_calls)
    uint32_t dropped_quads    = 0; // rejected before queue growth can exceed GPU buffers
};

class SpriteBatch {

public:
    static constexpr uint32_t kMaxQuadsPerFrame = 65536; // generous; grows on demand

    SpriteBatch(Context& ctx, VkRenderPass render_pass, const std::string& shader_dir,
                VkSampleCountFlagBits msaa_samples = VK_SAMPLE_COUNT_1_BIT)
        : _ctx(ctx), _msaa_samples(msaa_samples)
    {
        create_descriptor_layout();
        create_pipeline_layout();
        create_pipelines(render_pass, shader_dir);
        _shader_dir   = shader_dir;
        _render_pass  = render_pass;
        _descriptors.init(_ctx.device, _set_layout, 4096);
        create_dynamic_buffers();
        create_white_texture();
        create_light_texture();

        // ── Sprite-Lit setup (task 6) ────────────────────────────────────────
        _lights.init(_ctx.device, _ctx.allocator);
        create_lit_descriptor_layout();
        create_lit_pipeline_layout();
        create_lit_pipeline(render_pass, shader_dir);
        _lit_descriptors.init(_ctx.device, _lit_set_layout, 1024);

        // ── GPU instancing setup (task 10) ───────────────────────────────────
        create_unit_quad_buffers();
        create_instanced_pipeline(render_pass, shader_dir);
    }

    ~SpriteBatch() {
        for (auto& fb : _frame) {
            fb.vertex_buf.destroy(_ctx.allocator);
            fb.index_buf.destroy(_ctx.allocator);
        }
        _uploader->destroy(_white_tex);
        _uploader->destroy(_light_tex);
        _descriptors.destroy();
        _pipeline_cache.destroy(_ctx.device);
        if (_pipeline_layout) vkDestroyPipelineLayout(_ctx.device, _pipeline_layout, nullptr);
        if (_set_layout) vkDestroyDescriptorSetLayout(_ctx.device, _set_layout, nullptr);

        _lit_descriptors.destroy();
        if (_lit_pipeline) vkDestroyPipeline(_ctx.device, _lit_pipeline, nullptr);
        if (_lit_pipeline_layout) vkDestroyPipelineLayout(_ctx.device, _lit_pipeline_layout, nullptr);
        if (_lit_set_layout) vkDestroyDescriptorSetLayout(_ctx.device, _lit_set_layout, nullptr);
        _lights.destroy();

        // ── GPU instancing cleanup (task 10) ─────────────────────────────────
        for (auto& f : _inst_frame) f.instance_buf.destroy(_ctx.allocator);
        _unit_quad_vb.destroy(_ctx.allocator);
        _unit_quad_ib.destroy(_ctx.allocator);
        if (_inst_pipeline) vkDestroyPipeline(_ctx.device, _inst_pipeline, nullptr);
    }

    SpriteBatch(const SpriteBatch&) = delete;
    SpriteBatch& operator=(const SpriteBatch&) = delete;

    DescriptorCache& descriptors() { return _descriptors; }
    Texture& white_texture() { return _white_tex; }
    Texture& light_texture() { return _light_tex; }

    // ── Frame stats (perf overlay) ───────────────────────────────────────────
    // Valid after flush() has run for the current frame; reset at begin_frame().
    const FrameStats& frame_stats() const { return _stats; }

    // ── Sprite-Lit accessors (task 6) ────────────────────────────────────────
    // RenderSystem calls these once per frame: begin_frame() -> N x push_light()
    // -> set_camera() -> upload(frame_index), all BEFORE draw()/flush() runs.
    LightUBOManager& lights() { return _lights; }
    void invalidate_lit(VkImageView view) { _lit_descriptors.invalidate(view); }

    // Call once per frame before queuing any quads.
    void begin_frame(uint32_t frame_index, VkExtent2D viewport_extent) {
        _current = &_frame[frame_index % kMaxFramesInFlight];
        _current->vertex_cursor = 0;
        _current->index_cursor = 0;
        _viewport_extent = viewport_extent;
        _frame_index = frame_index;
        _batches.clear();
        _pending.clear();
        // ── GPU instancing (task 10) ─────────────────────────────────────────
        _inst_current = &_inst_frame[frame_index % kMaxFramesInFlight];
        _inst_current->instance_cursor = 0;
        _inst_batches.clear();
        _inst_pending.clear();
        _stats = FrameStats{};
    }

    uint32_t current_frame_slot() const { return _frame_index % kMaxFramesInFlight; }

    // Queue a quad for this frame.
    void push_quad(const QuadCommand& q) {
        // GPU-instanced sprites go to their own pending list; everything else
        // uses the existing CPU vertex-expand path.  Instancing is only valid
        // for simple unlit quads — lit, cutout, scissored and untextured quads
        // all stay on the CPU path which handles those edge cases correctly.
        if (q.use_instancing && !q.lit && q.alpha_cutoff < 0.f &&
            !q.has_scissor && q.texture_view != VK_NULL_HANDLE) {
            if (_inst_pending.size() >= kMaxInstancesPerFrame) {
                ++_stats.dropped_quads;
                return;
            }
            _inst_pending.push_back(q);
        } else {
            // The dynamic vertex/index buffers have room for exactly this
            // many CPU-expanded quads. Drop excess transient VFX instead of
            // sorting a runaway vector and failing halfway through a frame.
            if (_pending.size() >= kMaxQuadsPerFrame) {
                ++_stats.dropped_quads;
                return;
            }
            _pending.push_back(q);
        }
    }

    void flush(VkCommandBuffer cmd) {
        if (_pending.empty() && _inst_pending.empty()) return;
        if (!_pending.empty()) {

        // Texture-sort batching: stable-sort within each blend-mode group so
        // same-texture quads are consecutive → fewer vkCmdDrawIndexed calls.
        // stable_sort preserves painter's-algorithm order among same-texture
        // quads so layering is not disturbed.
        //
        // CRITICAL: layer is the primary sort key so parallax (layer=0),
        // world sprites (layer=1) and UI (layer=2) are never interleaved by
        // the texture-sort. Without this, parallax BlendMode::Blend quads
        // would sort together with text quads of the same blend mode and
        // render on top of them, making UI text appear transparent.
        //
        // CRITICAL 2: parallax quads (layer=0) must NOT be texture-sorted.
        // draw_parallax() already submitted them in depth order, and since
        // each parallax layer uses a different texture, a texture-pointer sort
        // would reorder them by VkImageView address (which changes between
        // process launches due to ASLR/heap layout), producing different
        // visual compositing on every restart. Only layers 1 and 2 are
        // texture-sorted — parallax is left in submission order.
        // Preserve painter order: moving full commands here was the exact
        // native crash path under dense boss/particle combat.
        if constexpr (false) std::stable_sort(_pending.begin(), _pending.end(), [](const QuadCommand& a, const QuadCommand& b) {
            // Primary key: layer group (parallax < world < ui)
            if (a.layer != b.layer) return a.layer < b.layer;
            // Parallax (layer 0): never reorder — depth is set by draw_parallax's own sort
            if (a.layer == 0) return false;
            // Secondary key: lit vs unlit (different pipeline + descriptor layout)
            if (a.lit != b.lit) return (int)a.lit < (int)b.lit;
            // Tertiary key: blend mode (keeps opaque/additive/alpha groups intact)
            if (a.blend != b.blend) return (int)a.blend < (int)b.blend;
            // Quaternary key: scissor rect presence (avoids mid-batch state changes)
            if (a.has_scissor != b.has_scissor) return (int)a.has_scissor < (int)b.has_scissor;
            // Quinary key: texture view pointer (sorts same textures together)
            return a.texture_view < b.texture_view;
        });

        for (auto& q : _pending) _push_quad_internal(q);
        _pending.clear();

        // Negative height flips Vulkan clip-space Y so (0,0)=top-left matches SDL/screen convention.
        float vph = (float)_viewport_extent.height;
        VkViewport vp{0, vph, (float)_viewport_extent.width, -vph, 0.f, 1.f};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        VkRect2D full_scissor{{0,0}, _viewport_extent};
        vkCmdSetScissor(cmd, 0, 1, &full_scissor);

        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, &_current->vertex_buf.buffer, offsets);
        vkCmdBindIndexBuffer(cmd, _current->index_buf.buffer, 0, VK_INDEX_TYPE_UINT32);

        BlendMode bound_blend = (BlendMode)-1;
        bool bound_lit = false;
        bool scissor_set = false;
        bool light_camera_sets_bound = false;
        std::string bound_custom_vert, bound_custom_frag;

        for (auto& b : _batches) {
            bool custom = !b.custom_vert_spv.empty() && !b.custom_frag_spv.empty();
            bool pipeline_changed = (b.lit != bound_lit)
                || (!b.lit && !custom && b.blend != bound_blend)
                || (!b.lit && custom && (b.custom_vert_spv != bound_custom_vert
                                       || b.custom_frag_spv != bound_custom_frag
                                       || b.blend != bound_blend));
            if (pipeline_changed) {
                VkPipeline pipe;
                VkPipelineLayout layout;
                if (b.lit) {
                    pipe   = _lit_pipeline;
                    layout = _lit_pipeline_layout;
                } else {
                    PipelineCache::Key key{b.custom_vert_spv, b.custom_frag_spv, b.blend};
                    pipe = _pipeline_cache.get_or_create(
                        _ctx.device, _render_pass, _pipeline_layout, _msaa_samples,
                        key,
                        _shader_dir + "/sprite.vert.spv",
                        _shader_dir + "/sprite.frag.spv");
                    // If custom shader failed to load, fall back to the built-in
                    // blend pipeline (seeded with empty-string keys at startup).
                    if (!pipe)
                        pipe = _pipeline_cache.get_or_create(
                            _ctx.device, _render_pass, _pipeline_layout, _msaa_samples,
                            PipelineCache::Key{"", "", b.blend},
                            _shader_dir + "/sprite.vert.spv",
                            _shader_dir + "/sprite.frag.spv");
                    layout = _pipeline_layout;
                }
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                bound_lit = b.lit;
                bound_blend = b.blend;
                bound_custom_vert = b.custom_vert_spv;
                bound_custom_frag = b.custom_frag_spv;
                (void)layout;
            }

            VkRect2D scissor = b.has_scissor ? b.scissor : full_scissor;
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            scissor_set = true;

            if (b.lit) {
                PushConstants pc{(float)_viewport_extent.width, (float)_viewport_extent.height,
                                  b.alpha_cutoff, b.use_texture, b.use_normal_map, b.light_strength};
                vkCmdPushConstants(cmd, _lit_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                    0, sizeof(PushConstants), &pc);

                VkDescriptorSet albedo_set = _lit_descriptors.get(
                    b.view, b.sampler, b.normal_view, b.normal_sampler,
                    _white_tex.image.view, _white_tex.sampler);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _lit_pipeline_layout,
                                         0, 1, &albedo_set, 0, nullptr);

                if (!light_camera_sets_bound) {
                    VkDescriptorSet light_set  = _lights.light_set(_frame_index);
                    VkDescriptorSet camera_set = _lights.camera_set(_frame_index);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _lit_pipeline_layout,
                                             1, 1, &light_set, 0, nullptr);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _lit_pipeline_layout,
                                             2, 1, &camera_set, 0, nullptr);
                    light_camera_sets_bound = true;
                }
            } else {
                PushConstants pc{(float)_viewport_extent.width, (float)_viewport_extent.height,
                                  b.alpha_cutoff, b.use_texture, 0, 0.f};
                vkCmdPushConstants(cmd, _pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                    0, sizeof(PushConstants), &pc);

                VkDescriptorSet set = _descriptors.get(b.view, b.sampler);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline_layout,
                                         0, 1, &set, 0, nullptr);
                light_camera_sets_bound = false; // next lit batch must rebind after an unlit one
            }

            vkCmdDrawIndexed(cmd, b.index_count, 1, b.index_start, 0, 0);
            _stats.draw_calls++;
            _stats.regular_quads += b.index_count / 6;
        }
        (void)scissor_set;
        } // if (!_pending.empty())

        // ── GPU instanced flush (task 10) ─────────────────────────────────────
        // Sort and flush the instanced pending list separately. Because all
        // instanced quads share the same unit-quad VBO + index buffer, the
        // only batch break needed is a texture change.
        if (!_inst_pending.empty()) {
            // Sort by texture so same-texture sprites batch together
            // Preserve submission order for the same reason as the CPU path.
            if constexpr (false) std::stable_sort(_inst_pending.begin(), _inst_pending.end(),
                [](const QuadCommand& a, const QuadCommand& b) {
                    if (a.layer != b.layer) return a.layer < b.layer;
                    if (a.blend != b.blend) return (int)a.blend < (int)b.blend;
                    return a.texture_view < b.texture_view;
                });

            for (auto& q : _inst_pending) _push_instance_internal(q);
            _inst_pending.clear();

            // Reset viewport/scissor to full (may have been changed by regular pass)
            float vph2 = (float)_viewport_extent.height;
            VkViewport vp2{0, vph2, (float)_viewport_extent.width, -vph2, 0.f, 1.f};
            vkCmdSetViewport(cmd, 0, 1, &vp2);
            VkRect2D full2{{0,0}, _viewport_extent};
            vkCmdSetScissor(cmd, 0, 1, &full2);

            // Bind static unit-quad vertex + index buffers
            VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &_unit_quad_vb.buffer, &zero);
            vkCmdBindIndexBuffer(cmd, _unit_quad_ib.buffer, 0, VK_INDEX_TYPE_UINT32);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _inst_pipeline);

            for (auto& b : _inst_batches) {
                PushConstants pc{(float)_viewport_extent.width, (float)_viewport_extent.height,
                                  -1.f, 1, 0, 0.f};
                vkCmdPushConstants(cmd, _pipeline_layout,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(PushConstants), &pc);

                VkDescriptorSet ds = _descriptors.get(b.view, b.sampler);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    _pipeline_layout, 0, 1, &ds, 0, nullptr);

                // Bind per-frame instance buffer at binding 1, offset to this batch's start
                VkDeviceSize inst_offset = (VkDeviceSize)b.instance_start * sizeof(InstanceData);
                vkCmdBindVertexBuffers(cmd, 1, 1, &_inst_current->instance_buf.buffer, &inst_offset);

                // 6 indices for the unit quad, instanceCount = number of sprites in this batch
                vkCmdDrawIndexed(cmd, 6, b.instance_count, 0, 0, 0);
                _stats.draw_calls++;
                _stats.instanced_batches++;
                _stats.instanced_quads += b.instance_count;
            }
        }
    }

private:
    struct Batch {
        VkImageView view; VkSampler sampler;
        BlendMode blend; float alpha_cutoff; int32_t use_texture;
        bool has_scissor; VkRect2D scissor;
        uint32_t index_start, index_count;
        // Sprite-Lit fields
        bool lit = false;
        VkImageView normal_view = VK_NULL_HANDLE;
        VkSampler   normal_sampler = VK_NULL_HANDLE;
        int32_t     use_normal_map = 0;
        float       light_strength = 1.0f;
        // Custom shader (task 13)
        std::string custom_vert_spv;
        std::string custom_frag_spv;
    };

    // ── GPU instancing structs (task 10) ──────────────────────────────────────
    struct InstBatch {
        VkImageView view;
        VkSampler   sampler;
        BlendMode   blend = BlendMode::Blend;
        uint32_t    instance_start = 0;
        uint32_t    instance_count = 0;
    };

    struct InstFrameBuffers {
        AllocatedBuffer instance_buf;
        uint32_t        instance_cursor = 0;
    };

    struct FrameBuffers {
        AllocatedBuffer vertex_buf, index_buf;
        uint32_t vertex_cursor = 0, index_cursor = 0;
    };

    static bool rects_equal(const VkRect2D& a, const VkRect2D& b) {
        return a.offset.x==b.offset.x && a.offset.y==b.offset.y &&
               a.extent.width==b.extent.width && a.extent.height==b.extent.height;
    }

    // Write one quad's vertices/indices and build/extend a Batch record.
    // Only called from flush() after _pending has been sorted.
    void _push_quad_internal(const QuadCommand& q) {
        if (_current->vertex_cursor + 4 > kMaxQuadsPerFrame * 4)
            throw std::runtime_error("SpriteBatch: per-frame quad limit exceeded — raise kMaxQuadsPerFrame");

        VkImageView view   = (q.texture_view != VK_NULL_HANDLE) ? q.texture_view : _white_tex.image.view;
        VkSampler   samp   = (q.sampler      != VK_NULL_HANDLE) ? q.sampler      : _white_tex.sampler;
        int32_t     use_tex = (q.texture_view != VK_NULL_HANDLE) ? 1 : 0;
        int32_t     use_nmap = (q.lit && q.normal_view != VK_NULL_HANDLE) ? 1 : 0;

        bool extend = !_batches.empty();
        if (extend) {
            Batch& last = _batches.back();
            extend = (last.view == view && last.sampler == samp && last.blend == q.blend &&
                      last.alpha_cutoff == q.alpha_cutoff && last.use_texture == use_tex &&
                      last.has_scissor == q.has_scissor &&
                      (!q.has_scissor || rects_equal(last.scissor, q.scissor)) &&
                      last.lit == q.lit &&
                      last.custom_vert_spv == q.custom_vert_spv &&
                      last.custom_frag_spv == q.custom_frag_spv &&
                      (!q.lit || (last.normal_view == q.normal_view &&
                                  last.normal_sampler == q.normal_sampler &&
                                  last.use_normal_map == use_nmap &&
                                  last.light_strength == q.light_strength)));
        }

        uint32_t base_vertex = _current->vertex_cursor;
        Vertex* vptr = (Vertex*)_current->vertex_buf.mapped_ptr() + base_vertex;
        vptr[0] = { {q.p0[0], q.p0[1]}, {q.uv0[0], q.uv0[1]}, {q.color[0],q.color[1],q.color[2],q.color[3]} };
        vptr[1] = { {q.p1[0], q.p1[1]}, {q.uv1[0], q.uv0[1]}, {q.color[0],q.color[1],q.color[2],q.color[3]} };
        vptr[2] = { {q.p2[0], q.p2[1]}, {q.uv1[0], q.uv1[1]}, {q.color[0],q.color[1],q.color[2],q.color[3]} };
        vptr[3] = { {q.p3[0], q.p3[1]}, {q.uv0[0], q.uv1[1]}, {q.color[0],q.color[1],q.color[2],q.color[3]} };
        _current->vertex_cursor += 4;

        uint32_t base_index = _current->index_cursor;
        uint32_t* iptr = (uint32_t*)_current->index_buf.mapped_ptr() + base_index;
        iptr[0]=base_vertex+0; iptr[1]=base_vertex+1; iptr[2]=base_vertex+2;
        iptr[3]=base_vertex+2; iptr[4]=base_vertex+3; iptr[5]=base_vertex+0;
        _current->index_cursor += 6;

        if (extend) {
            _batches.back().index_count += 6;
        } else {
            Batch b;
            b.view = view; b.sampler = samp; b.blend = q.blend;
            b.alpha_cutoff = q.alpha_cutoff; b.use_texture = use_tex;
            b.has_scissor = q.has_scissor; b.scissor = q.scissor;
            b.index_start = base_index; b.index_count = 6;
            b.lit = q.lit;
            b.normal_view = q.lit ? q.normal_view : VK_NULL_HANDLE;
            b.normal_sampler = q.lit ? q.normal_sampler : VK_NULL_HANDLE;
            b.use_normal_map = use_nmap;
            b.light_strength = q.lit ? q.light_strength : 1.0f;
            b.custom_vert_spv = q.custom_vert_spv;
            b.custom_frag_spv = q.custom_frag_spv;
            _batches.push_back(b);
        }
    }

    void create_descriptor_layout() {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 1;
        ci.pBindings = &binding;
        vk_check(vkCreateDescriptorSetLayout(_ctx.device, &ci, nullptr, &_set_layout),
                  "vkCreateDescriptorSetLayout (sprite)");
    }

    void create_pipeline_layout() {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        range.offset = 0;
        range.size = sizeof(PushConstants);

        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 1;
        ci.pSetLayouts = &_set_layout;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &range;
        vk_check(vkCreatePipelineLayout(_ctx.device, &ci, nullptr, &_pipeline_layout),
                  "vkCreatePipelineLayout (sprite)");
    }

    static std::vector<char> read_file(const std::string& path) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) throw std::runtime_error("Cannot open shader file: " + path);
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::vector<char> buf(sz);
        size_t read = fread(buf.data(), 1, sz, f);
        fclose(f);
        if ((long)read != sz) throw std::runtime_error("Short read on shader file: " + path);
        return buf;
    }

    VkShaderModule load_shader(const std::string& path) {
        auto code = read_file(path);
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = code.size();
        ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule mod;
        vk_check(vkCreateShaderModule(_ctx.device, &ci, nullptr, &mod), ("vkCreateShaderModule: " + path).c_str());
        return mod;
    }

    void create_pipelines(VkRenderPass render_pass, const std::string& shader_dir) {
        // Pre-build the three built-in blend variants and seed them into
        // PipelineCache with empty-string keys so flush() finds them instantly
        // without hitting the filesystem on the first frame.
        VkShaderModule vert = load_shader(shader_dir + "/sprite.vert.spv");
        VkShaderModule frag = load_shader(shader_dir + "/sprite.frag.spv");

        for (BlendMode mode : {BlendMode::Blend, BlendMode::Additive, BlendMode::None}) {
            VkPipeline pipe = PipelineCache::_build_pipeline(
                _ctx.device, render_pass, _pipeline_layout, _msaa_samples, vert, frag, mode);
            _pipeline_cache.seed({std::string(), std::string(), mode}, pipe);
        }

        vkDestroyShaderModule(_ctx.device, vert, nullptr);
        vkDestroyShaderModule(_ctx.device, frag, nullptr);
    }

    // ── Sprite-Lit setup (task 6) ────────────────────────────────────────────
    // set 0: albedo (binding 0) + normal map (binding 1), both fragment-only.
    void create_lit_descriptor_layout() {
        std::array<VkDescriptorSetLayoutBinding,2> bindings{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = (uint32_t)bindings.size();
        ci.pBindings = bindings.data();
        vk_check(vkCreateDescriptorSetLayout(_ctx.device, &ci, nullptr, &_lit_set_layout),
                  "vkCreateDescriptorSetLayout (sprite-lit)");
    }

    // set 0 = albedo+normal (above), set 1 = LightUBO, set 2 = CameraUBO
    // (both owned/created by LightUBOManager — see vk_light_ubo.hpp).
    void create_lit_pipeline_layout() {
        std::array<VkDescriptorSetLayout,3> sets{ _lit_set_layout, _lights.light_set_layout, _lights.camera_set_layout };

        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        range.offset = 0;
        range.size = sizeof(PushConstants);

        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = (uint32_t)sets.size();
        ci.pSetLayouts = sets.data();
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &range;
        vk_check(vkCreatePipelineLayout(_ctx.device, &ci, nullptr, &_lit_pipeline_layout),
                  "vkCreatePipelineLayout (sprite-lit)");
    }

    // One pipeline, always alpha-blended (Sprite-Lit materials are never
    // additive/cutout — those stay on the unlit pipeline; see _draw_sprite's
    // shader-to-pipeline routing in render_system_vk.hpp).
    void create_lit_pipeline(VkRenderPass render_pass, const std::string& shader_dir) {
        VkShaderModule vert = load_shader(shader_dir + "/sprite_lit.vert.spv");
        VkShaderModule frag = load_shader(shader_dir + "/sprite_lit.frag.spv");

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName = "main";
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attrs[3] = {
            {0, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(Vertex, pos)},
            {1, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(Vertex, uv)},
            {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)},
        };
        VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vin.vertexBindingDescriptionCount = 1;
        vin.pVertexBindingDescriptions = &binding;
        vin.vertexAttributeDescriptionCount = 3;
        vin.pVertexAttributeDescriptions = attrs;

        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = _msaa_samples;

        std::vector<VkDynamicState> dyn_states = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = (uint32_t)dyn_states.size();
        dyn.pDynamicStates = dyn_states.data();

        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|
                              VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;

        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vin;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pDynamicState = &dyn;
        ci.pColorBlendState = &cb;
        ci.layout = _lit_pipeline_layout;
        ci.renderPass = render_pass;
        ci.subpass = 0;

        vk_check(vkCreateGraphicsPipelines(_ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &_lit_pipeline),
                  "vkCreateGraphicsPipelines (sprite-lit)");

        vkDestroyShaderModule(_ctx.device, vert, nullptr);
        vkDestroyShaderModule(_ctx.device, frag, nullptr);
    }

    void create_dynamic_buffers() {
        VkDeviceSize vbuf_size = (VkDeviceSize)kMaxQuadsPerFrame * 4 * sizeof(Vertex);
        VkDeviceSize ibuf_size = (VkDeviceSize)kMaxQuadsPerFrame * 6 * sizeof(uint32_t);
        for (auto& fb : _frame) {
            fb.vertex_buf.create(_ctx.allocator, vbuf_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                   VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
            fb.index_buf.create(_ctx.allocator, ibuf_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                  VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
        }
    }

    // A 1x1 opaque white pixel, bound whenever a quad is untextured (fills,
    // lines, debug draws). Lets sprite.frag's use_texture branch sample
    // *something* valid even when QuadCommand carries no real texture —
    // simpler than a separate no-texture pipeline variant, and matches
    // the old SDL_RenderFillRect path needing no texture binding at all.
    void create_white_texture() {
        _uploader = std::make_unique<TextureUploader>(_ctx);
        uint8_t white[4] = {255,255,255,255};
        _white_tex = _uploader->upload(white, 1, 1, FilterMode::Point);
    }

    // Pre-baked radial light sprite used by the Light2D editor/runtime gizmo.
    // A single additive quad over a smooth texture is much cheaper than the
    // old stack of overlapping shells, and the bilinear+mip chain removes the
    // visible banding between successive rings.
    void create_light_texture() {
        constexpr uint32_t kSize = 256;
        std::vector<uint8_t> pixels((size_t)kSize * kSize * 4);
        const float center = (float)(kSize - 1) * 0.5f;
        const float inv_radius = 1.0f / center;
        for (uint32_t y = 0; y < kSize; ++y) {
            for (uint32_t x = 0; x < kSize; ++x) {
                float dx = (float)x - center;
                float dy = (float)y - center;
                float dist = std::sqrt(dx * dx + dy * dy) * inv_radius; // 0..~1.4
                float t = std::clamp(dist, 0.0f, 1.0f);
                // Smooth, slightly gamma-shaped falloff: bright core, soft edge.
                float falloff = 1.0f - t;
                falloff = falloff * falloff * falloff * (falloff * (falloff * 6.0f - 15.0f) + 10.0f);
                falloff = std::clamp(falloff, 0.0f, 1.0f);
                float alpha = std::pow(falloff, 1.35f);
                size_t idx = ((size_t)y * kSize + x) * 4;
                pixels[idx + 0] = 255;
                pixels[idx + 1] = 255;
                pixels[idx + 2] = 255;
                pixels[idx + 3] = (uint8_t)std::lround(alpha * 255.0f);
            }
        }
        _light_tex = _uploader->upload(pixels.data(), kSize, kSize, FilterMode::Bilinear, true);
    }

    Context& _ctx;
    VkSampleCountFlagBits _msaa_samples = VK_SAMPLE_COUNT_1_BIT;
    VkDescriptorSetLayout _set_layout = VK_NULL_HANDLE;
    VkPipelineLayout _pipeline_layout = VK_NULL_HANDLE;

    // ── Pipeline cache (task 13) ──────────────────────────────────────────────
    // Replaces the old fixed std::array<VkPipeline,3>.  Built-in blend
    // variants are seeded at startup; custom shaders are created lazily on
    // first use and live for the SpriteBatch's lifetime.
    PipelineCache _pipeline_cache;
    std::string   _shader_dir;    // stored for lazy custom pipeline creation
    VkRenderPass  _render_pass = VK_NULL_HANDLE;

    DescriptorCache _descriptors;

    std::array<FrameBuffers, kMaxFramesInFlight> _frame;
    FrameBuffers* _current = nullptr;
    std::vector<Batch> _batches;
    std::vector<QuadCommand> _pending; // accumulated per-frame, sorted before flush
    VkExtent2D _viewport_extent{};
    uint32_t _frame_index = 0;

    std::unique_ptr<TextureUploader> _uploader;
    Texture _white_tex;
    Texture _light_tex;

    // ── Sprite-Lit state (task 6) ─────────────────────────────────────────────
    VkDescriptorSetLayout _lit_set_layout = VK_NULL_HANDLE;
    VkPipelineLayout _lit_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline _lit_pipeline = VK_NULL_HANDLE;
    LitDescriptorCache _lit_descriptors;
    LightUBOManager _lights;

    // ── GPU instancing state (task 10) ───────────────────────────────────────
    // One static unit-quad VBO + IBO, shared every frame (never changes).
    // Per-frame host-visible instance buffer (one InstanceData per sprite).
    // One instanced pipeline variant reusing _pipeline_layout + _set_layout
    // (same descriptor layout as the unlit path — just one texture binding).
    AllocatedBuffer _unit_quad_vb;
    AllocatedBuffer _unit_quad_ib;
    VkPipeline      _inst_pipeline = VK_NULL_HANDLE;

    static constexpr uint32_t kMaxInstancesPerFrame = 65536;
    std::array<InstFrameBuffers, kMaxFramesInFlight> _inst_frame;
    InstFrameBuffers* _inst_current = nullptr;
    std::vector<InstBatch>     _inst_batches;
    std::vector<QuadCommand>   _inst_pending;
    FrameStats _stats; // reset in begin_frame(), filled in flush()

    // Write one quad into the per-frame instance buffer and open/extend an InstBatch.
    void _push_instance_internal(const QuadCommand& q) {
        if (_inst_current->instance_cursor >= kMaxInstancesPerFrame)
            throw std::runtime_error("SpriteBatch: per-frame instanced sprite limit exceeded");

        VkImageView view = (q.texture_view != VK_NULL_HANDLE) ? q.texture_view : _white_tex.image.view;
        VkSampler   samp = (q.sampler      != VK_NULL_HANDLE) ? q.sampler      : _white_tex.sampler;

        uint32_t slot = _inst_current->instance_cursor++;
        InstanceData* dst = (InstanceData*)_inst_current->instance_buf.mapped_ptr() + slot;

        // Reconstruct per-instance fields from QuadCommand corners.
        // p0=TL, p1=TR, p2=BR, p3=BL  (clockwise from top-left, pre-rotation)
        // screen_pos = centre of the quad
        dst->screen_pos[0] = (q.p0[0] + q.p2[0]) * 0.5f;
        dst->screen_pos[1] = (q.p0[1] + q.p2[1]) * 0.5f;
        // size from top-left to bottom-right span (axis-aligned before rotation)
        dst->size[0] = std::abs(q.p2[0] - q.p0[0]);
        dst->size[1] = std::abs(q.p2[1] - q.p0[1]);
        // rotation: angle from top-left → top-right edge
        float dx = q.p1[0] - q.p0[0], dy = q.p1[1] - q.p0[1];
        dst->rotation = std::atan2(dy, dx);
        // pivot: always 0.5,0.5 for the instanced path (centred quad, pivot
        // is baked into screen_pos by the caller via world_to_screen)
        dst->pivot[0] = 0.5f;
        dst->pivot[1] = 0.5f;
        dst->uv_rect[0] = q.uv0[0]; dst->uv_rect[1] = q.uv0[1];
        dst->uv_rect[2] = q.uv1[0]; dst->uv_rect[3] = q.uv1[1];
        for (int i=0;i<4;i++) dst->color[i] = q.color[i];
        // flip detection: if uv u0>u1 the caller flipped horizontally, etc.
        dst->flip[0] = (q.uv0[0] <= q.uv1[0]) ? 1.f : -1.f;
        dst->flip[1] = (q.uv0[1] <= q.uv1[1]) ? 1.f : -1.f;

        // Extend or open a batch
        bool extend = !_inst_batches.empty() &&
            _inst_batches.back().view  == view &&
            _inst_batches.back().sampler == samp &&
            _inst_batches.back().blend == q.blend;
        if (extend) {
            _inst_batches.back().instance_count++;
        } else {
            InstBatch b;
            b.view = view; b.sampler = samp; b.blend = q.blend;
            b.instance_start = slot;
            b.instance_count = 1;
            _inst_batches.push_back(b);
        }
    }

    // Static 4-vertex unit quad in local sprite space [-0.5, 0.5].
    // Vertices: TL, TR, BR, BL (same winding as the CPU path).
    // Shared across all instanced draws — written once at startup.
    void create_unit_quad_buffers() {
        struct UnitVertex { float pos[2]; };
        static const UnitVertex verts[4] = {
            {{-0.5f, -0.5f}}, // TL
            {{ 0.5f, -0.5f}}, // TR
            {{ 0.5f,  0.5f}}, // BR
            {{-0.5f,  0.5f}}, // BL
        };
        static const uint32_t indices[6] = {0,1,2, 2,3,0};

        // Upload via a staging buffer → device-local buffer
        VkDeviceSize vsize = sizeof(verts);
        VkDeviceSize isize = sizeof(indices);

        // Use host-visible for simplicity (same pattern as the dynamic vertex buf)
        _unit_quad_vb.create(_ctx.allocator, vsize,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
        memcpy(_unit_quad_vb.mapped_ptr(), verts, vsize);

        _unit_quad_ib.create(_ctx.allocator, isize,
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
        memcpy(_unit_quad_ib.mapped_ptr(), indices, isize);

        // Per-frame instance buffers
        VkDeviceSize inst_size = (VkDeviceSize)kMaxInstancesPerFrame * sizeof(InstanceData);
        for (auto& f : _inst_frame) {
            f.instance_buf.create(_ctx.allocator, inst_size,
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
        }
    }

    // Instanced pipeline: sprite_inst.vert (two bindings: unit quad vertex +
    // per-instance data) + sprite.frag (unmodified — same push constants,
    // same single-texture descriptor set as the regular unlit pipeline).
    // Reuses _pipeline_layout and _set_layout so no new descriptor infra needed.
    void create_instanced_pipeline(VkRenderPass render_pass, const std::string& shader_dir) {
        // sprite_inst.vert.spv must be compiled from sprite_inst.vert alongside
        // the other shaders (add to the project's CMakeLists shader compile step).
        VkShaderModule vert = load_shader(shader_dir + "/sprite_inst.vert.spv");
        VkShaderModule frag = load_shader(shader_dir + "/sprite.frag.spv"); // reuse unlit frag

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert; stages[0].pName = "main";
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag; stages[1].pName = "main";

        // Binding 0: unit-quad vertex data (rate=vertex)
        VkVertexInputBindingDescription bindings[2]{};
        bindings[0].binding   = 0;
        bindings[0].stride    = sizeof(float) * 2; // vec2 local_pos only
        bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        // Binding 1: per-instance data (rate=instance)
        bindings[1].binding   = 1;
        bindings[1].stride    = sizeof(InstanceData);
        bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

        // Attribute layout must match sprite_inst.vert locations exactly.
        VkVertexInputAttributeDescription attrs[8]{};
        // Binding 0 — unit quad
        attrs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};                            // in_local_pos
        // Binding 1 — per instance (InstanceData fields in order)
        attrs[1] = {3, 1, VK_FORMAT_R32G32_SFLOAT,       offsetof(InstanceData, screen_pos)};
        attrs[2] = {4, 1, VK_FORMAT_R32G32_SFLOAT,       offsetof(InstanceData, size)};
        attrs[3] = {5, 1, VK_FORMAT_R32_SFLOAT,          offsetof(InstanceData, rotation)};
        attrs[4] = {6, 1, VK_FORMAT_R32G32_SFLOAT,       offsetof(InstanceData, pivot)};
        attrs[5] = {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(InstanceData, uv_rect)};
        attrs[6] = {8, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(InstanceData, color)};
        attrs[7] = {9, 1, VK_FORMAT_R32G32_SFLOAT,       offsetof(InstanceData, flip)};

        VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vin.vertexBindingDescriptionCount   = 2;
        vin.pVertexBindingDescriptions      = bindings;
        vin.vertexAttributeDescriptionCount = 8;
        vin.pVertexAttributeDescriptions    = attrs;

        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1; vp.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode    = VK_CULL_MODE_NONE;
        rs.frontFace   = VK_FRONT_FACE_CLOCKWISE;
        rs.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = _msaa_samples;

        std::vector<VkDynamicState> dyn_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = (uint32_t)dyn_states.size();
        dyn.pDynamicStates    = dyn_states.data();

        // Alpha blend (same as BlendMode::Blend — instanced path is always blended)
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|
                                  VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        cba.blendEnable         = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp        = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp        = VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &cba;

        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.stageCount          = 2;
        ci.pStages             = stages;
        ci.pVertexInputState   = &vin;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState      = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState   = &ms;
        ci.pDynamicState       = &dyn;
        ci.pColorBlendState    = &cb;
        ci.layout              = _pipeline_layout; // reuse unlit layout
        ci.renderPass          = render_pass;
        ci.subpass             = 0;

        vk_check(vkCreateGraphicsPipelines(_ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &_inst_pipeline),
                  "vkCreateGraphicsPipelines (sprite-instanced)");

        vkDestroyShaderModule(_ctx.device, vert, nullptr);
        vkDestroyShaderModule(_ctx.device, frag, nullptr);
    }

}; // class SpriteBatch

} // namespace vkr
