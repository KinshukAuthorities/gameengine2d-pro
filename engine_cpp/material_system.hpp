#pragma once
/*
 * material_system.hpp — Unity2D-style Material / Shader asset system.
 *
 * This engine renders through a real Vulkan programmable pipeline.
 * A Material configures which pipeline variant a SpriteRenderer uses and
 * supplies per-material parameters.
 *
 * ── Built-in shader variants ────────────────────────────────────────────────
 *
 *   shader          "Sprite-Unlit"    — standard alpha-blended, no lighting
 *                   "Sprite-Lit"      — lit by scene Light2D components via
 *                                       the per-frame GpuLight UBO; supports
 *                                       an optional normal map for NdotL shading
 *                   "Sprite-Additive" — additive blend (glow / VFX)
 *                   "Sprite-Cutout"   — alpha test: pixels below alpha_cutoff
 *                                       are fully discarded (no blending)
 *
 * ── Custom shaders (task 13) ────────────────────────────────────────────────
 *
 * Set custom_vert_spv / custom_frag_spv to paths of pre-compiled SPIR-V
 * files (relative to the project's assets folder, same resolution rules as
 * textures).  When both are non-empty, SpriteBatch uses those modules instead
 * of sprite.vert / sprite.frag.  The pipeline is cached on first use and
 * reused for all subsequent quads that share the same (vert, frag, blend)
 * combination — see SpriteBatch::PipelineCache.
 *
 * Custom shaders must be compatible with the unlit pipeline layout:
 *   - push constants: PushConstants struct (viewport_size, alpha_cutoff,
 *     use_texture) — same layout and size as sprite.vert/sprite.frag
 *   - set 0, binding 0: combined-image-sampler (the sprite albedo texture)
 *   - vertex inputs:  location 0 = vec2 pos, 1 = vec2 uv, 2 = vec4 color
 *     (same as Vertex in vk_sprite_batch.hpp)
 * The built-in "Sprite-Lit" pipeline uses a different layout (set 0 has two
 * samplers + separate UBO sets for lights and camera) — custom lit shaders
 * that need that layout must replicate it exactly or they will mis-bind.
 * For simplicity, custom shaders typically extend the unlit layout.
 *
 * To compile a GLSL shader to SPIR-V in your project's CMakeLists.txt:
 *   find_program(GLSLC glslc HINTS ENV VULKAN_SDK)
 *   add_custom_command(OUTPUT my.frag.spv
 *     COMMAND ${GLSLC} my.frag -o my.frag.spv
 *     DEPENDS my.frag)
 *
 * ── Other per-material fields ────────────────────────────────────────────────
 *
 *   color            RGBA tint, MULTIPLIED with the SpriteRenderer's own color
 *   texture          optional texture override (empty = use the sprite's own texture)
 *   normal_map       optional tangent-space normal map for "Sprite-Lit" materials
 *                    (leave empty for pure radial-falloff lighting without surface normals)
 *   alpha_cutoff     for "Sprite-Cutout": pixels with alpha below this are fully discarded
 *   filter_mode      "point" | "bilinear" — overrides the sprite's own filter mode
 *   render_queue_offset  extra order-in-layer bias, like Unity's material render queue
 *
 * Materials live on disk as small ".material" JSON files under a project's
 * assets folder (see component_defs.hpp's Material defaults for the schema)
 * and are referenced from a SpriteRenderer by path via its "material" field
 * — same relationship as Unity: Sprite Renderer -> Material -> Shader.
 *
 * MaterialCache loads/parses each file once and hands out a shared pointer,
 * mirroring Unity's sharedMaterial (many renderers, one asset, edits affect
 * everyone using it) — see render_system_vk.hpp's _draw_sprite() for where
 * the fields below actually get applied to the draw call.
 */

#include "entity.hpp"
#include "vk_render/texture_system_vk.hpp"
#include <nlohmann/json.hpp>
#include <SDL2/SDL.h>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <memory>
#include <filesystem>

