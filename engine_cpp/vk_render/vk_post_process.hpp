#pragma once
/*
 * vk_post_process.hpp — Two-pass post-processing for Nova Engine.
 *
 * Pass 1 (scene pass): render all sprites into an offscreen RGBA8 image
 *   (same size as the swapchain) using a standard render pass.
 *
 * Pass 2 (composite pass): fullscreen triangle that samples the offscreen
 *   image and applies:
 *     - Bloom  (configurable threshold + intensity; done here as a simple
 *       brightpass + 2-tap Gaussian blur baked into the composite shader for
 *       zero extra passes; a real separable blur can be added as Pass 1.5).
 *     - Color grading (exposure, saturation, contrast via fragment shader).
 *     - Vignette (radial darkening).
 *
 * Usage in vk_renderer_backend.hpp
 * ----------------------------------
 *   // Construction (once):
 *   PostProcessPipeline pp(ctx, swapchain_extent, swapchain_render_pass, shader_dir);
 *
 *   // Per-frame record:
 *   // 1. Begin scene render pass:
 *   pp.begin_scene_pass(cmd);
 *   sprite_batch.flush(cmd);               // draw all sprites
 *   vkCmdEndRenderPass(cmd);
 *
 *   // 2. Begin swapchain render pass as normal, then:
 *   pp.begin_composite_pass(cmd, swapchain_framebuffer, swapchain_extent);
 *   pp.flush_composite(cmd);               // fullscreen triangle
 *   vkCmdEndRenderPass(cmd);
 *
 *   // Resize when swapchain changes:
 *   pp.resize(new_extent);
 *
 * Shader files needed (compile with glslc):
 *   shaders/fullscreen.vert.spv   — passthrough triangle, no VBOs
 *   shaders/composite.frag.spv    — bloom + grade + vignette
 */

#include "vk_context.hpp"
#include "vk_buffer.hpp"
#include <array>
#include <string>
#include <vector>
#include <stdexcept>

namespace vkr {

// Push constants for the composite shader (16 bytes — fits in 128-byte limit)
struct PostProcessPushConstants {
    float bloom_threshold = 0.8f;  // luminance above which bloom fires
    float bloom_intensity = 0.3f;  // additive bloom strength
    float exposure        = 1.0f;  // linear exposure multiplier
    float vignette        = 0.4f;  // vignette strength [0=off, 1=heavy]
    float saturation      = 1.0f;  // 0=greyscale, 1=original, >1=hypersaturated
    float contrast        = 1.0f;  // pivot-0.5 contrast
    float _pad0 = 0.f, _pad1 = 0.f;
};

class PostProcessPipeline {
public:
    PostProcessPipeline(Context& ctx, VkExtent2D extent,
                        VkFormat swapchain_format,
                        const std::string& shader_dir)
        : _ctx(ctx), _extent(extent), _swapchain_format(swapchain_format)
    {
        _create_scene_resources();
        _create_descriptor_layout();
        _create_composite_pipeline(shader_dir);
        _create_descriptor_set();
    }

    ~PostProcessPipeline() {
        _destroy_scene_resources();
        if (_desc_pool)      vkDestroyDescriptorPool(_ctx.device, _desc_pool, nullptr);
        if (_comp_pipeline)  vkDestroyPipeline(_ctx.device, _comp_pipeline, nullptr);
        if (_comp_layout)    vkDestroyPipelineLayout(_ctx.device, _comp_layout, nullptr);
        if (_desc_set_layout) vkDestroyDescriptorSetLayout(_ctx.device, _desc_set_layout, nullptr);
        if (_sampler)        vkDestroySampler(_ctx.device, _sampler, nullptr);
    }

    PostProcessPipeline(const PostProcessPipeline&) = delete;
    PostProcessPipeline& operator=(const PostProcessPipeline&) = delete;

    // Returns the scene-pass render pass — SpriteBatch should be constructed
    // with this render pass so its pipelines are compatible.
    VkRenderPass scene_render_pass() const { return _scene_render_pass; }

    // Returns the offscreen framebuffer the scene should render into.
    VkFramebuffer scene_framebuffer() const { return _scene_framebuffer; }

    // Returns the offscreen image view (for debug display in editor).
    VkImageView scene_image_view() const { return _scene_image.view; }

    // Begin the offscreen scene render pass. Caller should then record all
    // sprite draw calls, then vkCmdEndRenderPass.
    void begin_scene_pass(VkCommandBuffer cmd) const {
        VkClearValue clear{};
        clear.color = {{0.f, 0.f, 0.f, 0.f}};

        VkRenderPassBeginInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpi.renderPass  = _scene_render_pass;
        rpi.framebuffer = _scene_framebuffer;
        rpi.renderArea.extent = _extent;
        rpi.clearValueCount = 1;
        rpi.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);
    }

