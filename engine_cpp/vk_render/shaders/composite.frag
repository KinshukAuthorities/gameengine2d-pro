#version 450

// composite.frag — post-process composite pass.
//
// Applies to the scene color image:
//   1. Simple bloom  — brightpass threshold → 3-tap Gaussian blur in-shader
//      (no separate blur pass needed for moderate bloom; add a ping-pong
//       blur pass if you need strong bloom).
//   2. Exposure      — linear exposure multiplier before tonemapping.
//   3. Color grading — saturation, contrast (pivot at 0.5).
//   4. Vignette      — radial edge darkening.
//   5. Reinhard tonemap — keeps HDR-ish workflow without a full HDR swapchain.

layout(set = 0, binding = 0) uniform sampler2D u_scene;

layout(push_constant) uniform PC {
    float bloom_threshold;
    float bloom_intensity;
    float exposure;
    float vignette;
    float saturation;
    float contrast;
    float _pad0;
    float _pad1;
};

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;

// Luminance (BT.709 weights)
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// Reinhard tonemap per-channel
vec3 tonemap(vec3 c) { return c / (c + vec3(1.0)); }

void main() {
    vec2 texel = 1.0 / vec2(textureSize(u_scene, 0));

    // ── 1. Scene sample ──────────────────────────────────────────────────────
    vec3 color = texture(u_scene, v_uv).rgb;

    // ── 2. Bloom (3×3 brightpass box blur baked here) ────────────────────────
    if (bloom_intensity > 0.0) {
        vec3 bloom = vec3(0.0);
        float weight = 0.0;
        // 3×3 kernel
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                vec3 s = texture(u_scene, v_uv + vec2(dx, dy) * texel).rgb;
                float l = luma(s);
                if (l > bloom_threshold) {
                    float w = l - bloom_threshold;
                    bloom  += s * w;
                    weight += w;
                }
            }
        }
        if (weight > 0.0) bloom /= weight;
        color += bloom * bloom_intensity;
    }

    // ── 3. Exposure ──────────────────────────────────────────────────────────
    color *= exposure;

    // ── 4. Color grading ─────────────────────────────────────────────────────
    // Saturation
    float grey = luma(color);
    color = mix(vec3(grey), color, saturation);

    // Contrast (pivot at 0.5 in linear space)
    color = (color - 0.5) * contrast + 0.5;
    color = max(vec3(0.0), color);

    // ── 5. Tonemap ───────────────────────────────────────────────────────────
    color = tonemap(color);

    // ── 6. Vignette ──────────────────────────────────────────────────────────
    if (vignette > 0.0) {
        vec2 uv_centered = v_uv * 2.0 - 1.0;
        float dist = dot(uv_centered, uv_centered); // 0 at center, 1 at corner
        float vig  = 1.0 - dist * vignette;
        color *= clamp(vig, 0.0, 1.0);
    }

    out_color = vec4(color, 1.0);
}
