#pragma once
#ifndef ENGINE_TEXTURE_SYSTEM_DEFINED
#define ENGINE_TEXTURE_SYSTEM_DEFINED
/*
 * texture_system_vk.hpp — Vulkan port of texture_system.hpp.
 *
 * Drop-in replacement: same namespace (texture::), same public types
 * (ResolvedSprite, TextureCache, FilterMode, WrapMode, SpriteRect,
 * TextureImportSettings) and — this is the part that matters for existing
 * projects — the EXACT SAME ".meta" JSON schema as the SDL2 version
 * (texture_system.hpp), field-for-field:
 *
 *   filter_mode, wrap_mode, pixels_per_unit, pivot_x, pivot_y,
 *   generate_mipmaps, srgb, sprite_mode ("single"|"multiple"),
 *   cell_width, cell_height, cell_offset_x, cell_offset_y,
 *   cell_padding_x, cell_padding_y, sprites[] (name,x,y,w,h,pivot_x,pivot_y)
 *
 * This matters because existing ".meta" sidecars on disk, and the editor's
 * Sprite Editor / Texture Inspector panels (panels.hpp), read and write
 * exactly this schema — diverging from it would silently break every
 * already-sliced spritesheet in a project the moment it's loaded under the
 * Vulkan backend.
 *
 * What changes vs the SDL2 version:
 *   - TextureCache ctor takes (vkr::Context&, base_dir) instead of (SDL_Renderer*, base_dir)
 *   - get() returns vkr::Texture* instead of SDL_Texture*
 *   - get_sprite() returns a ResolvedSprite holding vkr::Texture* + VkRect2D-shaped
 *     src fields (via SDL_Rect-compatible plain ints) instead of SDL_Texture pointer
 *     and SDL_Rect
 *   - Filter mode is baked into vkr::Texture::sampler (an immutable Vulkan
 *     sampler object) instead of a per-draw SDL_SetTextureScaleMode call;
 *     changing filter mode means re-creating the sampler (done automatically
 *     on hot-reload / .meta change)
 *   - Hot-reload invalidates any cached SpriteBatch descriptor sets pointing
 *     at the old VkImageView via a registered callback (RenderSystem wires
 *     this to SpriteBatch::descriptors().invalidate())
 *
 * What doesn't change:
 *   - .meta file schema, atlas slicing (uniform grid + named rects), pivot/PPU
 *     resolution, base-dir path resolution, mtime-based hot-reload polling.
 *   - All render_system_vk.hpp call sites use get()/get_sprite() exactly as
 *     the SDL2 render_system.hpp did; only the return types' pointee type
 *     differs (vkr::Texture* vs SDL_Texture*).
 */

#include "vk_context.hpp"
#include "vk_texture.hpp"
#include "entity.hpp"
#include <nlohmann/json.hpp>
#include <SDL2/SDL.h>

// Pixel decoding via stb_image.h (single-header, public domain — see
// engine_cpp/third_party/stb_image.h, implementation compiled once in
// vk_render/stb_image_impl.cpp). Replaces the old three-way fallback
// (SDL2_image when installed -> Windows WIC COM API -> SDL_LoadBMP-only)
// with one cross-platform decoder: PNG, JPEG, BMP, GIF, TGA, PSD all work
// identically on every platform, no install step, no Windows-only code.
#include "../third_party/stb_image.h"

#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <functional>

namespace texture {

// ── Filter / Wrap modes ─────────────────────────────────────────────────────
// Identical enum + string mapping to texture_system.hpp — material_system.hpp
// and render_system_vk.hpp use these without any changes.
enum class FilterMode { Point, Bilinear };
enum class WrapMode   { Clamp, Repeat };

inline FilterMode filter_from_string(const std::string& s) {
    return (s == "bilinear" || s == "linear") ? FilterMode::Bilinear : FilterMode::Point;
}
inline std::string filter_to_string(FilterMode f) {
    return f == FilterMode::Bilinear ? "bilinear" : "point";
}
inline WrapMode wrap_from_string(const std::string& s) {
    return (s == "repeat") ? WrapMode::Repeat : WrapMode::Clamp;
}
inline std::string wrap_to_string(WrapMode w) {
    return w == WrapMode::Repeat ? "repeat" : "clamp";
}

// ── SpriteRect ───────────────────────────────────────────────────────────────
// Byte-for-byte the same fields/JSON as texture_system.hpp's SpriteRect.
struct SpriteRect {
    std::string name;
    int   x = 0, y = 0, w = 0, h = 0;     // source pixels within the sheet
    float pivot_x = 0.5f, pivot_y = 0.5f;  // overrides the texture's default pivot
    bool  has_pivot_override = false;

