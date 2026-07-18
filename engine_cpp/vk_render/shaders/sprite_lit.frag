#version 450
/*
 * sprite_lit.frag — fragment shader for Sprite-Lit materials.
 *
 * Replaces the old fake "additive circle" Light2D rendering with real per-pixel
 * lighting:
 *   - Inverse-square / smooth radial falloff per light
 *   - Optional normal-map dot product (tangent-space XY normals stored in
 *     R8G8 of the normal texture, Z reconstructed)
 *   - Up to kMaxLights (16) active lights, uploaded once per frame as a UBO
 *
 * Without a normal map the lighting is purely radial (think Unity's Light2D
 * "freeform" / "global" mode without normals). With a normal map the light
 * direction from each light to the fragment pixel is dotted against the
 * decoded surface normal, giving proper Lambertian shading.
 *
 * Push-constant layout is IDENTICAL to sprite.frag so both shaders share the
 * same pipeline layout. The extra data (light UBO, normal map sampler) arrives
 * via descriptor sets:
 *   set 0, binding 0 — combined-image-sampler  (albedo, same as sprite.frag)
 *   set 0, binding 1 — combined-image-sampler  (normal map)
 *   set 1, binding 0 — uniform buffer          (LightUBO)
 */

// ── Inputs from vertex stage ──────────────────────────────────────────────────
layout(location = 0) in vec2 frag_uv;
layout(location = 1) in vec4 frag_color;
// World-space position of this fragment, written by sprite_lit.vert
layout(location = 2) in vec2 frag_world_pos;

layout(location = 0) out vec4 out_color;

// ── Descriptors ───────────────────────────────────────────────────────────────
layout(set = 0, binding = 0) uniform sampler2D albedo_sampler;
layout(set = 0, binding = 1) uniform sampler2D normal_sampler;

// Per-light data, mirroring vk_light_ubo.hpp's GpuLight struct exactly
struct GpuLight {
    vec2  position;   // world-space centre
    float radius;     // falloff radius (0 = light does nothing)
    float intensity;  // linear intensity multiplier
    vec4  color;      // pre-multiplied RGB, A unused
};

const int kMaxLights = 16;

layout(set = 1, binding = 0) uniform LightUBO {
    GpuLight lights[kMaxLights];
    int      light_count;
    int      _pad0, _pad1, _pad2;
    vec4     ambient; // RGB colour, A intensity
} u_lights;

// ── Push constants (identical layout to sprite.frag) ─────────────────────────
layout(push_constant) uniform PushConstants {
    vec2  viewport_size;   // offset  0 — vertex stage
    float alpha_cutoff;    // offset  8 — fragment: < 0 disables cutout
    int   use_texture;     // offset 12 — 0 = solid color fill (no albedo sample)
    int   use_normal_map;  // offset 16 — 0 = skip normal sampling
    float light_strength;  // offset 20 — material light multiplier (1.0 = normal)
} pc;

// ── Smooth radial falloff ────────────────────────────────────────────────────
// smoothstep-based falloff that reaches exactly 0 at `radius` and 1 at centre.
float radial_falloff(float dist2, float radius2) {
    if (radius2 <= 0.0) return 0.0;
    float t = clamp(dist2 / radius2, 0.0, 1.0);
    // Smooth cubic ease-out: cheap, natural-looking and exactly 0 at edge.
    return 1.0 - (t * t * (3.0 - 2.0 * t));
}

void main() {
    // ── Albedo ────────────────────────────────────────────────────────────────
    vec4 base = (pc.use_texture != 0)
                ? texture(albedo_sampler, frag_uv)
                : vec4(1.0);
    vec4 albedo = base * frag_color;

    // Sprite-Cutout discard (same logic as sprite.frag)
    if (pc.alpha_cutoff >= 0.0 && albedo.a < pc.alpha_cutoff) discard;

    // ── Normal ────────────────────────────────────────────────────────────────
    // Normal map stores tangent-space XY in RG, biased [0,1] -> [-1,1].
    // Z is reconstructed assuming unit length (same convention as Unity's
    // default normal-map import: DXT5nm / BC5 packing or raw R8G8).
    vec3 surface_normal;
    if (pc.use_normal_map != 0) {
        vec2 rg = texture(normal_sampler, frag_uv).rg * 2.0 - 1.0;
        float z = sqrt(max(0.0, 1.0 - dot(rg, rg)));
        surface_normal = normalize(vec3(rg, z));
    } else {
        // No normal map — treat surface as facing directly toward camera (0,0,1)
        // so all lights contribute purely based on distance falloff alone.
        surface_normal = vec3(0.0, 0.0, 1.0);
    }

    // ── Accumulate lights ─────────────────────────────────────────────────────
    // Project-level ambient lighting is supplied by Shadow 2D Settings.
    // Keeping it in the same UBO makes changes live without rebuilding a
    // material or loading a second shader variant.
    vec3 light_accum = u_lights.ambient.rgb * u_lights.ambient.a;
    for (int i = 0; i < u_lights.light_count && i < kMaxLights; ++i) {
        GpuLight L = u_lights.lights[i];
        if (L.radius <= 0.0 || L.intensity <= 0.0) continue;

        vec2  delta  = L.position - frag_world_pos;
        float dist2   = dot(delta, delta);
        float radius2 = L.radius * L.radius;
        if (dist2 >= radius2) continue;

        float atten = radial_falloff(dist2, radius2);

        float ndotl = 1.0;
        if (pc.use_normal_map != 0) {
            // Normalized light direction with a fixed positive Z so the
            // normal-map contribution remains soft and never collapses to
            // a harsh fully-dark silhouette.
            float inv_dist = inversesqrt(max(dist2, 1e-8));
            vec3 light_dir = normalize(vec3(delta * inv_dist, L.radius * 0.5));
            ndotl = max(dot(surface_normal, light_dir), 0.0);
        }

        light_accum += L.color.rgb * L.intensity * atten * ndotl;
    }

    // ── Composite ─────────────────────────────────────────────────────────────
    // Apply material light_strength before clamping so authors can dial down
    // or boost the lighting contribution per-material.
    // Clamp so overbright lights saturate to white rather than wrapping.
    // Alpha is passed through unchanged so cutout/transparency works normally.
    out_color = vec4(min(albedo.rgb * light_accum * pc.light_strength, vec3(1.0)), albedo.a);
}
