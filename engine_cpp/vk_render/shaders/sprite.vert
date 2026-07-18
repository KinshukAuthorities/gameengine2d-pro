#version 450
/*
 * sprite.vert — vertex shader for the 2D sprite batch.
 *
 * Every draw call in the old SDL2 renderer (SDL_RenderCopyEx, RenderFillRect,
 * RenderDrawLine) ultimately became "some axis-aligned or rotated quad with
 * a position, size, source UV rect, and a color" by the time it reached the
 * GPU driver internally. This shader is that same idea made explicit: the
 * CPU-side batcher (vk_renderer_backend.hpp) writes one Vertex per corner
 * into a per-frame dynamic buffer, and this shader just transforms screen
 * pixel coordinates into Vulkan's [-1,1] clip space.
 *
 * Screen-space convention matches the old engine exactly: (0,0) = top-left,
 * +X = right, +Y = down (same as SDL_Rect / Camera::world_to_screen).
 */

layout(location = 0) in vec2 in_pos;       // screen-space pixels
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;     // 0..1 RGBA, already includes opacity/tint

layout(location = 0) out vec2 frag_uv;
layout(location = 1) out vec4 frag_color;

// Push constant: just the viewport size, so this one pipeline works for
// both the main swapchain target and any offscreen render target (editor
// viewport, render_to_bytes) without rebuilding a projection matrix buffer
// per resize — mirrors how the old SDL renderer's coordinate space was
// always just "whatever SDL_GetRendererOutputSize() reported".
// Push constant: viewport size for NDC conversion (this stage), plus
// per-draw-call cutout/texture flags consumed by sprite.frag. Both stages
// share ONE push constant block — see sprite.frag for the full struct and
// vk_renderer_backend.hpp for the matching VkPushConstantRange split.
layout(push_constant) uniform PushConstants {
    vec2 viewport_size;
    float alpha_cutoff;
    int use_texture;
} pc;

void main() {
    vec2 ndc = (in_pos / pc.viewport_size) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    frag_uv = in_uv;
    frag_color = in_color;
}