    // Record the fullscreen composite quad into the currently-active render
    // pass (caller's swapchain render pass). Call flush_composite() to draw.
    void flush_composite(VkCommandBuffer cmd, const PostProcessPushConstants& params = {}) const {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _comp_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 _comp_layout, 0, 1, &_desc_set, 0, nullptr);
        vkCmdPushConstants(cmd, _comp_layout,
                            VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, sizeof(PostProcessPushConstants), &params);
        // Fullscreen triangle: 3 vertices, no VBO — vert shader generates
        // positions from gl_VertexIndex (0,1,2 cover the viewport).
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    // Recreate size-dependent resources after swapchain resize.
    void resize(VkExtent2D new_extent) {
        vkDeviceWaitIdle(_ctx.device);
        _destroy_scene_resources();
        _extent.width  = std::max(1u, new_extent.width);
        _extent.height = std::max(1u, new_extent.height);
        _create_scene_resources();
        _update_descriptor_set();
    }

    PostProcessPushConstants params; // tweak at runtime (editor-exposed)

private:
    // ── Offscreen image ──────────────────────────────────────────────────────
    struct OffscreenImage {
        VkImage     image   = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView view    = VK_NULL_HANDLE;
    };

    void _create_scene_resources() {
        // Clamp to 1×1 if the window is minimised / zero-sized (prevents
        // VK_ERROR_OUT_OF_HOST_MEMORY from vmaCreateImage with extent 0).
        uint32_t w = std::max(1u, _extent.width);
        uint32_t h = std::max(1u, _extent.height);

        // Create RGBA8 offscreen image
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType   = VK_IMAGE_TYPE_2D;
        ici.format      = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent      = {w, h, 1};
        ici.mipLevels   = 1;
        ici.arrayLayers = 1;
        ici.samples     = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling      = VK_IMAGE_TILING_OPTIMAL;
        ici.usage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        vk_check(vmaCreateImage(_ctx.allocator, &ici, &aci,
                                 &_scene_image.image, &_scene_image.alloc, nullptr),
                  "vmaCreateImage (post-process offscreen)");

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image    = _scene_image.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format   = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk_check(vkCreateImageView(_ctx.device, &vci, nullptr, &_scene_image.view),
                  "vkCreateImageView (post-process offscreen)");

        // Transition to COLOR_ATTACHMENT_OPTIMAL
        VkCommandBuffer cmd = _ctx.begin_one_shot();
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.image = _scene_image.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
        _ctx.end_one_shot(cmd);

        // Render pass for scene
        _create_scene_render_pass();

        // Framebuffer
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass      = _scene_render_pass;
        fci.attachmentCount = 1;
        fci.pAttachments    = &_scene_image.view;
        fci.width  = _extent.width;
        fci.height = _extent.height;
        fci.layers = 1;
        vk_check(vkCreateFramebuffer(_ctx.device, &fci, nullptr, &_scene_framebuffer),
                  "vkCreateFramebuffer (post-process scene)");
    }

    void _create_scene_render_pass() {
        VkAttachmentDescription att{};
        att.format         = VK_FORMAT_R8G8B8A8_UNORM;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; // ready for composite

        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments    = &ref;

        // Dependency: ensure composite pass waits for scene to finish writing
        VkSubpassDependency dep{};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = 0;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpi.attachmentCount = 1;
        rpi.pAttachments    = &att;
        rpi.subpassCount    = 1;
        rpi.pSubpasses      = &subpass;
        rpi.dependencyCount = 1;
        rpi.pDependencies   = &dep;
        vk_check(vkCreateRenderPass(_ctx.device, &rpi, nullptr, &_scene_render_pass),
                  "vkCreateRenderPass (post-process scene)");
    }

    void _destroy_scene_resources() {
        if (_scene_framebuffer) {
            vkDestroyFramebuffer(_ctx.device, _scene_framebuffer, nullptr);
            _scene_framebuffer = VK_NULL_HANDLE;
        }
        if (_scene_render_pass) {
            vkDestroyRenderPass(_ctx.device, _scene_render_pass, nullptr);
            _scene_render_pass = VK_NULL_HANDLE;
        }
        if (_scene_image.view) {
            vkDestroyImageView(_ctx.device, _scene_image.view, nullptr);
            _scene_image.view = VK_NULL_HANDLE;
        }
        if (_scene_image.image) {
            vmaDestroyImage(_ctx.allocator, _scene_image.image, _scene_image.alloc);
            _scene_image.image = VK_NULL_HANDLE;
            _scene_image.alloc = VK_NULL_HANDLE;
        }
    }

    // ── Composite pipeline ───────────────────────────────────────────────────
    void _create_descriptor_layout() {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 1;
        ci.pBindings    = &binding;
        vk_check(vkCreateDescriptorSetLayout(_ctx.device, &ci, nullptr, &_desc_set_layout),
                  "vkCreateDescriptorSetLayout (post-process composite)");

        // Bilinear sampler for the offscreen color image
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vk_check(vkCreateSampler(_ctx.device, &sci, nullptr, &_sampler),
                  "vkCreateSampler (post-process)");
    }

