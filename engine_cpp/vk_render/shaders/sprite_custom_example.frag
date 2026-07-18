#version 450
/*
 * sprite_custom_example.frag — starter template for custom material shaders.
 *
 * Compatible with the unlit pipeline layout in vk_sprite_batch.hpp:
 *   push_constant: PushConstants (viewport_size, alpha_cutoff, use_texture)
 *   set 0 binding 0: sampler2D (sprite albedo)
 *
 * To compile:
 *   glslc sprite_custom_example.frag -o sprite_custom_example.frag.spv
 *
 * Then drag the .spv into the "Frag SPV" field of any Material in the editor.
 * The engine automatically sets "Vert SPV" to the built-in sprite.vert if
 * only the fragment shader is custom — but you MUST set both fields if you
 * supply a custom vert. See material_system.hpp for full docs.
 *
 * Example effect below: desaturate + tint toward a hot "ember" palette.
 * Replace the effect block with anything you like.
 */

layout(push_constant) uniform PushConstants {
    vec2  viewport_size;
    float alpha_cutoff;
    int   use_texture;
    int   use_normal_map; // ignored in unlit
    float light_strength; // ignored in unlit
} pc;

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) in  vec2 frag_uv;
layout(location = 1) in  vec4 frag_color;
layout(location = 0) out vec4 out_color;

void main() {
    vec4 base = pc.use_texture == 1 ? texture(tex, frag_uv) : vec4(1.0);
    base *= frag_color;

    // Alpha test (mirrors Sprite-Cutout; noop when alpha_cutoff < 0)
    if (pc.alpha_cutoff >= 0.0 && base.a < pc.alpha_cutoff) discard;

    // ── Custom effect: ember/heat desaturate ──────────────────────────────
    float luma = dot(base.rgb, vec3(0.299, 0.587, 0.114));
    // Shift toward hot orange on bright areas, deep red on dark areas
    vec3 ember = mix(vec3(0.6, 0.05, 0.0), vec3(1.0, 0.55, 0.1), luma);
    // Blend 70% toward the ember palette, keep 30% original color
    base.rgb = mix(base.rgb, ember, 0.7);

    out_color = base;
}
