#version 450
/*
 * sprite_lit.vert — vertex shader for Sprite-Lit materials.
 *
 * Identical to sprite.vert except it also outputs frag_world_pos so the
 * fragment shader can compute per-fragment distances to each light without
 * needing a second pass. Both push-constant and vertex layout are
 * backwards-compatible with the sprite batch's existing Vertex / PushConstants
 * structs — the lit pipeline can reuse the same vertex buffer and layout.
 *
 * The extra push-constant fields (use_normal_map, _pad) are declared here but
 * not consumed — they exist solely to keep the PushConstants block byte-layout
 * identical between sprite.vert / sprite_lit.vert so both share one
 * VkPipelineLayout.
 */

layout(location = 0) in vec2 in_pos;    // screen-space pixels
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec2 frag_uv;
layout(location = 1) out vec4 frag_color;
layout(location = 2) out vec2 frag_world_pos; // extra output consumed by sprite_lit.frag

layout(push_constant) uniform PushConstants {
    vec2  viewport_size;
    float alpha_cutoff;
    int   use_texture;
    int   use_normal_map;
    float _pad;
} pc;

// World-origin and pixels-per-unit are injected as a small UBO so the vertex
// shader can invert the camera transform cheaply.  We reuse set=1 binding=0
// (the LightUBO) header fields for this: the first two floats of LightUBO are
// intentionally the camera origin in world space.  However, since the layout
// std140 packs GpuLight[0] at offset 0 immediately (the light_count is at the
// END), we instead pass camera data via a dedicated set=2 UBO.
//
// Simpler alternative used here: pass camera_offset and pixels_per_unit via
// push constants — but push constant space is tight (128 bytes minimum).
//
// ─── Chosen approach: store camera data in a tiny separate UBO at set=2 ──────
// See vk_light_ubo.hpp's CameraUBO struct and SpriteBatch::begin_frame_lit().
layout(set = 2, binding = 0) uniform CameraUBO {
    vec2  camera_world_pos; // world-space position of screen centre
    float pixels_per_unit;  // zoom * base_ppu (how many pixels = 1 world unit)
    float viewport_w;
    float viewport_h;
    float _pad0, _pad1, _pad2;
} u_cam;

void main() {
    // Standard NDC conversion (same as sprite.vert)
    vec2 ndc = (in_pos / pc.viewport_size) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    frag_uv    = in_uv;
    frag_color = in_color;

    // Invert camera transform to recover world position from screen pixels.
    // screen_pos = (world_pos - cam_pos) * ppu + (viewport/2)
    // => world_pos = (screen_pos - viewport/2) / ppu + cam_pos
    vec2 screen_centre = vec2(u_cam.viewport_w, u_cam.viewport_h) * 0.5;
    frag_world_pos = (in_pos - screen_centre) / u_cam.pixels_per_unit
                     + u_cam.camera_world_pos;
}