    void _create_composite_pipeline(const std::string& shader_dir) {
        VkPushConstantRange pc_range{};
        pc_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pc_range.size       = sizeof(PostProcessPushConstants);

        VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lci.setLayoutCount        = 1;
        lci.pSetLayouts           = &_desc_set_layout;
        lci.pushConstantRangeCount = 1;
        lci.pPushConstantRanges   = &pc_range;
        vk_check(vkCreatePipelineLayout(_ctx.device, &lci, nullptr, &_comp_layout),
                  "vkCreatePipelineLayout (post-process composite)");

        auto load_spv = [](const std::string& path) {
            FILE* f = fopen(path.c_str(), "rb");
            if (!f) throw std::runtime_error("Cannot open shader: " + path);
            fseek(f, 0, SEEK_END); size_t sz = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
            std::vector<char> code(sz);
            fread(code.data(), 1, sz, f); fclose(f);
            return code;
        };

        auto make_module = [&](const std::vector<char>& code) {
            VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            ci.codeSize = code.size();
            ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
            VkShaderModule mod;
            vk_check(vkCreateShaderModule(_ctx.device, &ci, nullptr, &mod), "vkCreateShaderModule");
            return mod;
        };

        auto vert_code = load_spv(shader_dir + "/fullscreen.vert.spv");
        auto frag_code = load_spv(shader_dir + "/composite.frag.spv");
        VkShaderModule vert = make_module(vert_code);
        VkShaderModule frag = make_module(frag_code);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vert; stages[0].pName = "main";
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = frag; stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1; vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|
                              VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        cba.blendEnable = VK_FALSE; // composite result is written opaque
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &cba;

        std::vector<VkDynamicState> dyn = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn_ci{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn_ci.dynamicStateCount = (uint32_t)dyn.size(); dyn_ci.pDynamicStates = dyn.data();

        // Build a throwaway render pass compatible with the swapchain format
        // (composite draws into the swapchain's existing render pass; we just
        // need the pipeline to be compatible with *a* render pass of the same
        // format — the actual render pass handle used at draw time may differ).
        VkAttachmentDescription att{};
        att.format         = _swapchain_format;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments    = &ref;
        VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpi.attachmentCount = 1; rpi.pAttachments = &att;
        rpi.subpassCount    = 1; rpi.pSubpasses   = &subpass;
        VkRenderPass compat_rp;
        vk_check(vkCreateRenderPass(_ctx.device, &rpi, nullptr, &compat_rp),
                  "vkCreateRenderPass (composite compat)");

        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.stageCount          = 2;
        ci.pStages             = stages;
        ci.pVertexInputState   = &vin;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState      = &vps;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState   = &ms;
        ci.pColorBlendState    = &cb;
        ci.pDynamicState       = &dyn_ci;
        ci.layout              = _comp_layout;
        ci.renderPass          = compat_rp;
        ci.subpass             = 0;
        vk_check(vkCreateGraphicsPipelines(_ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &_comp_pipeline),
                  "vkCreateGraphicsPipelines (composite)");

        vkDestroyRenderPass(_ctx.device, compat_rp, nullptr);
        vkDestroyShaderModule(_ctx.device, vert, nullptr);
        vkDestroyShaderModule(_ctx.device, frag, nullptr);
    }

    void _create_descriptor_set() {
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = 1; pci.poolSizeCount = 1; pci.pPoolSizes = &size;
        vk_check(vkCreateDescriptorPool(_ctx.device, &pci, nullptr, &_desc_pool),
                  "vkCreateDescriptorPool (post-process composite)");

        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = _desc_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &_desc_set_layout;
        vk_check(vkAllocateDescriptorSets(_ctx.device, &ai, &_desc_set),
                  "vkAllocateDescriptorSets (composite)");

        _update_descriptor_set();
    }

    void _update_descriptor_set() {
        VkDescriptorImageInfo img{_sampler, _scene_image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet          = _desc_set;
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo      = &img;
        vkUpdateDescriptorSets(_ctx.device, 1, &write, 0, nullptr);
    }

    Context& _ctx;
    VkExtent2D    _extent;
    VkFormat      _swapchain_format;

    OffscreenImage _scene_image;
    VkRenderPass   _scene_render_pass   = VK_NULL_HANDLE;
    VkFramebuffer  _scene_framebuffer   = VK_NULL_HANDLE;

    VkDescriptorSetLayout _desc_set_layout = VK_NULL_HANDLE;
    VkSampler             _sampler         = VK_NULL_HANDLE;
    VkPipelineLayout      _comp_layout     = VK_NULL_HANDLE;
    VkPipeline            _comp_pipeline   = VK_NULL_HANDLE;
    VkDescriptorPool      _desc_pool       = VK_NULL_HANDLE;
    VkDescriptorSet       _desc_set        = VK_NULL_HANDLE;
};

} // namespace vkr
