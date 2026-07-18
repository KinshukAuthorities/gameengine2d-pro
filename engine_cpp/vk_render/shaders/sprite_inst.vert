#version 450
/*
 * sprite_inst.vert — instanced vertex shader for the GPU-instanced sprite path.
 *
 * When SpriteRenderer.gpu_instancing is enabled, SpriteBatch routes the quad
 * through a separate instanced pipeline instead of the CPU-baked vertex path.
 *
 * Vertex buffer  (binding 0, rate=vertex):  a single static unit-quad [-0.5,0.5]
 *   in normalised sprite-local space (4 vertices, 6 indices, never changes).
 *
 * Instance buffer (binding 1, rate=instance): one InstanceData per sprite,
 *   written CPU-side into a per-frame VMA host-visible buffer exactly like the
 *   existing Vertex buffer path — same host-visible, per-frame-slot design,
 *   just with far fewer bytes per sprite (no 4x vertex fan expansion on the CPU).
 *
 *   Per-instance layout (matches InstanceData struct in vk_sprite_batch.hpp):
 *     location 3:  vec2  screen_pos       — screen-space pixel centre (from world_to_screen)
 *     location 4:  vec2  size             — pixel width, height after zoom+scale
 *     location 5:  float rotation         — radians, clockwise
 *     location 6:  vec2  pivot            — normalised [0,1] from top-left
 *     location 7:  vec4  uv_rect          — (u0,v0,u1,v1)
 *     location 8:  vec4  color            — 0..1 RGBA tint+opacity
 *     location 9:  vec2  flip             — (flip_x ? -1:1, flip_y ? -1:1)
 *
 * Screen-space convention: (0,0)=top-left, +X right, +Y down — same as every
 * other shader in this engine (sprite.vert, sprite_lit.vert).
 */

// ── Vertex inputs (unit quad, binding 0) ─────────────────────────────────────
layout(location = 0) in vec2 in_local_pos;   // [-0.5, 0.5] unit quad

// ── Instance inputs (per sprite, binding 1) ──────────────────────────────────
layout(location = 3) in vec2  inst_screen_pos;
layout(location = 4) in vec2  inst_size;
layout(location = 5) in float inst_rotation;
layout(location = 6) in vec2  inst_pivot;
layout(location = 7) in vec4  inst_uv_rect;   // u0,v0,u1,v1
layout(location = 8) in vec4  inst_color;
layout(location = 9) in vec2  inst_flip;

// ── Fragment outputs ──────────────────────────────────────────────────────────
layout(location = 0) out vec2 frag_uv;
layout(location = 1) out vec4 frag_color;

// ── Push constants (shared with sprite.frag) ─────────────────────────────────
layout(push_constant) uniform PushConstants {
    vec2  viewport_size;
    float alpha_cutoff;
    int   use_texture;
} pc;

void main() {
    // 1. Apply flip to local position
    vec2 local = in_local_pos * inst_flip;

    // 2. Scale from unit space to pixel size, accounting for pivot offset.
    //    Unity pivot: (0,0)=bottom-left, (1,1)=top-right; our convention
    //    matches the CPU path — pivot_y from top, so (0.5,0.5) = centre.
    vec2 pivot_offset = (inst_pivot - vec2(0.5)) * inst_size;
    vec2 scaled = local * inst_size - pivot_offset;

    // 3. Rotate in screen space (clockwise, +Y down).
    float s = sin(inst_rotation);
    float c = cos(inst_rotation);
    vec2 rotated = vec2(
        scaled.x * c - scaled.y * s,
        scaled.x * s + scaled.y * c
    );

    // 4. Translate to screen position.
    vec2 screen_px = inst_screen_pos + rotated;

    // 5. Convert screen pixels → Vulkan NDC.  Negative-Y viewport in
    //    SpriteBatch::flush() flips Y so (0,0) stays top-left.
    vec2 ndc = (screen_px / pc.viewport_size) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);

    // 6. UV: map unit-quad [−0.5,0.5] corner to [u0,u1] × [v0,v1].
    //    in_local_pos goes −0.5..+0.5; remap to 0..1 first, then into the rect.
    vec2 uv01 = in_local_pos + 0.5;           // 0..1
    frag_uv = mix(inst_uv_rect.xy, inst_uv_rect.zw, uv01);

    frag_color = inst_color;
}