    static SpriteRect from_json(const nlohmann::json& j) {
        SpriteRect s;
        s.name = j.value("name", std::string(""));
        s.x = j.value("x", 0); s.y = j.value("y", 0);
        s.w = j.value("w", 0); s.h = j.value("h", 0);
        if (j.contains("pivot_x") || j.contains("pivot_y")) {
            s.has_pivot_override = true;
            s.pivot_x = j.value("pivot_x", 0.5f);
            s.pivot_y = j.value("pivot_y", 0.5f);
        }
        return s;
    }
    nlohmann::json to_json() const {
        nlohmann::json j;
        j["name"] = name; j["x"] = x; j["y"] = y; j["w"] = w; j["h"] = h;
        if (has_pivot_override) { j["pivot_x"] = pivot_x; j["pivot_y"] = pivot_y; }
        return j;
    }
};

// ── TextureImportSettings ───────────────────────────────────────────────────
// Same fields/defaults/JSON keys as texture_system.hpp, including the
// cell_* grid-slicing parameters and slice_grid() regeneration logic used by
// the editor's "Slice -> Grid By Cell Size" Sprite Editor action.
struct TextureImportSettings {
    FilterMode filter_mode      = FilterMode::Point;
    WrapMode   wrap_mode        = WrapMode::Clamp;
    float      pixels_per_unit  = 100.0f;
    float      default_pivot_x  = 0.5f;
    float      default_pivot_y  = 0.5f;
    bool       generate_mipmaps = false;  // reserved, mirrors SDL2 version
    bool       srgb             = true;
    std::string sprite_mode = "single";   // "single" | "multiple"
    int cell_width = 16, cell_height = 16, cell_offset_x = 0, cell_offset_y = 0,
        cell_padding_x = 0, cell_padding_y = 0;
    std::vector<SpriteRect> sprites;      // only populated when sprite_mode == "multiple"

    static TextureImportSettings from_json(const nlohmann::json& j) {
        TextureImportSettings s;
        s.filter_mode      = filter_from_string(j.value("filter_mode", std::string("point")));
        s.wrap_mode        = wrap_from_string(j.value("wrap_mode", std::string("clamp")));
        s.pixels_per_unit  = j.value("pixels_per_unit", 100.0f);
        s.default_pivot_x  = j.value("pivot_x", 0.5f);
        s.default_pivot_y  = j.value("pivot_y", 0.5f);
        s.generate_mipmaps = j.value("generate_mipmaps", false);
        s.srgb             = j.value("srgb", true);
        s.sprite_mode      = j.value("sprite_mode", std::string("single"));
        s.cell_width   = j.value("cell_width", 16);
        s.cell_height  = j.value("cell_height", 16);
        s.cell_offset_x = j.value("cell_offset_x", 0);
        s.cell_offset_y = j.value("cell_offset_y", 0);
        s.cell_padding_x = j.value("cell_padding_x", 0);
        s.cell_padding_y = j.value("cell_padding_y", 0);
        if (j.contains("sprites") && j["sprites"].is_array())
            for (auto& sj : j["sprites"]) s.sprites.push_back(SpriteRect::from_json(sj));
        return s;
    }

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["filter_mode"]      = filter_to_string(filter_mode);
        j["wrap_mode"]        = wrap_to_string(wrap_mode);
        j["pixels_per_unit"]  = pixels_per_unit;
        j["pivot_x"] = default_pivot_x; j["pivot_y"] = default_pivot_y;
        j["generate_mipmaps"] = generate_mipmaps;
        j["srgb"]              = srgb;
        j["sprite_mode"]       = sprite_mode;
        j["cell_width"] = cell_width; j["cell_height"] = cell_height;
        j["cell_offset_x"] = cell_offset_x; j["cell_offset_y"] = cell_offset_y;
        j["cell_padding_x"] = cell_padding_x; j["cell_padding_y"] = cell_padding_y;
        auto arr = nlohmann::json::array();
        for (auto& s : sprites) arr.push_back(s.to_json());
        j["sprites"] = arr;
        return j;
    }

    bool save_to_file(const std::string& meta_path) const {
        std::ofstream f(meta_path);
        if (!f) return false;
        f << to_json().dump(2);
        return true;
    }

    const SpriteRect* find_sprite(const std::string& name) const {
        for (auto& s : sprites) if (s.name == name) return &s;
        return nullptr;
    }