namespace material {

enum class Shader { SpriteUnlit, SpriteLit, SpriteAdditive, SpriteCutout };

inline Shader shader_from_string(const std::string& s) {
    if (s == "Sprite-Lit")       return Shader::SpriteLit;
    if (s == "Sprite-Additive")  return Shader::SpriteAdditive;
    if (s == "Sprite-Cutout")    return Shader::SpriteCutout;
    return Shader::SpriteUnlit;
}
inline std::string shader_to_string(Shader s) {
    switch (s) {
        case Shader::SpriteLit:      return "Sprite-Lit";
        case Shader::SpriteAdditive: return "Sprite-Additive";
        case Shader::SpriteCutout:   return "Sprite-Cutout";
        default:                     return "Sprite-Unlit";
    }
}

struct MaterialAsset {
    std::string name        = "New Material";
    Shader      shader       = Shader::SpriteUnlit;
    Uint8       color[4]    = {255,255,255,255}; // multiplied with SpriteRenderer.color
    std::string texture;                          // optional override texture path
    float       alpha_cutoff = 0.5f;               // Sprite-Cutout threshold (0..1)
    std::string filter_mode  = "";                 // "" = don't override; else "point"/"bilinear"
    int         render_queue_offset = 0;           // extra order-in-layer bias

    // Lighting knobs for "Sprite-Lit": used by the real 2D lighting pipeline
    // (sprite_lit.frag).  Unlit sprites ignore scene lights entirely, exactly
    // like Unity's Sprite-Unlit-Default vs Sprite-Lit-Default.
    float       light_strength = 1.0f;

    // Optional normal-map texture (second sampler in sprite_lit.frag).
    // Leave empty for pure radial-falloff lighting without surface normals.
    // Should reference a linear (non-sRGB) R8G8(B8) tangent-space normal map;
    // set the texture's .meta "srgb": false so it uploads as UNORM (task 5).
    std::string normal_map;

    // ── Custom shader pair (task 13) ─────────────────────────────────────────
    // Paths to pre-compiled SPIR-V files, relative to the project's assets
    // folder (resolved by the same rules as texture paths).  When both are
    // non-empty, SpriteBatch uses these modules instead of the built-in
    // sprite.vert / sprite.frag.  The pipeline is created on first use and
    // cached by (vert_spv, frag_spv, BlendMode) in SpriteBatch::PipelineCache.
    //
    // Must be compatible with the unlit descriptor layout — see the file-level
    // comment above for the required interface (push constants, set 0 binding).
    // Leave both empty to use the built-in shader selected by `shader` above.
    std::string custom_vert_spv;   // e.g. "assets/shaders/my_sprite.vert.spv"
    std::string custom_frag_spv;   // e.g. "assets/shaders/my_sprite.frag.spv"

    // Returns true when this material requests a fully custom shader pair.
    bool has_custom_shader() const {
        return !custom_vert_spv.empty() && !custom_frag_spv.empty();
    }

    static MaterialAsset from_json(const Entity& j) {
        MaterialAsset m;
        m.name    = j.value("name", std::string("New Material"));
        m.shader  = shader_from_string(j.value("shader", std::string("Sprite-Unlit")));
        auto col  = j.value("color", std::vector<int>{255,255,255,255});
        for (int i=0;i<4 && i<(int)col.size();++i) m.color[i] = (Uint8)col[i];
        m.texture        = j.value("texture", std::string(""));
        m.alpha_cutoff   = j.value("alpha_cutoff", 0.5f);
        m.filter_mode    = j.value("filter_mode", std::string(""));
        m.render_queue_offset = j.value("render_queue_offset", 0);
        m.light_strength = j.value("light_strength", 1.0f);
        m.normal_map     = j.value("normal_map", std::string(""));
        m.custom_vert_spv = j.value("custom_vert_spv", std::string(""));
        m.custom_frag_spv = j.value("custom_frag_spv", std::string(""));
        return m;
    }

    Entity to_json() const {
        Entity j = Entity::object();
        j["name"] = name;
        j["shader"] = shader_to_string(shader);
        j["color"] = std::vector<int>{color[0],color[1],color[2],color[3]};
        j["texture"] = texture;
        j["alpha_cutoff"] = alpha_cutoff;
        j["filter_mode"] = filter_mode;
        j["render_queue_offset"] = render_queue_offset;
        j["light_strength"] = light_strength;
        j["normal_map"]     = normal_map;
        j["custom_vert_spv"] = custom_vert_spv;
        j["custom_frag_spv"] = custom_frag_spv;
        return j;
    }

