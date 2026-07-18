#version 450
/*
 * sprite.frag — fragment shader for the 2D sprite batch.
 *
 * Covers everything material_system.hpp's fixed "shader" enum used to mean
 * under SDL2 (where there was no real shader, just SDL_SetTextureColorMod/
 * AlphaMod/BlendMode calls):
 *
 *   Sprite-Unlit / Sprite-Lit  -> normal alpha blend (BLEND pipeline variant)
 *   Sprite-Additive            -> additive blend (ADD pipeline variant);
 *                                  blending itself happens in fixed-function
 *                                  blend state, NOT here — see vk_pipeline
 *                                  creation in vk_renderer_backend.hpp
 *   Sprite-Cutout               -> alpha_cutoff discard, handled here
 *
 * "use_texture" lets the same pipeline draw untextured fills/lines (the old
 * SDL_RenderFillRect / SDL_RenderDrawLine calls in UI panels, debug lines,
 * tilemap fallback shading, 9-slice borders) without a dummy white texture
 * trick — just multiply color by 1 instead of sampling.
 */

layout(location = 0) in vec2 frag_uv;
layout(location = 1) in vec4 frag_color;

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D tex_sampler;

// Push constant: viewport size for NDC conversion (vertex stage), plus
// per-draw-call cutout/texture flags (fragment stage). Both stages share
// ONE push constant block in Vulkan — vk_renderer_backend.hpp's pipeline
// layout declares the matching combined VkPushConstantRange for each stage
// so the byte offsets below line up exactly with sprite.vert's struct.
layout(push_constant) uniform PushConstants {
    vec2 viewport_size;     // offset 0  (vertex stage)
    float alpha_cutoff;     // offset 8  (fragment stage) — negative = disabled
    int use_texture;        // offset 12 (fragment stage) — 0 = solid color
} pc;

void main() {
    vec4 base = (pc.use_texture != 0) ? texture(tex_sampler, frag_uv) : vec4(1.0);
    vec4 result = base * frag_color;

    // Sprite-Cutout: pixels below the threshold are fully discarded rather
    // than blended, matching Unity2D's cutout shader (hard edge, no
    // translucency) instead of the soft edge normal alpha blending gives.
    if (pc.alpha_cutoff >= 0.0 && result.a < pc.alpha_cutoff) discard;

    out_color = result;
}