    // Regenerates `sprites` as a uniform grid — identical algorithm to
    // texture_system.hpp's slice_grid() so re-slicing produces the same
    // sprite names/rects regardless of which renderer backend is active.
    void slice_grid(int image_w, int image_h) {
        sprites.clear();
        if (cell_width <= 0 || cell_height <= 0) return;
        int idx = 0;
        for (int y = cell_offset_y; y + cell_height <= image_h; y += cell_height + cell_padding_y) {
            for (int x = cell_offset_x; x + cell_width <= image_w; x += cell_width + cell_padding_x) {
                SpriteRect s;
                s.x = x; s.y = y; s.w = cell_width; s.h = cell_height;
                s.name = "sprite_" + std::to_string(idx++);
                sprites.push_back(s);
            }
        }
        sprite_mode = "multiple";
    }
};

// ── Resolved sprite ──────────────────────────────────────────────────────────
// What render_system_vk.hpp actually needs to draw one frame. Same semantics
// as texture_system.hpp's ResolvedSprite, with an SDL_Rect-compatible `src`
// member kept as plain ints (so callers don't need a Vulkan header to read
// it) and the GPU handle being vkr::Texture* instead of SDL_Texture*.
struct ResolvedSprite {
    vkr::Texture* texture = nullptr;
    SDL_Rect      src{0,0,0,0};
    bool          has_src = false;
    float         pivot_x = 0.5f, pivot_y = 0.5f;
    float         pixels_per_unit = 100.0f;
    FilterMode    filter_mode = FilterMode::Point;
    WrapMode      wrap_mode   = WrapMode::Clamp;
};

// ── TextureCache ─────────────────────────────────────────────────────────────
class TextureCache {
public:
    // Invoked right before a hot-reloaded texture's old VkImageView is
    // destroyed, so RenderSystem can drop any SpriteBatch descriptor sets
    // still pointing at it (see SpriteBatch::DescriptorCache::invalidate).
    using InvalidateCallback = std::function<void(VkImageView)>;

    explicit TextureCache(vkr::Context& ctx, std::string base_dir = "")
        : _ctx(ctx), _uploader(ctx), _base_dir(std::move(base_dir)) {}
    ~TextureCache() { clear(); }

    void set_base_dir(const std::string& d) { _base_dir = d; clear(); }
    void set_invalidate_callback(InvalidateCallback cb) { _invalidate_cb = std::move(cb); }

    // Whole-texture accessor — used by callers that don't care about
    // sprite-sheet slicing (UI images, tilesets, particles, parallax).
    vkr::Texture* get(const std::string& name) {
        auto* entry = _get_entry(name);
        return (entry && entry->tex.valid()) ? &entry->tex : nullptr;
    }

    TextureImportSettings* import_settings(const std::string& image_path) {
        auto* entry = _get_entry(image_path);
        return entry ? &entry->settings : nullptr;
    }

    bool save_import_settings(const std::string& image_path) {
        auto* entry = _get_entry(image_path);
        if (!entry) return false;
        return entry->settings.save_to_file(entry->resolved_path + ".meta");
    }

    // Replaces an in-memory texture owned by a runtime component.  This is
    // deliberately part of the same cache as file textures: materials and
    // SpriteRenderers can consume the returned key without a second, fragile
    // rendering path.  The caller supplies a stable key such as
    // "__runtime/crt/42"; no project file is ever created for it.
    vkr::Texture* replace_dynamic(const std::string& key, const uint8_t* pixels,
                                  uint32_t width, uint32_t height,
                                  FilterMode filter = FilterMode::Bilinear,
                                  WrapMode wrap = WrapMode::Clamp,
                                  bool srgb = false) {
        if (key.empty() || !pixels || width == 0 || height == 0) return nullptr;
        Entry& entry = _cache[key];
        if (entry.tex.valid()) {
            if (_invalidate_cb && entry.tex.image.view != VK_NULL_HANDLE)
                _invalidate_cb(entry.tex.image.view);
            _uploader.destroy(entry.tex);
        }
        entry.resolved_path.clear(); // Excludes this entry from file polling.
        entry.img_mtime = {};
        entry.meta_mtime = {};
        entry.has_meta_mtime = false;
        entry.settings = TextureImportSettings{};
        entry.settings.filter_mode = filter;
        entry.settings.wrap_mode = wrap;
        entry.settings.srgb = srgb;
        entry.tex = _uploader.upload(pixels, width, height,
                                     filter == FilterMode::Bilinear ? vkr::FilterMode::Bilinear : vkr::FilterMode::Point,
                                     false,
                                     wrap == WrapMode::Repeat ? vkr::WrapMode::Repeat : vkr::WrapMode::Clamp,
                                     srgb);
        return entry.tex.valid() ? &entry.tex : nullptr;
    }