    // Writes this material out as pretty-printed JSON, the same ".material"
    // format MaterialCache reads back — used by the editor's Material
    // Inspector (every field edit re-saves) and "Create > Material".
    // Resolves the filter mode actually used to draw, with the same
    // precedence Unity applies: an explicit Material override wins; failing
    // that, the Texture's own import setting (Unity: Texture2D.filterMode)
    // is used; failing that (no texture asset found), "point" is the
    // engine-wide default for crisp pixel art.
    static std::string effective_filter_mode(const MaterialAsset* mat,
                                              texture::TextureCache* textures,
                                              const std::string& texture_ref,
                                              const std::string& component_fallback) {
        if (mat && !mat->filter_mode.empty()) return mat->filter_mode;
        if (textures && !texture_ref.empty()) {
            std::string image_path = texture_ref;
            auto colon = texture_ref.find(':');
            if (colon != std::string::npos && colon > 1) image_path = texture_ref.substr(0, colon);
            if (auto* settings = textures->import_settings(image_path))
                return texture::filter_to_string(settings->filter_mode);
        }
        return component_fallback;
    }

    bool save_to_file(const std::string& path) const {
        std::ofstream f(path);
        if (!f) return false;
        nlohmann::json raw = to_json();   // Entity has a public operator nlohmann::json()
        f << raw.dump(2);
        return true;
    }
};

// Default new-material JSON, used both by the engine (if a referenced
// .material file is missing) and by the editor's "Create > Material".
inline MaterialAsset default_material() { return MaterialAsset{}; }

// ── MaterialCache ────────────────────────────────────────────────────────────
// One shared instance per (resolved) file path — Unity's sharedMaterial
// model. Re-reads from disk if the file's mtime changed since last load, so
// editing a .material in the Inspector and saving it (or another tool
// touching the file) is picked up without restarting.
class MaterialCache {
public:
    void set_base_dir(const std::string& dir) { _base_dir = dir; }

    // Resolves a path the same way TextureCache does (raw -> base/path ->
    // base/assets/path), loads + parses the JSON, and returns a shared,
    // cached MaterialAsset. Returns nullptr if path is empty or unreadable.
    std::shared_ptr<MaterialAsset> get(const std::string& path) {
        if (path.empty()) return nullptr;

        std::string resolved = _resolve(path);
        if (resolved.empty()) return nullptr;

        std::error_code ec;
        auto mtime = std::filesystem::last_write_time(resolved, ec);

        auto it = _cache.find(resolved);
        if (it != _cache.end() && !ec && it->second.mtime == mtime)
            return it->second.asset;

        std::ifstream f(resolved);
        if (!f) return (it != _cache.end()) ? it->second.asset : nullptr;
        nlohmann::json raw;
        try { f >> raw; }
        catch (...) { return (it != _cache.end()) ? it->second.asset : nullptr; }
        Entity j = raw;   // Entity has a public operator=(const nlohmann::json&)

        auto asset = std::make_shared<MaterialAsset>(MaterialAsset::from_json(j));
        _cache[resolved] = { asset, mtime };
        return asset;
    }

    void clear() { _cache.clear(); }

    // Expose path resolution so RenderSystem can resolve custom SPV paths
    // using the same base-dir logic as material files themselves.
    std::string resolve_path(const std::string& name) const { return _resolve(name); }

private:
    struct CacheEntry { std::shared_ptr<MaterialAsset> asset; std::filesystem::file_time_type mtime{}; };
    std::string _base_dir;
    std::unordered_map<std::string, CacheEntry> _cache;

    std::string _resolve(const std::string& name) const {
        namespace fs = std::filesystem;
        std::vector<std::string> candidates = { name };
        if (!_base_dir.empty()) {
            candidates.push_back(_base_dir + "/" + name);
            candidates.push_back(_base_dir + "/assets/" + name);
        }
        for (auto& c : candidates) if (fs::exists(c)) return c;
        return "";
    }
};

} // namespace material