    // Decodes animated GIFs once and uploads only the frame selected by the
    // VideoPlayer2D clock.  GIF is intentionally handled in the core runtime
    // rather than through a platform codec so exported games retain working
    // playback without an extra redistributable.
    vkr::Texture* get_animated_gif_frame(const std::string& image_path, float seconds, bool loop) {
        Entry* entry = _get_entry(image_path);
        if (!entry || entry->gif_frames <= 1 || entry->gif_pixels.empty())
            return entry && entry->tex.valid() ? &entry->tex : nullptr;

        int total_ms = 0;
        for (int delay : entry->gif_delays) total_ms += std::max(10, delay);
        if (total_ms <= 0) total_ms = entry->gif_frames * 100;
        int clock_ms = std::max(0, (int)std::floor(seconds * 1000.f));
        if (loop) clock_ms %= total_ms;
        else clock_ms = std::min(clock_ms, total_ms - 1);
        int frame = 0;
        for (; frame + 1 < entry->gif_frames; ++frame) {
            clock_ms -= std::max(10, entry->gif_delays[(size_t)frame]);
            if (clock_ms < 0) break;
        }
        if (entry->gif_current_frame == frame && entry->tex.valid()) return &entry->tex;

        if (entry->tex.valid()) {
            if (_invalidate_cb && entry->tex.image.view != VK_NULL_HANDLE)
                _invalidate_cb(entry->tex.image.view);
            _uploader.destroy(entry->tex);
        }
        const size_t frame_bytes = (size_t)entry->gif_width * (size_t)entry->gif_height * 4u;
        const uint8_t* frame_pixels = entry->gif_pixels.data() + frame_bytes * (size_t)frame;
        entry->tex = _uploader.upload(frame_pixels, (uint32_t)entry->gif_width, (uint32_t)entry->gif_height,
                                      entry->settings.filter_mode == FilterMode::Bilinear ? vkr::FilterMode::Bilinear : vkr::FilterMode::Point,
                                      false,
                                      entry->settings.wrap_mode == WrapMode::Repeat ? vkr::WrapMode::Repeat : vkr::WrapMode::Clamp,
                                      entry->settings.srgb);
        entry->gif_current_frame = frame;
        return entry->tex.valid() ? &entry->tex : nullptr;
    }

    // Resolves "path" or "path:sprite_name" into a ResolvedSprite, same
    // precedence as the SDL2 version: instance overrides (handled by the
    // caller) beat the sprite's own pivot override, which beats the
    // texture's import-setting default pivot.
    ResolvedSprite get_sprite(const std::string& ref) {
        ResolvedSprite out;
        if (ref.empty()) return out;

        std::string image_path = ref;
        std::string sprite_name;
        auto colon = ref.find(':');
        if (colon != std::string::npos && colon > 1) {
            image_path = ref.substr(0, colon);
            sprite_name = ref.substr(colon + 1);
        }

        auto* entry = _get_entry(image_path);
        if (!entry || !entry->tex.valid()) return out;

        out.texture         = &entry->tex;
        out.pivot_x          = entry->settings.default_pivot_x;
        out.pivot_y          = entry->settings.default_pivot_y;
        out.pixels_per_unit  = entry->settings.pixels_per_unit;
        out.filter_mode      = entry->settings.filter_mode;
        out.wrap_mode        = entry->settings.wrap_mode;

        if (!sprite_name.empty()) {
            if (const SpriteRect* sp = entry->settings.find_sprite(sprite_name)) {
                out.src = SDL_Rect{sp->x, sp->y, sp->w, sp->h};
                out.has_src = (sp->w > 0 && sp->h > 0);
                if (sp->has_pivot_override) { out.pivot_x = sp->pivot_x; out.pivot_y = sp->pivot_y; }
            }
        }
        return out;
    }

    std::vector<std::string> list_sprites(const std::string& image_path) {
        std::vector<std::string> names;
        auto* entry = _get_entry(image_path);
        if (!entry) return names;
        for (auto& s : entry->settings.sprites) names.push_back(s.name);
        return names;
    }

    void get_size(const std::string& image_path, int& w, int& h) {
        w = 0; h = 0;
        auto* entry = _get_entry(image_path);
        if (entry && entry->tex.valid()) { w = (int)entry->tex.width; h = (int)entry->tex.height; }
    }

    // Call once per frame (or on editor file-save events) to pick up
    // externally-edited pixels/.meta without restarting.
    void poll_hot_reload() {
        namespace fs = std::filesystem;
        for (auto& [key, entry] : _cache) {
            if (entry.resolved_path.empty()) continue;
            std::error_code ec;
            auto img_mtime = fs::last_write_time(entry.resolved_path, ec);
            if (ec) continue;
            std::string meta_path = entry.resolved_path + ".meta";
            bool meta_exists = fs::exists(meta_path, ec);
            auto meta_mtime = meta_exists ? fs::last_write_time(meta_path, ec) : fs::file_time_type{};

            bool need_pixel_reload = (img_mtime != entry.img_mtime);
            bool need_meta_reload  = (meta_exists && (!entry.has_meta_mtime || meta_mtime != entry.meta_mtime))
                                      || (!meta_exists && entry.has_meta_mtime);
            if (need_pixel_reload || need_meta_reload)
                _reload_entry(entry, need_pixel_reload, need_meta_reload);
        }
    }

    void clear() {
        for (auto& [k, e] : _cache) if (e.tex.valid()) _uploader.destroy(e.tex);
        _cache.clear();
    }

private:
    struct Entry {
        vkr::Texture tex;
        std::string  resolved_path;
        std::filesystem::file_time_type img_mtime{};
        std::filesystem::file_time_type meta_mtime{};
        bool has_meta_mtime = false;
        TextureImportSettings settings;
        int gif_width = 0, gif_height = 0, gif_frames = 0, gif_current_frame = -1;
        std::vector<uint8_t> gif_pixels;
        std::vector<int> gif_delays;
    };

    vkr::Context&        _ctx;
    vkr::TextureUploader  _uploader;
    std::string           _base_dir;
    InvalidateCallback    _invalidate_cb;
    std::unordered_map<std::string, Entry> _cache;

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

    Entry* _get_entry(const std::string& image_path) {
        namespace fs = std::filesystem;
        auto it = _cache.find(image_path);
        if (it != _cache.end()) return &it->second;

        std::string resolved = _resolve(image_path);
        if (resolved.empty()) {
            std::cerr << "[TextureCache] Cannot resolve: " << image_path << "\n";
            Entry blank;
            blank.resolved_path = image_path;
            auto [ins, _] = _cache.emplace(image_path, std::move(blank));
            return &ins->second;
        }

        Entry entry;
        entry.resolved_path = resolved;
        std::error_code ec;
        entry.img_mtime = fs::last_write_time(resolved, ec);
        _load_meta(entry);
        // Import settings control the sampler/colour-space used by the first
        // upload as well as hot reloads. Loading metadata first fixes the
        // long-standing first-open mismatch for point/bilinear textures.
        _load_pixels(entry);

        auto [ins, ok] = _cache.emplace(image_path, std::move(entry));
        return &ins->second;
    }

    void _load_pixels(Entry& entry) {
        // stbi_load with desired_channels=4 decodes straight to tightly
        // packed RGBA8 — no intermediate SDL_Surface, no separate
        // SDL_ConvertSurfaceFormat step (stb_image does the channel
        // expansion/conversion internally), which is what
        // vkr::TextureUploader::upload() wants as-is.
        int w = 0, h = 0, src_channels = 0;
        std::string extension = std::filesystem::path(entry.resolved_path).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (extension == ".gif") {
            std::ifstream gif_file(entry.resolved_path, std::ios::binary);
            const std::vector<stbi_uc> bytes((std::istreambuf_iterator<char>(gif_file)), std::istreambuf_iterator<char>());
            int* delays = nullptr, frames = 0;
            stbi_uc* gif = !bytes.empty() ? stbi_load_gif_from_memory(bytes.data(), (int)bytes.size(), &delays,
                                                                        &w, &h, &frames, &src_channels, 4) : nullptr;
            if (gif && frames > 0 && w > 0 && h > 0) {
                const size_t frame_bytes = (size_t)w * (size_t)h * 4u;
                entry.gif_width = w; entry.gif_height = h; entry.gif_frames = frames; entry.gif_current_frame = 0;
                entry.gif_pixels.assign(gif, gif + frame_bytes * (size_t)frames);
                entry.gif_delays.resize((size_t)frames, 100);
                for (int index = 0; delays && index < frames; ++index) entry.gif_delays[(size_t)index] = std::max(10, delays[index]);
                stbi_image_free(gif);
                if (delays) stbi_image_free(delays);
                vkr::FilterMode vk_filter = (entry.settings.filter_mode == FilterMode::Bilinear)
                    ? vkr::FilterMode::Bilinear : vkr::FilterMode::Point;
                vkr::WrapMode vk_wrap = (entry.settings.wrap_mode == WrapMode::Repeat)
                    ? vkr::WrapMode::Repeat : vkr::WrapMode::Clamp;
                entry.tex = _uploader.upload(entry.gif_pixels.data(), (uint32_t)w, (uint32_t)h, vk_filter,
                                              entry.settings.generate_mipmaps, vk_wrap, entry.settings.srgb);
                return;
            }
            if (gif) stbi_image_free(gif);
            if (delays) stbi_image_free(delays);
        }
        stbi_uc* pixels = stbi_load(entry.resolved_path.c_str(), &w, &h, &src_channels, 4);
        if (!pixels) {
            std::cerr << "[TextureCache] Cannot load: " << entry.resolved_path
                       << " (" << stbi_failure_reason() << ")\n";
            return;
        }

        vkr::FilterMode vk_filter = (entry.settings.filter_mode == FilterMode::Bilinear)
            ? vkr::FilterMode::Bilinear : vkr::FilterMode::Point;
        vkr::WrapMode vk_wrap = (entry.settings.wrap_mode == WrapMode::Repeat)
            ? vkr::WrapMode::Repeat : vkr::WrapMode::Clamp;

        entry.tex = _uploader.upload(static_cast<const uint8_t*>(pixels),
                                      (uint32_t)w, (uint32_t)h, vk_filter,
                                      entry.settings.generate_mipmaps,
                                      vk_wrap, entry.settings.srgb);
        stbi_image_free(pixels);
    }

    void _load_meta(Entry& entry) {
        namespace fs = std::filesystem;
        std::string meta_path = entry.resolved_path + ".meta";
        std::error_code ec;
        if (fs::exists(meta_path, ec)) {
            std::ifstream f(meta_path);
            nlohmann::json raw;
            bool ok = false;
            if (f) { try { f >> raw; ok = true; } catch (...) { ok = false; } }
            entry.settings = ok ? TextureImportSettings::from_json(raw) : TextureImportSettings{};
            entry.meta_mtime = fs::last_write_time(meta_path, ec);
            entry.has_meta_mtime = true;
        } else {
            entry.settings = TextureImportSettings{};
            entry.has_meta_mtime = false;
        }
    }

    void _reload_entry(Entry& entry, bool reload_pixels, bool reload_meta) {
        if (reload_pixels) {
            // Notify the SpriteBatch's descriptor cache before the old
            // VkImageView is destroyed — otherwise a cached descriptor set
            // could point at a freed view (GPU use-after-free).
            if (_invalidate_cb && entry.tex.image.view != VK_NULL_HANDLE)
                _invalidate_cb(entry.tex.image.view);
            if (entry.tex.valid()) _uploader.destroy(entry.tex);

            std::error_code ec;
            entry.img_mtime = std::filesystem::last_write_time(entry.resolved_path, ec);
            entry.gif_width = entry.gif_height = entry.gif_frames = 0;
            entry.gif_current_frame = -1;
            entry.gif_pixels.clear();
            entry.gif_delays.clear();
            _load_pixels(entry);
        }
        if (reload_meta) {
            _load_meta(entry);
            // Filter mode may have changed — re-create the sampler in place
            // (pixels/image stay valid; only the immutable sampler swaps).
            if (entry.tex.valid()) {
                vkr::FilterMode vk_filter = (entry.settings.filter_mode == FilterMode::Bilinear)
                    ? vkr::FilterMode::Bilinear : vkr::FilterMode::Point;
                vkr::WrapMode vk_wrap = (entry.settings.wrap_mode == WrapMode::Repeat)
                    ? vkr::WrapMode::Repeat : vkr::WrapMode::Clamp;
                if (entry.tex.sampler != VK_NULL_HANDLE)
                    vkDestroySampler(_ctx.device, entry.tex.sampler, nullptr);
                entry.tex.sampler = _uploader.create_sampler(vk_filter, 1, vk_wrap);
            }
        }
        std::cout << "[TextureCache] Hot-reloaded: " << entry.resolved_path << "\n";
    }
};

} // namespace texture
#endif // ENGINE_TEXTURE_SYSTEM_DEFINED
