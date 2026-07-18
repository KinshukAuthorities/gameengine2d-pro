#pragma once
/*
 * render_system_vk.hpp — Vulkan port of render_system.hpp.
 *
 * Identical public API to RenderSystem (SDL2 version):
 *   clear(), present(), draw(), draw_ui(), draw_parallax(),
 *   draw_nineslice(), render_to_bytes(), set_sorting_layers(), ...
 *
 * Internal SDL2 calls replaced with QuadCommand + SpriteBatch::push_quad.
 * SDL2 stays for window + input only; SDL_Renderer is gone entirely.
 *
 * Key mappings:
 *   SDL_RenderCopyEx(...)         -> push_quad (simple/tiled/sliced)
 *   SDL_RenderFillRect(...)       -> push_quad (use_texture=0)
 *   SDL_RenderDrawLine(...)       -> push_quad (thin unit-wide quad, use_texture=0)
 *   SDL_SetRenderTarget(tex)      -> RenderTarget::begin()
 *   SDL_SetRenderTarget(nullptr)  -> RenderTarget::end()
 *   SDL_RenderReadPixels(...)     -> RenderTarget readback path
 *   SDL_SetTextureScaleMode(...)  -> Texture::sampler swap (vkr::FilterMode)
 *   SDL_RenderSetClipRect(...)    -> QuadCommand::scissor (VkRect2D)
 *   SDL_BLENDMODE_BLEND           -> BlendMode::Blend
 *   SDL_BLENDMODE_ADD             -> BlendMode::Additive
 *
 * This file is header-only (large, but same as the old render_system.hpp).
 * draw_ui() lives in render_system_vk.cpp (same split as the SDL2 version).
 */

#include "entity.hpp"
#include "camera.hpp"
#include "transform_system.hpp"
#include "tile_palette.hpp"
#include "vk_render/texture_system_vk.hpp"
#include "material_system.hpp"
#include "vk_render/vk_renderer_backend.hpp"
#include "vk_render/vk_render_target.hpp"
#include "vk_render/vk_sprite_batch.hpp"
#include "vk_render/vk_font_atlas.hpp"
#include "vk_render/vk_particle_compute.hpp"
// Needed for ParticleSystem::eval_size()/eval_color() (task 12 curve
// evaluators), shared between the CPU and GPU particle render paths so
// curves look identical regardless of which path an emitter is on. No
// circular include: systems.hpp does not include this file.
#include "systems.hpp"
// Global script debug primitives share the engine's Debug state. Rendering
// them here makes Debug::draw_line work in standalone and editor play mode.
#include "script_system.hpp"

#include <SDL2/SDL.h>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <utility>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <fstream>
#include <array>
#include <filesystem>

#ifdef min
#  undef min
#endif
#ifdef max
#  undef max
#endif

// Image loading (pixel decoding) is handled by stb_image.h — no init/quit
// lifecycle needed (stbi_load/stbi_image_free are self-contained per call),
// unlike SDL_image's IMG_Init/IMG_Quit or the old WIC CoInitializeEx dance.
// Kept as no-op macros so the constructor/destructor call sites below don't
// need to change.
#define ENGINE_IMG_INIT() 0
#define ENGINE_IMG_QUIT() ((void)0)

using texture::TextureCache;
using texture::ResolvedSprite;

// ─── RenderSystem ─────────────────────────────────────────────────────────────
class RenderSystem {
public:
    // ── Primary constructor: owns everything, creates from SDL window ─────────
    // window:      SDL window (Vulkan surface created from it)
    // cam:         camera reference (same as SDL2 version)
    // shader_dir:  directory containing sprite.vert.spv / sprite.frag.spv
    RenderSystem(SDL_Window* window, Camera& cam,
                 const std::string& shader_dir = "shaders")
        : _cam(cam), _owns_backend(true)
    {
        ENGINE_IMG_INIT();
        SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0"); // hint still read by SDL window mgr

        _backend = std::make_unique<vkr::RendererBackend>(window, shader_dir, false);
        _textures = std::make_unique<TextureCache>(_backend->ctx());
        _materials = std::make_unique<material::MaterialCache>();

        // Hook texture hot-reload to invalidate descriptor sets for old views
        _textures->set_invalidate_callback([this](VkImageView v) {
            _backend->batch().descriptors().invalidate(v);
        });

        // NOTE: no default font is loaded here. UIText/UIButton's font atlas
        // must be loaded explicitly via load_default_font(absolute_path) by
        // the caller (main.cpp / editor_main.cpp), the same way shader_dir
        // is resolved relative to the executable rather than baked in as a
        // bare relative path here — see those files' get_executable_dir()
        // usage. Until load_default_font() is called, UIText/UIButton
        // automatically fall back to the built-in pixel font.

        std::cout << "[RenderSystem] Vulkan backend ready\n";
    }

    // ── Offscreen-only constructor: for editor/viewport renderer ─────────────
    // Takes an existing RendererBackend (no window ownership here).
    RenderSystem(vkr::RendererBackend& backend, Camera& cam)
        : _cam(cam), _owns_backend(false), _external_backend(&backend)
    {
        _textures  = std::make_unique<TextureCache>(backend.ctx());
        _materials = std::make_unique<material::MaterialCache>();
        _textures->set_invalidate_callback([&backend](VkImageView v) {
            backend.batch().descriptors().invalidate(v);
        });
        // See primary constructor's comment: caller loads the font
        // explicitly via load_default_font(absolute_path).
    }

    ~RenderSystem() {
        if (_owns_backend && _backend) _backend->wait_idle();
        _font.destroy(_get_backend().ctx());
        for (auto& [_, entry] : _project_fonts) entry.atlas.destroy(_get_backend().ctx());
        _textures.reset();
        if (_owns_backend) ENGINE_IMG_QUIT();
    }

    RenderSystem(const RenderSystem&) = delete;
    RenderSystem& operator=(const RenderSystem&) = delete;

    // ── Public helpers ────────────────────────────────────────────────────────

    struct GlobalLightingSettings {
        bool enabled = true;
        float ambient_intensity = .15f;
        std::array<float, 3> ambient_color{1.f, 1.f, 1.f};
        int max_lights = 16;
        float shadow_strength = 1.f;
    };

    void set_global_lighting_settings(GlobalLightingSettings settings) {
        settings.ambient_intensity = std::clamp(settings.ambient_intensity, 0.f, 4.f);
        for (float& channel : settings.ambient_color) channel = std::clamp(channel, 0.f, 1.f);
        settings.max_lights = std::clamp(settings.max_lights, 0, vkr::kMaxLights);
        settings.shadow_strength = std::clamp(settings.shadow_strength, 0.f, 1.f);
        _global_lighting = settings;
    }

    const GlobalLightingSettings& global_lighting_settings() const { return _global_lighting; }

    void set_asset_dir(const std::string& dir) {
        _asset_dir = dir;
        _textures->set_base_dir(dir);
        _materials->set_base_dir(dir);
        _tile_palette_cache.clear();
        _load_global_lighting_settings();
    }

    material::MaterialCache& materials() { return *_materials; }

    // ── Perf stats (editor diagnostics) ──────────────────────────────────────
    // Culling counters: filled in by draw()'s frustum-culling loop each
    // frame. Purely diagnostic (read-only for callers) — lets the editor
    // print "N/M visible, K culled" to the Console while Playing. Not used
    // for any rendering decision, so it's safe to read mid-frame or stale.
    struct CullStats {
        uint32_t total_considered = 0; // entities the culling loop evaluated this frame
        uint32_t culled           = 0; // of those, how many were skipped (outside camera view)
        uint32_t visible          = 0; // total_considered - culled
    };
    const CullStats& cull_stats() const { return _cull_stats; }

    // GPU draw-call / instancing counters straight from SpriteBatch::flush()
    // this frame — see vk_sprite_batch.hpp's FrameStats for field meanings.
    const vkr::FrameStats& frame_stats() { return _get_backend().batch().frame_stats(); }

    // Replacement for the SDL2 build's SDL_GetRendererOutputSize(renderer, &w, &h):
    // core.cpp reads this once per frame for Screen::Set(...) and the UI mouse
    // hit-testing extent. Returns the offscreen target's size during
    // render_to_bytes(), otherwise the live swapchain extent.
    VkExtent2D current_extent() { return _cmd_extent(); }

    // Expose the Vulkan context for systems that need direct GPU access
    // (e.g. GpuParticleCompute). Only valid while the backend is alive.
    vkr::Context& vk_ctx() { return _get_backend().ctx(); }
    VkCommandBuffer current_cmd() const { return _cmd; }

    // Register the per-entity GPU particle emitter map so _draw_particles()
    // can read live particle data from the SSBO instead of the (empty) CPU array.
    // Call once after the map is constructed in core.cpp.
    void set_gpu_emitters(
        std::unordered_map<int, std::unique_ptr<vkr::GpuParticleCompute>>* emitters)
    {
        _gpu_emitters = emitters;
    }

    // For code that previously checked renderer != nullptr
    bool valid() const { return _owns_backend ? (_backend != nullptr) : (_external_backend != nullptr); }

    // ── Frame control (primary path) ──────────────────────────────────────────

    // begin_frame() stores the command buffer for this frame. Call before
    // draw() / draw_parallax() / draw_ui(). The SDL2 version had no
    // equivalent — clear() implicitly started the frame.
    // Returns false if the window is minimized (caller should skip draw).
    bool begin_frame(SDL_Color bg = {30, 30, 30, 255}) {
        _textures->poll_hot_reload();
        auto& be = _get_backend();
        float r = bg.r / 255.f, g = bg.g / 255.f, b = bg.b / 255.f, a = bg.a / 255.f;
        _cmd = be.begin_frame(r, g, b, a);
        return _cmd != VK_NULL_HANDLE;
    }

    // Compatibility shim: the old API had clear() + present() as the frame
    // markers. We keep them but they now forward into begin/end_frame.
    void clear(SDL_Color col = {30, 30, 30, 255}) {
        if (!_frame_begun) {
            _frame_begun = begin_frame(col);
        }
    }

    void present() {
        if (_frame_begun) {
            _get_backend().end_frame(_cmd);
            _cmd = VK_NULL_HANDLE;
            _frame_begun = false;
        }
    }

    // ── Offscreen render → raw RGBA bytes (for editor viewport) ──────────────
    std::vector<uint8_t> render_to_bytes(EntityList& entities, float alpha,
                                          int w, int h,
                                          SDL_Color bg = {35, 35, 40, 255}) {
        auto& be = _get_backend();
        be.wait_idle();

        // Lazily create / resize the offscreen target
        if (!_offscreen || (int)_offscreen->width() != w || (int)_offscreen->height() != h) {
            _offscreen = std::make_unique<vkr::RenderTarget>();
            _offscreen->create(be.ctx(), (uint32_t)w, (uint32_t)h);
        }

        // Save camera dimensions
        int prev_cw = _cam.width, prev_ch = _cam.height;
        _cam.width = w; _cam.height = h;

        // Build a one-shot command buffer for the whole frame
        VkCommandBuffer cmd = be.begin_one_shot();

        float r = bg.r / 255.f, g = bg.g / 255.f, b = bg.b / 255.f, a = bg.a / 255.f;
        _offscreen->begin(cmd, r, g, b, a);

        // We need a SpriteBatch for this offscreen pass. Reuse the backend's
        // batch using frame slot 0 (we're not in a normal frame).
        be.batch().begin_frame(0, {(uint32_t)w, (uint32_t)h});

        // Make current_extent() and the light camera UBO use the offscreen
        // target size instead of the swapchain size.
        _target_extent_active = true;
        _target_extent = {(uint32_t)w, (uint32_t)h};

        _cmd = cmd;
        draw_parallax(entities);
        draw(entities, alpha);

        // Flush the batch into the offscreen render pass
        be.batch().flush(cmd);
        _offscreen->end(cmd);

        // Readback
        _offscreen->transition_for_readback(cmd);
        _offscreen->copy_to_readback_buffer(cmd);
        _offscreen->transition_after_readback(cmd);

        be.end_one_shot(cmd);
        _cmd = VK_NULL_HANDLE;
        _target_extent_active = false;

        _cam.width = prev_cw; _cam.height = prev_ch;
        return _offscreen->read_pixels();
    }

    // ── Offscreen render → externally-owned RenderTarget, no readback ────────
    // Same idea as render_to_bytes(), but for callers (the editor's
    // ViewportPanel) that already own a persistent vkr::RenderTarget they
    // sample directly as an ImGui texture — no CPU readback needed, and the
    // target's lifetime/resize is the caller's responsibility, not ours.
    //
    // Usage (mirrors render_to_bytes()'s body, just split into steps so the
    // caller can interleave its own non-RenderSystem drawing — e.g. an ImGui
    // draw-list grid baked into the same texture isn't supported this way
    // anymore; see ViewportPanel's gizmo overlay, which draws after this
    // instead):
    //   VkCommandBuffer cmd = rs.begin_render_to_target(target, w, h, bg);
    //   rs.draw_parallax(entities);
    //   rs.draw(entities);
    //   rs.draw_ui(entities, ...);
    //   rs.end_render_to_target(target, cmd);
    //
    // Like render_to_bytes(), this uses a one-shot command buffer
    // (begin_one_shot/end_one_shot), which waits on the graphics queue
    // before returning — simple and correct, but a real GPU stall once per
    // editor frame. Fine for now; if viewport framerate ever becomes a
    // bottleneck with complex scenes, this is the first place to look at
    // moving onto a per-frame-slot command buffer instead.
    VkCommandBuffer begin_render_to_target(vkr::RenderTarget& target, int w, int h,
                                            SDL_Color bg = {35, 35, 40, 255}) {
        auto& be = _get_backend();

        VkCommandBuffer cmd = be.begin_one_shot();
        float r = bg.r / 255.f, g = bg.g / 255.f, b = bg.b / 255.f, a = bg.a / 255.f;
        target.begin(cmd, r, g, b, a);
        be.batch().begin_frame(0, {(uint32_t)w, (uint32_t)h});

        _target_extent_active = true;
        _target_extent = {(uint32_t)w, (uint32_t)h};

        _cmd = cmd;
        return cmd;
    }

    void end_render_to_target(vkr::RenderTarget& target, VkCommandBuffer cmd) {
        auto& be = _get_backend();
        be.batch().flush(cmd);
        target.end(cmd);
        be.end_one_shot(cmd);
        _cmd = VK_NULL_HANDLE;
        _target_extent_active = false;
    }


    // ── Sorting layers ────────────────────────────────────────────────────────
    void set_sorting_layers(const std::vector<std::string>& names) { _sorting_layers = names; }
    const std::vector<std::string>& sorting_layers() const { return _sorting_layers; }
    int sorting_layer_index(const std::string& name) const {
        if (name.empty()) return 0;
        for (size_t i = 0; i < _sorting_layers.size(); ++i)
            if (_sorting_layers[i] == name) return (int)i;
        return 0;
    }

    // ── draw() ────────────────────────────────────────────────────────────────
    // Cheap peek at the texture/tileset name an entity will draw with, used
    // only to group draw calls by texture (see draw()'s secondary sort
    // below) — independent from RenderSystem's own MaterialCache since this
    // just reads the raw JSON field, no texture upload/lookup happens here.
    // Returns "" for entity kinds with no single texture (lights, particles
    // use per-particle textures so aren't worth sorting) or untextured
    // sprites — those entities simply keep their original relative order
    // (std::stable_sort), which is always safe.
    std::string _peek_texture_name(Entity& e) {
        if (has_component(e, "SpriteRenderer")) {
            auto& sr = e["components"]["SpriteRenderer"];
            std::string mp = sr.value("material", std::string(""));
            if (!mp.empty()) {
                auto mat = _materials->get(mp);
                if (mat && !mat->texture.empty()) return mat->texture;
            }
            return sr.value("texture", std::string(""));
        }
        if (has_component(e, "Tilemap")) {
            auto& tm = e["components"]["Tilemap"];
            const std::string palette_ref = tm.value("tile_palette", std::string(""));
            if (!palette_ref.empty()) {
                if (const auto* palette = _tile_palette_cache.get(_asset_dir, palette_ref))
                    return palette->atlas;
            }
            return tm.value("tileset", std::string(""));
        }
        if (has_component(e, "LineRenderer2D")) {
            return e["components"]["LineRenderer2D"].value("texture", std::string(""));
        }
        if (has_component(e, "TrailRenderer2D")) {
            return e["components"]["TrailRenderer2D"].value("texture", std::string(""));
        }
        return "";
    }

    void draw(EntityList& entities, float alpha = 1.f) {
        if (!_cmd) return;
        _current_layer = 1; // world sprites between parallax and UI

        // ── Light2D pre-pass (task 6) ─────────────────────────────────────────
        // Collect all active Light2D components into the LightUBOManager
        // BEFORE recording any sprite draw commands, so sprite_lit.frag can
        // read the populated UBO during flush().
        {
            auto& be = _get_backend();
            auto& lmgr = be.batch().lights();
            lmgr.begin_frame();
            lmgr.set_ambient(_global_lighting.ambient_color[0], _global_lighting.ambient_color[1],
                             _global_lighting.ambient_color[2], _global_lighting.ambient_intensity);
            if (_global_lighting.enabled) {
                for (auto& e : entities) {
                    if (lmgr.light_count() >= _global_lighting.max_lights) break;
                    if (!entity_active(e)) continue;
                    if (!has_component(e, "Light2D") || !has_component(e, "Transform")) continue;
                    auto& light = e["components"]["Light2D"];
                    if (!light.value("enabled", true)) continue;
                    float radius    = light.value("radius", 200.f);
                    float intensity = light.value("intensity", 1.f);
                    auto lcol = light.value("color", std::vector<int>{255, 230, 150, 255});
                    float r = (lcol.size() > 0 ? lcol[0] : 255) / 255.f;
                    float g = (lcol.size() > 1 ? lcol[1] : 230) / 255.f;
                    float b = (lcol.size() > 2 ? lcol[2] : 150) / 255.f;
                    auto wt = transform::cached_world(e);
                    // radius is stored in screen-pixels (at zoom=1). frag_world_pos
                    // is in world units where 1 world unit = _cam.zoom screen pixels,
                    // so divide by zoom to get the correct world-space falloff radius.
                    float radius_world = radius / std::max(_cam.zoom, 0.001f);
                    lmgr.push_light(wt.x, wt.y, radius_world, intensity, r, g, b);
                }
            }
            // Camera data for world-pos reconstruction in sprite_lit.vert
            VkExtent2D ext = _cmd_extent();
            float ppu = _cam.zoom; // pixels per world unit (assumes 1 unit = 1 pixel at zoom 1)
            lmgr.set_camera(_cam.x, _cam.y, ppu, (float)ext.width, (float)ext.height);

            // Match the UBO upload slot to the SpriteBatch frame slot. This is
            // 0 for one-shot/offscreen passes and the swapchain frame index for
            // normal on-screen rendering.
            lmgr.upload(be.batch().current_frame_slot());
        }

        std::vector<Entity*> sorted;
        sorted.reserve(entities.size());
        for (auto& e : entities)
            if (entity_active(e)) sorted.push_back(&e);

        std::stable_sort(sorted.begin(), sorted.end(), [this](Entity* a, Entity* b) {
            SortKey ka = _sort_key(*a), kb = _sort_key(*b);
            if (ka.sorting_layer != kb.sorting_layer) return ka.sorting_layer < kb.sorting_layer;
            if (ka.legacy_layer  != kb.legacy_layer)  return ka.legacy_layer  < kb.legacy_layer;
            return ka.order_in_layer < kb.order_in_layer;
        });

        // Secondary, SAFE texture-grouping pass: entities sharing the exact
        // same (sorting_layer, legacy_layer, order_in_layer) tuple have no
        // guaranteed relative order to begin with — std::stable_sort above
        // only promises ties keep their original container order, not that
        // it's meaningful — so regrouping just within each same-key run by
        // texture is normally free: it can't change anything the design
        // ever actually depended on, while letting SpriteBatch::flush()
        // merge many same-texture quads into one draw call instead of
        // breaking batch every time draw order happens to interleave
        // textures.
        //
        // ONE exception: SpriteMask entities set global scissor state
        // (_active_scissor/_mask_active, see _draw_sprite_mask) that
        // subsequent _draw_sprite() calls read for "visible_inside_mask" —
        // so a mask's position relative to the sprites after it IS
        // load-bearing even within a same-sort-key run. Any run containing
        // a SpriteMask is left completely untouched (original stable order)
        // rather than risk separating a mask from what it's meant to clip.
        //
        // Entities at DIFFERENT sort keys are never reordered relative to
        // each other either way — that ordering is load-bearing for visual
        // layering and is left exactly as the stable_sort above produced it.
        {
            size_t run_start = 0;
            auto same_key = [this](Entity* a, Entity* b) {
                SortKey ka = _sort_key(*a), kb = _sort_key(*b);
                return ka.sorting_layer == kb.sorting_layer &&
                       ka.legacy_layer  == kb.legacy_layer  &&
                       ka.order_in_layer == kb.order_in_layer;
            };
            while (run_start < sorted.size()) {
                size_t run_end = run_start + 1;
                while (run_end < sorted.size() && same_key(sorted[run_start], sorted[run_end]))
                    ++run_end;

                bool has_mask = false;
                for (size_t i = run_start; i < run_end; ++i)
                    if (has_component(*sorted[i], "SpriteMask")) { has_mask = true; break; }

                if (!has_mask && run_end - run_start > 1) {
                    std::stable_sort(sorted.begin() + run_start, sorted.begin() + run_end,
                        [this](Entity* a, Entity* b) {
                            return _peek_texture_name(*a) < _peek_texture_name(*b);
                        });
                }
                run_start = run_end;
            }
        }

        // Pre-compute visible world rect once — used for frustum culling below.
        // The 256-px margin avoids pop-in for large sprites whose pivot is
        // at the edge of the screen.
        const Camera::WorldBounds _vis = _cam.visible_world_bounds(256.f);

        _cull_stats = CullStats{}; // reset this frame's counters before the loop below

        for (auto* ep : sorted) {
            auto& e = *ep;
            // ── Frustum culling ───────────────────────────────────────────────
            // Skip entities whose world-space bounds don't intersect the
            // camera's visible rect. A small margin avoids pop-in at edges
            // (e.g. a sprite whose pivot is centred may extend half its size
            // beyond the transform position). Light2D entities are never
            // culled here — their visual radius extends well past the
            // transform origin and they're already trivially cheap to draw
            // (one quad). SpriteMask entities affect clip state and must
            // always run regardless of position. Tilemaps have their own
            // inner-loop culling in _draw_tilemap().
            if (!has_component(e, "SpriteMask") && !has_component(e, "Tilemap") &&
                has_component(e, "Transform")) {
                ++_cull_stats.total_considered;
                auto wt = transform::cached_world(e);
                // Estimate half-extent from SpriteRenderer size, or use a
                // conservative 512 px fallback for other component types.
                float half_w = 256.f, half_h = 256.f;
                if (has_component(e, "SpriteRenderer")) {
                    auto& sr = e["components"]["SpriteRenderer"];
                    // base sprite size in world units (scale already applied
                    // by transform::cached_world via scale_x/scale_y)
                    half_w = std::max(16.f, sr.value("width",  64.f) * wt.scale_x * 0.5f);
                    half_h = std::max(16.f, sr.value("height", 64.f) * wt.scale_y * 0.5f);
                    // Particle emitters attach to SpriteRenderer-less entities usually
                } else if (has_component(e, "Light2D")) {
                    float r = e["components"]["Light2D"].value("radius", 200.f);
                    half_w = half_h = r;
                } else if (has_component(e, "TextMeshPro2D")) {
                    const auto& text = e["components"]["TextMeshPro2D"];
                    half_w = std::max(16.f, text.value("bounds_w", 200.f) * std::abs(wt.scale_x) * .5f);
                    half_h = std::max(16.f, std::max(text.value("bounds_h", 0.f),
                        text.value("font_size", 24.f) * 2.f) * std::abs(wt.scale_y) * .5f);
                }
                float ex_min_x = wt.x - half_w, ex_max_x = wt.x + half_w;
                float ex_min_y = wt.y - half_h, ex_max_y = wt.y + half_h;
                if (ex_max_x < _vis.min_x || ex_min_x > _vis.max_x ||
                    ex_max_y < _vis.min_y || ex_min_y > _vis.max_y) {
                    ++_cull_stats.culled;
                    continue;
                }
                ++_cull_stats.visible;
            }
            if (has_component(e, "SpriteMask"))      _draw_sprite_mask(e);
            if (has_component(e, "Tilemap"))         _draw_tilemap(e);
            if (has_component(e, "SpriteRenderer") && has_component(e, "Transform"))
                                                     _draw_sprite(e, alpha);
            if (has_component(e, "Shadow2DCaster") && has_component(e, "Transform"))
                                                     _draw_shadow_caster(e, entities);
            if (has_component(e, "TextMeshPro2D") && has_component(e, "Transform"))
                                                     _draw_text_mesh(e);
            if (has_component(e, "LineRenderer2D")) _draw_line_renderer(e);
            if (has_component(e, "TrailRenderer2D")) _draw_trail_renderer(e);
            if (has_component(e, "ParticleEmitter")) _draw_particles(e);
            if (has_component(e, "Light2D") && has_component(e, "Transform"))
                                                     _draw_light(e);
        }

        _active_scissor = VkRect2D{{0,0},{0,0}};
        _mask_active = false;

        _draw_debug_lines(entities);
    }

    // ── draw_viewports() — multi-viewport rendering (task 11) ─────────────────
    // Renders the scene multiple times, once per ViewportDesc, each with its
    // own Camera and sub-region of the render target.  Designed for:
    //   - Split-screen (two side-by-side viewports with different cameras)
    //   - Minimap (a small top-right corner viewport with a zoomed-out camera)
    //   - Picture-in-picture (secondary viewport showing a fixed angle)
    //
    // Call this instead of draw() when you need more than one camera in a frame.
    // Each viewport gets its own Vulkan scissor + viewport rect so quads outside
    // that region are clipped by the GPU; the camera is swapped temporarily so
    // world_to_screen() produces the correct screen-space coordinates for that
    // sub-region.
    //
    // Usage example (split-screen, two cameras left/right):
    //   Camera cam_left(w/2, h), cam_right(w/2, h);
    //   // ... update cameras ...
    //   VkRect2D left_rect  = {{0,   0}, {(uint32_t)w/2, (uint32_t)h}};
    //   VkRect2D right_rect = {{w/2, 0}, {(uint32_t)w/2, (uint32_t)h}};
    //   rs.draw_viewports(entities, {
    //       { {0,   0, w/2, h}, &cam_left  },
    //       { {w/2, 0, w/2, h}, &cam_right },
    //   });
    //
    // Notes:
    //  - draw_parallax() / draw_ui() are not automatically called per viewport;
    //    call them yourself between begin_frame() and flush if needed.
    //  - Camera width/height are temporarily set to the viewport rect dimensions
    //    so world_to_screen() centres correctly within the sub-region.  They
    //    are restored after each viewport.
    //  - LightUBO is re-uploaded per viewport (each camera may see different
    //    lights). For performance with many lights + many viewports, consider
    //    uploading once into a shared UBO slot.
    void draw_viewports(EntityList& entities,
                        const std::vector<ViewportDesc>& viewports,
                        float alpha = 1.f) {
        if (!_cmd || viewports.empty()) return;

        // Save entire camera state before iterating viewports so it can be
        // fully restored after the last viewport draws.
        const float saved_x     = _cam.x,     saved_y    = _cam.y;
        const float saved_zoom  = _cam.zoom,   saved_ang  = _cam.angle;
        const int   saved_w     = _cam.width,  saved_h    = _cam.height;

        VkExtent2D full_extent = _cmd_extent();

        for (const auto& vp : viewports) {
            if (!vp.camera) continue;

            Camera& cam = *vp.camera;

            // Temporarily point _cam at this viewport's camera by swapping fields.
            // _cam is a reference — we can't rebind a reference, so we copy the
            // fields we need and restore them afterward.  This keeps all the
            // existing draw helpers (_draw_sprite, _draw_tilemap, etc.) working
            // unchanged since they access _cam directly.
            int vp_w = vp.rect.width  > 0 ? vp.rect.width  : (int)full_extent.width;
            int vp_h = vp.rect.height > 0 ? vp.rect.height : (int)full_extent.height;

            // Swap camera state into _cam
            _cam.x     = cam.x;     _cam.y     = cam.y;
            _cam.zoom  = cam.zoom;  _cam.angle  = cam.angle;
            _cam.width = vp_w;      _cam.height = vp_h;

            // Set Vulkan viewport and scissor to the sub-region
            float vh = (float)vp_h;
            // Negative-height viewport: Vulkan NDC +Y is down when viewport
            // height is negative (our convention throughout SpriteBatch).
            VkViewport vk_vp{
                (float)vp.rect.x,
                (float)vp.rect.y + vh,   // origin at bottom of the rect for -Y
                (float)vp_w,
                -vh,                      // negative height flips Y
                0.f, 1.f
            };
            VkRect2D vk_scissor{
                {(int32_t)vp.rect.x, (int32_t)vp.rect.y},
                {(uint32_t)vp_w, (uint32_t)vp_h}
            };
            vkCmdSetViewport(_cmd, 0, 1, &vk_vp);
            vkCmdSetScissor (_cmd, 0, 1, &vk_scissor);

            // Draw the scene for this viewport
            draw(entities, alpha);

            // Flush this viewport's batch before moving to the next one so
            // scissor state doesn't bleed across viewports.
            _get_backend().batch().flush(_cmd);

            // Restore _cam fields to pre-viewport state for the next iteration
            _cam.x = saved_x; _cam.y = saved_y; _cam.zoom = saved_zoom;
            _cam.angle = saved_ang;
            _cam.width = saved_w; _cam.height = saved_h;
        }

        // Restore full-target viewport/scissor
        float fh = (float)full_extent.height;
        VkViewport full_vp{0, fh, (float)full_extent.width, -fh, 0.f, 1.f};
        VkRect2D   full_sc{{0,0}, full_extent};
        vkCmdSetViewport(_cmd, 0, 1, &full_vp);
        vkCmdSetScissor (_cmd, 0, 1, &full_sc);
    }

    // draw_ui() is in render_system_vk.cpp
    void draw_ui(EntityList& entities, int mouse_x, int mouse_y,
                 bool mouse_down, bool mouse_just_down);

    // ── draw_parallax() ───────────────────────────────────────────────────────
    void draw_parallax(EntityList& entities) {
        if (!_cmd) return;
        _current_layer = 0; // parallax is always behind world sprites and UI
        VkExtent2D ext = _cmd_extent();
        int sw = (int)ext.width, sh = (int)ext.height;

        std::vector<Entity*> bgs;
        for (auto& e : entities)
            if (entity_active(e) && has_component(e, "ParallaxBackground"))
                bgs.push_back(&e);
        std::stable_sort(bgs.begin(), bgs.end(), [](Entity* a, Entity* b) {
            return (*a)["components"]["ParallaxBackground"].value("depth", -1000.f)
                 < (*b)["components"]["ParallaxBackground"].value("depth", -1000.f);
        });

        for (auto* ep : bgs) {
            auto& comp = (*ep)["components"]["ParallaxBackground"];
            float opacity = std::max(0.f, std::min(1.f, comp.value("opacity", 1.f)));
            if (opacity <= 0) continue;

            float scroll_x = comp.value("speed_x", 0.15f);
            float scroll_y = comp.value("speed_y", 0.f);
            float par_x = _cam.x * scroll_x;
            float par_y = _cam.y * scroll_y;

            std::string tex_name = comp.value("texture", std::string(""));
            auto resolved = tex_name.empty() ? ResolvedSprite{} : _textures->get_sprite(tex_name);
            vkr::Texture* tex = resolved.texture;

            auto color = comp.value("color", std::vector<int>{255,255,255,255});
            float cr=1,cg=1,cb=1,ca=1;
            if (color.size()>=4){cr=color[0]/255.f;cg=color[1]/255.f;cb=color[2]/255.f;ca=color[3]/255.f;}
            ca *= opacity;

            if (!tex) {
                _push_fill_rect(0, 0, sw, sh, {cr, cg, cb, ca}, vkr::BlendMode::Blend);
                continue;
            }

            float scale = std::max(0.001f, comp.value("scale", 1.f));
            float dw = tex->width  * scale;
            float dh = tex->height * scale;
            if (dw < 1) dw = 1; if (dh < 1) dh = 1;

            bool tile_x = comp.value("tiling_x", true);
            bool tile_y = comp.value("tiling_y", true);
            float ox = std::fmod(par_x, dw);
            float oy = std::fmod(par_y, dh);

            if (tile_x && tile_y) {
                for (float ty2 = -dh+oy; ty2 < sh; ty2+=dh)
                    for (float tx2 = -dw+ox; tx2 < sw; tx2+=dw)
                        _push_textured_rect(tex, 0.f,0.f,1.f,1.f, tx2,ty2,dw,dh,
                            {cr,cg,cb,ca}, vkr::BlendMode::Blend, -1.f, false, {});
            } else {
                _push_textured_rect(tex, 0.f,0.f,1.f,1.f, ox,oy,dw,dh,
                    {cr,cg,cb,ca}, vkr::BlendMode::Blend, -1.f, false, {});
            }
        }
    }

    // ── draw_nineslice() ──────────────────────────────────────────────────────
    // Used by draw_ui (UIPanel/UIImage with sliced draw mode).
    // dst is screen-space pixel rect. border_* are source-texture pixels.
    void draw_nineslice(vkr::Texture* tex, const SDL_Rect& dst,
                         int border_left, int border_right, int border_top, int border_bottom,
                         bool fill_center,
                         std::array<float,4> tint = {1,1,1,1},
                         bool flip_h = false) {
        if (!tex) return;
        int tw = (int)tex->width, th = (int)tex->height;
        if (tw == 0 || th == 0) return;

        int sl = std::min(border_left,  tw/2), sr_ = std::min(border_right, tw/2);
        int st = std::min(border_top,   th/2), sb  = std::min(border_bottom,th/2);
        int mid_src_w = std::max(0, tw - sl - sr_);
        int mid_src_h = std::max(0, th - st - sb);

        float bl = (float)sl, br = (float)sr_, bt = (float)st, bb = (float)sb;
        if (bl+br > dst.w && bl+br>0) { float s=dst.w/(bl+br); bl*=s; br*=s; }
        if (bt+bb > dst.h && bt+bb>0) { float s=dst.h/(bt+bb); bt*=s; bb*=s; }
        float mid_w = std::max(0.f, (float)dst.w - bl - br);
        float mid_h = std::max(0.f, (float)dst.h - bt - bb);

        float rw = (float)tw, rh = (float)th;
        // Helper: uv from source pixels
        auto uv = [&](int px, int py, int pw, int ph,
                      float& u0, float& v0, float& u1, float& v1) {
            u0 = px / rw; v0 = py / rh;
            u1 = (px+pw) / rw; v1 = (py+ph) / rh;
            if (flip_h) { float tmp=u0; u0=u1; u1=tmp; }
        };

        auto blit = [&](int ssx, int ssy, int ssw, int ssh,
                        float dx, float dy, float dw, float dh) {
            if (ssw<=0||ssh<=0||dw<=0||dh<=0) return;
            float u0,v0,u1,v1; uv(ssx,ssy,ssw,ssh,u0,v0,u1,v1);
            _push_textured_rect(tex,u0,v0,u1,v1,
                dst.x+dx,dst.y+dy,dw,dh, tint, vkr::BlendMode::Blend,-1.f,false,{});
        };

        blit(0,         0,          sl,  st,  0,          0,          bl,    bt);
        blit(sl+mid_src_w,0,        sr_, st,  bl+mid_w,   0,          br,    bt);
        blit(0,         st+mid_src_h,sl, sb,  0,          bt+mid_h,   bl,    bb);
        blit(sl+mid_src_w,st+mid_src_h,sr_,sb, bl+mid_w,  bt+mid_h,   br,    bb);
        blit(sl, 0,             mid_src_w, st,  bl, 0,         mid_w, bt);
        blit(sl, st+mid_src_h,  mid_src_w, sb,  bl, bt+mid_h,  mid_w, bb);
        blit(0,  st,            sl,  mid_src_h,  0,  bt,        bl,    mid_h);
        blit(sl+mid_src_w, st,  sr_, mid_src_h,  bl+mid_w, bt,  br,   mid_h);
        if (fill_center) blit(sl, st, mid_src_w, mid_src_h, bl, bt, mid_w, mid_h);
    }

    // Overload for callers that pass SDL_Texture* (nil-safe — for compat with
    // the old SDL2 API used in render_system_vk.cpp's draw_ui)
    void draw_nineslice_sdl(void* /*unused sdl_tex*/, const SDL_Rect& dst,
                             int bl, int br, int bt, int bb, bool fill,
                             vkr::Texture* tex, std::array<float,4> tint = {1,1,1,1}) {
        if (tex) draw_nineslice(tex, dst, bl, br, bt, bb, fill, tint);
    }

    // ── Low-level draw primitives (used by draw_ui) ───────────────────────────

    // Textured axis-aligned rect (UVs normalized 0..1)
    void push_textured_rect(vkr::Texture* tex,
                             float u0, float v0, float u1, float v1,
                             float x, float y, float w, float h,
                             std::array<float,4> color,
                             vkr::BlendMode blend = vkr::BlendMode::Blend,
                             float alpha_cutoff = -1.f,
                             bool has_scissor = false, VkRect2D scissor = {}) {
        _push_textured_rect(tex,u0,v0,u1,v1,x,y,w,h,color,blend,alpha_cutoff,has_scissor,scissor);
    }

    // Colored filled rect (untextured)
    void push_fill_rect(float x, float y, float w, float h,
                        std::array<float,4> color,
                        vkr::BlendMode blend = vkr::BlendMode::Blend,
                        bool has_scissor = false, VkRect2D scissor = {}) {
        _push_fill_rect(x,y,w,h,color,blend,has_scissor,scissor);
    }

    // Line (thin quad — 1 pixel wide in screen space)
    void push_line(float x1, float y1, float x2, float y2,
                   std::array<float,4> color) {
        _push_line(x1,y1,x2,y2,color);
    }

    // Outline rect
    void push_draw_rect(float x, float y, float w, float h,
                        std::array<float,4> color, int thickness = 1) {
        for (int i = 0; i < thickness; ++i) {
            float xi=x+i, yi=y+i, wi=w-i*2, hi=h-i*2;
            if (wi<=0||hi<=0) break;
            _push_line(xi,   yi,    xi+wi, yi,    color);
            _push_line(xi+wi,yi,    xi+wi, yi+hi, color);
            _push_line(xi+wi,yi+hi, xi,    yi+hi, color);
            _push_line(xi,   yi+hi, xi,    yi,    color);
        }
    }

    // Circle (midpoint algorithm, thin outline quads)
    void push_fill_circle(float cx, float cy, float r, std::array<float,4> color,
                          vkr::BlendMode blend = vkr::BlendMode::Blend) {
        for (int dy = -(int)r; dy <= (int)r; ++dy) {
            float dx = std::sqrt(std::max(0.f, r*r - (float)(dy*dy)));
            _push_fill_rect(cx-dx, cy+dy, dx*2.f, 1.f, color, blend);
        }
    }

    // ── Pixel-text helper (used by draw_ui for UIText / UIButton label) ───────
    // Implemented in render_system_vk.cpp (same as before). Kept as the
    // fallback path for when no TTF font atlas is loaded.
    void draw_pixel_text(const std::string& text, float x, float y, int scale,
                          std::array<float,4> color);

    // ── Real font-atlas text (preferred path — see vk_font_atlas.hpp) ────────
    // Draws `text` at baseline position (x, y) using the loaded FontAtlas,
    // scaled so its cap height matches `pixel_size` (UIText's font_size,
    // in pixels). Falls back to draw_pixel_text() automatically if no font
    // is loaded (see has_font_atlas()).
    bool has_font_atlas() const { return _font.valid(); }

    void draw_text_atlas(const std::string& text, float x, float y, float pixel_size,
                          std::array<float,4> color, const std::string& font_path = {}) {
        vkr::FontAtlas* atlas = _font_for(font_path);
        if (!atlas || !atlas->valid()) { draw_pixel_text(text, x, y, std::max(1, (int)(pixel_size/8)), color); return; }

        auto clamp01 = [](float v) { return std::max(0.f, std::min(1.f, v)); };
        std::array<float,4> shadow = {0.f, 0.f, 0.f, clamp01(color[3] * 0.75f)};
        vkr::Texture* tex = atlas->texture();
        float scale = pixel_size / vkr::FontAtlas::kBakePixelHeight;

        auto draw_pass = [&](float ox, float oy, const std::array<float,4>& tint) {
            // FontAtlas/stb_truetype's pen_y is the text BASELINE; UIText callers
            // pass the top-left of the line, so push the baseline down by the
            // ascent-ish amount (~bake size) before walking glyphs.
            float pen_x = (x + ox) / scale;
            float pen_y = ((y + oy) / scale) + vkr::FontAtlas::kBakePixelHeight * 0.8f;
            for (unsigned char ch : text) {
                vkr::FontAtlas::GlyphQuad q;
                if (atlas->get_glyph_quad((int)ch, &pen_x, &pen_y, q)) {
                    float w = (q.x1 - q.x0) * scale, h = (q.y1 - q.y0) * scale;
                    push_textured_rect(tex, q.u0, q.v0, q.u1, q.v1,
                                        q.x0 * scale, q.y0 * scale, w, h, tint);
                }
            }
        };

        // A tiny shadow/outline makes UI text readable over bright or noisy
        // backgrounds without changing the font asset itself.
        draw_pass(1.f, 1.f, shadow);
        draw_pass(0.f, 0.f, color);
    }

    // Pixel width of `text` if drawn via draw_text_atlas() at `pixel_size`.
    // Used by draw_ui()'s center/right alignment math. Falls back to the
    // pixel-font's fixed-width metric when no atlas is loaded.
    float measure_text_atlas(const std::string& text, float pixel_size,
                             const std::string& font_path = {}) {
        vkr::FontAtlas* atlas = _font_for(font_path);
        if (!atlas || !atlas->valid()) return (float)text.size() * 6.f * std::max(1, (int)(pixel_size/8));
        return atlas->measure_text(text) * (pixel_size / vkr::FontAtlas::kBakePixelHeight);
    }

    // Vertical advance between lines at `pixel_size`, matching draw_text_atlas.
    float text_line_height_atlas(float pixel_size, const std::string& font_path = {}) {
        vkr::FontAtlas* atlas = _font_for(font_path);
        if (!atlas || !atlas->valid()) return 8.f * std::max(1, (int)(pixel_size/8));
        return atlas->line_height() * (pixel_size / vkr::FontAtlas::kBakePixelHeight);
    }

    // Loads `path` (should be an ABSOLUTE path — resolve it relative to the
    // executable's own location the same way editor_main.cpp/main.cpp
    // resolve shader_dir, not relative to cwd) as the UIText/UIButton font
    // atlas. Safe to call again later with a different path (e.g. a future
    // per-project font override) — each call re-bakes and re-uploads a
    // fresh atlas. Failure is non-fatal: draw_text_atlas()/
    // measure_text_atlas() both fall back to the built-in pixel font
    // automatically via has_font_atlas() if no atlas is loaded.
    void load_default_font(const std::string& path) {
        _font.load(_get_backend().ctx(), path);
    }

private:
    // Resolves a component-selected font lazily and keeps it alive for the
    // renderer lifetime.  Font references authored in the Inspector are
    // project-relative; unlike a global working-directory lookup this keeps
    // both Play mode and standalone exports deterministic.
    vkr::FontAtlas* _font_for(const std::string& requested) {
        if (requested.empty()) return _font.valid() ? &_font : nullptr;
        namespace fs = std::filesystem;
        fs::path path(requested);
        if (path.is_relative() && !_asset_dir.empty()) path = fs::path(_asset_dir) / path;
        std::error_code ec;
        path = fs::weakly_canonical(path, ec);
        const std::string key = (ec ? fs::path(requested) : path).generic_string();
        auto& entry = _project_fonts[key];
        if (!entry.attempted) {
            entry.attempted = true;
            entry.atlas.load(_get_backend().ctx(), key);
        }
        return entry.atlas.valid() ? &entry.atlas : (_font.valid() ? &_font : nullptr);
    }

    Camera& _cam;
    bool    _owns_backend = true;
    std::unique_ptr<vkr::RendererBackend> _backend;    // null when _owns_backend=false
    vkr::RendererBackend* _external_backend = nullptr; // non-owning

    std::unique_ptr<TextureCache>            _textures;
    std::unique_ptr<material::MaterialCache> _materials;
    vkr::FontAtlas                           _font; // see load_default_font()
    struct FontCacheEntry {
        vkr::FontAtlas atlas;
        bool attempted = false;
    };
    std::unordered_map<std::string, FontCacheEntry> _project_fonts;
    std::string _asset_dir;
    tilepalette::Cache _tile_palette_cache;
    GlobalLightingSettings _global_lighting{};

    void _load_global_lighting_settings() {
        _global_lighting = GlobalLightingSettings{};
        if (_asset_dir.empty()) return;
        const std::filesystem::path settings_path = std::filesystem::path(_asset_dir) / "settings" / "shadow2d.json";
        std::ifstream input(settings_path);
        nlohmann::json json;
        try { if (!input || !(input >> json)) return; } catch (...) { return; }
        GlobalLightingSettings loaded = _global_lighting;
        loaded.enabled = json.value("enabled", loaded.enabled);
        loaded.ambient_intensity = json.value("ambient_intensity", loaded.ambient_intensity);
        loaded.max_lights = json.value("max_lights", loaded.max_lights);
        loaded.shadow_strength = json.value("shadow_strength", loaded.shadow_strength);
        const auto color = json.value("ambient_color", std::vector<float>{1.f, 1.f, 1.f});
        for (size_t index = 0; index < loaded.ambient_color.size() && index < color.size(); ++index)
            loaded.ambient_color[index] = color[index];
        set_global_lighting_settings(loaded);
    }

    // Current frame command buffer
    VkCommandBuffer _cmd = VK_NULL_HANDLE;
    bool _frame_begun = false;
    // Non-owning pointer to core.cpp's GPU emitter map — set via set_gpu_emitters().
    std::unordered_map<int, std::unique_ptr<vkr::GpuParticleCompute>>* _gpu_emitters = nullptr;

    // Layer tag written into each QuadCommand so the texture-sort in
    // SpriteBatch::flush() never reorders quads across draw phases.
    // 0 = parallax, 1 = world sprites, 2 = ui
    int _current_layer = 1;

    // Offscreen target for render_to_bytes
    std::unique_ptr<vkr::RenderTarget> _offscreen;
    // Set by begin_render_to_target()/cleared by end_render_to_target() so
    // _cmd_extent() (used by draw_ui()'s resolve() for anchor math) reports
    // the externally-owned target's size — e.g. the editor ViewportPanel's
    // resizable preview texture — instead of falling through to the main
    // swapchain/window extent. Without this, UI anchor positions are computed
    // against the editor window's size rather than the viewport texture's
    // size, so UIText/UIPanel/UIButton/UIImage quads land outside the
    // visible render target and never appear in the Play viewport.
    bool _target_extent_active = false;
    VkExtent2D _target_extent{};

    // Sorting layers
    std::vector<std::string> _sorting_layers;

    // Frustum-culling counters for the current frame, filled in by draw()'s
    // culling loop, read back via cull_stats(). See CullStats above.
    CullStats _cull_stats;

    // SpriteMask scissor state
    VkRect2D _active_scissor{};
    bool _mask_active = false;

    // ── Helpers ───────────────────────────────────────────────────────────────

    vkr::RendererBackend& _get_backend() {
        return _owns_backend ? *_backend : *_external_backend;
    }

    VkExtent2D _cmd_extent() {
        if (_target_extent_active) return _target_extent;
        if (_offscreen) return {_offscreen->width(), _offscreen->height()};
        return _get_backend().current_extent();
    }

    vkr::SpriteBatch& _batch() { return _get_backend().batch(); }

    // Normalized UVs from a source-pixel rect within a texture
    static void _src_to_uv(int tw, int th,
                             int sx, int sy, int sw, int sh,
                             float& u0, float& v0, float& u1, float& v1) {
        u0 = (float)sx / tw; v0 = (float)sy / th;
        u1 = (float)(sx+sw) / tw; v1 = (float)(sy+sh) / th;
    }

    // Core quad builder — rotated quad from (cx,cy) with (hw,hh) half-extents
    // and rotation angle in degrees CCW (matching SDL's CW-positive convention
    // by negating).
    void _push_quad_rotated(vkr::Texture* tex,
                              float u0, float v0, float u1, float v1,
                              float cx, float cy,   // center in screen pixels
                              float hw, float hh,   // half-width, half-height
                              float pivot_nx, float pivot_ny, // normalised pivot (0..1)
                              float angle_deg,       // CW rotation in degrees (SDL convention)
                              bool flip_h, bool flip_v,
                              std::array<float,4> color,
                              vkr::BlendMode blend,
                              float alpha_cutoff,
                              bool has_scissor, VkRect2D scissor,
                              // Sprite-Lit fields (task 6) — defaulted so all
                              // existing unlit call sites compile unchanged.
                              bool lit = false,
                              VkImageView normal_view = VK_NULL_HANDLE,
                              VkSampler   normal_sampler = VK_NULL_HANDLE,
                              float light_strength = 1.0f,
                              // GPU instancing (task 10)
                              bool use_instancing = false,
                              std::string custom_vert_spv = {},
                              std::string custom_frag_spv = {}) {
        // Apply flip to UVs
        if (flip_h) { std::swap(u0, u1); }
        if (flip_v) { std::swap(v0, v1); }

        // Build 4 corner positions relative to pivot offset.
        // SDL's pivot is relative to the dst rect top-left; our center (cx,cy)
        // already accounts for that so corners are just offsets from center.
        float rad = angle_deg * (float)M_PI / 180.f;
        float cos_a = std::cos(rad), sin_a = std::sin(rad);

        auto rot = [&](float lx, float ly) -> std::array<float,2> {
            return { cx + lx * cos_a - ly * sin_a,
                     cy + lx * sin_a + ly * cos_a };
        };

        // (cx,cy) is the pivot point in screen space. The unrotated
        // top-left corner relative to that pivot is -pivot*size; the other
        // three corners follow from the full width/height.
        float dst_w = hw * 2.f, dst_h = hh * 2.f;
        float tl_x = -pivot_nx * dst_w, tl_y = -pivot_ny * dst_h;

        auto p_tl = rot(tl_x,            tl_y);
        auto p_tr = rot(tl_x + dst_w,    tl_y);
        auto p_br = rot(tl_x + dst_w,    tl_y + dst_h);
        auto p_bl = rot(tl_x,            tl_y + dst_h);

        vkr::QuadCommand q;
        q.p0 = p_tl; q.p1 = p_tr; q.p2 = p_br; q.p3 = p_bl;
        q.uv0 = {u0, v0}; q.uv1 = {u1, v1};
        q.color[0]=color[0]; q.color[1]=color[1]; q.color[2]=color[2]; q.color[3]=color[3];
        q.texture_view = tex ? tex->image.view : VK_NULL_HANDLE;
        q.sampler      = tex ? tex->sampler    : VK_NULL_HANDLE;
        q.blend        = blend;
        q.alpha_cutoff = alpha_cutoff;
        q.has_scissor  = has_scissor;
        q.scissor      = scissor;
        q.layer        = _current_layer;
        q.lit            = lit;
        q.normal_view    = normal_view;
        q.normal_sampler = normal_sampler;
        q.light_strength = light_strength;
        q.use_instancing = use_instancing;
        q.custom_vert_spv = std::move(custom_vert_spv);
        q.custom_frag_spv = std::move(custom_frag_spv);
        _batch().push_quad(q);
    }

    // Axis-aligned textured rect (no rotation) — cheaper than the rotated path
    void _push_textured_rect(vkr::Texture* tex,
                              float u0, float v0, float u1, float v1,
                              float x, float y, float w, float h,
                              std::array<float,4> color,
                              vkr::BlendMode blend,
                              float alpha_cutoff,
                              bool has_scissor, VkRect2D scissor) {
        vkr::QuadCommand q;
        q.p0={x,   y  }; q.p1={x+w, y  };
        q.p2={x+w, y+h}; q.p3={x,   y+h};
        q.uv0={u0,v0}; q.uv1={u1,v1};
        q.color[0]=color[0];q.color[1]=color[1];q.color[2]=color[2];q.color[3]=color[3];
        q.texture_view = tex ? tex->image.view : VK_NULL_HANDLE;
        q.sampler      = tex ? tex->sampler    : VK_NULL_HANDLE;
        q.blend        = blend;
        q.alpha_cutoff = alpha_cutoff;
        q.has_scissor  = has_scissor;
        q.scissor      = scissor;
        q.layer        = _current_layer;
        _batch().push_quad(q);
    }

    void _push_fill_rect(float x, float y, float w, float h,
                          std::array<float,4> color, vkr::BlendMode blend,
                          bool has_scissor = false, VkRect2D scissor = {}) {
        _push_textured_rect(nullptr,0,0,1,1,x,y,w,h,color,blend,-1.f,has_scissor,scissor);
    }

    void _push_line(float x1, float y1, float x2, float y2, std::array<float,4> color) {
        // Emit a 1-pixel-wide quad along the line direction
        float dx = x2-x1, dy = y2-y1;
        float len = std::sqrt(dx*dx + dy*dy);
        if (len < 0.001f) return;
        float nx = -dy/len * 0.5f, ny = dx/len * 0.5f; // normal, half-pixel offset
        vkr::QuadCommand q;
        q.p0={x1+nx,y1+ny}; q.p1={x2+nx,y2+ny};
        q.p2={x2-nx,y2-ny}; q.p3={x1-nx,y1-ny};
        q.uv0={0,0}; q.uv1={1,1};
        q.color[0]=color[0];q.color[1]=color[1];q.color[2]=color[2];q.color[3]=color[3];
        q.texture_view = VK_NULL_HANDLE;
        q.sampler      = VK_NULL_HANDLE;
        q.blend        = vkr::BlendMode::Blend;
        q.alpha_cutoff = -1.f;
        q.layer        = _current_layer;
        _batch().push_quad(q);
    }

    // ── Sort key (identical logic to SDL2 version) ────────────────────────────
    struct SortKey { int sorting_layer=0; int legacy_layer=0; int order_in_layer=0; };

    SortKey _sort_key(Entity& e) {
        SortKey k;
        const Entity* group_owner = &e;
        for (int guard = 0; guard < 256; ++guard) {
            if (has_component(*group_owner, "SortingGroup")) {
                auto& sg = (*const_cast<Entity*>(group_owner))["components"]["SortingGroup"];
                k.sorting_layer  = sorting_layer_index(sg.value("sorting_layer", std::string("")));
                k.order_in_layer = sg.value("order_in_layer", 0);
                return k;
            }
            int pid = transform::parent_id_of(*group_owner);
            if (pid < 0) break;
            auto& reg = transform::registry();
            auto idx = transform::node_index(pid);
            if (idx == transform::npos()) break;
            Entity* parent = reg.nodes[idx].entity;
            if (!parent) break;
            group_owner = parent;
        }
        if (has_component(e, "SpriteRenderer")) {
            auto& sr = e["components"]["SpriteRenderer"];
            k.sorting_layer  = sorting_layer_index(sr.value("sorting_layer", std::string("")));
            k.legacy_layer   = sr.value("layer", 0);
            k.order_in_layer = sr.value("order_in_layer", 0);
            std::string mp = sr.value("material", std::string(""));
            if (!mp.empty()) if (auto m = _materials->get(mp)) k.order_in_layer += m->render_queue_offset;
        } else if (has_component(e, "Tilemap")) {
            auto& tm = e["components"]["Tilemap"];
            k.sorting_layer  = sorting_layer_index(tm.value("sorting_layer", std::string("")));
            k.order_in_layer = tm.value("order_in_layer", 0);
        } else if (has_component(e, "TextMeshPro2D")) {
            auto& text = e["components"]["TextMeshPro2D"];
            k.sorting_layer  = sorting_layer_index(text.value("sorting_layer", std::string("")));
            k.order_in_layer = text.value("order_in_layer", 0);
        } else if (has_component(e, "LineRenderer2D")) {
            auto& line = e["components"]["LineRenderer2D"];
            k.sorting_layer  = sorting_layer_index(line.value("sorting_layer", std::string("")));
            k.order_in_layer = line.value("order_in_layer", 0);
        } else if (has_component(e, "TrailRenderer2D")) {
            auto& trail = e["components"]["TrailRenderer2D"];
            k.sorting_layer  = sorting_layer_index(trail.value("sorting_layer", std::string("")));
            k.order_in_layer = trail.value("order_in_layer", 0);
        }
        return k;
    }

    static std::string _plain_text_markup(const std::string& source, bool rich_text) {
        if (!rich_text) return source;
        std::string output;
        for (size_t i = 0; i < source.size();) {
            if (source[i] != '<') { output.push_back(source[i++]); continue; }
            const size_t end = source.find('>', i + 1);
            if (end == std::string::npos) { output.push_back(source[i++]); continue; }
            std::string tag = source.substr(i + 1, end - i - 1);
            std::transform(tag.begin(), tag.end(), tag.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (tag == "br" || tag == "br/" || tag == "/br") output.push_back('\n');
            i = end + 1;
        }
        return output;
    }

    std::vector<std::string> _wrap_text_mesh(const std::string& source, float width,
                                             float pixel_size, const std::string& font) {
        std::vector<std::string> lines;
        size_t line_start = 0;
        while (line_start <= source.size()) {
            const size_t line_end = source.find('\n', line_start);
            const std::string raw = source.substr(line_start,
                line_end == std::string::npos ? std::string::npos : line_end - line_start);
            if (width <= 0.f || measure_text_atlas(raw, pixel_size, font) <= width) {
                lines.push_back(raw);
            } else {
                std::string current;
                size_t word_start = 0;
                while (word_start < raw.size()) {
                    while (word_start < raw.size() && std::isspace((unsigned char)raw[word_start])) ++word_start;
                    const size_t word_end = raw.find_first_of(" \t", word_start);
                    const std::string word = raw.substr(word_start,
                        word_end == std::string::npos ? std::string::npos : word_end - word_start);
                    if (word.empty()) break;
                    const std::string candidate = current.empty() ? word : current + " " + word;
                    if (!current.empty() && measure_text_atlas(candidate, pixel_size, font) > width) {
                        lines.push_back(current);
                        current = word;
                    } else current = candidate;
                    if (word_end == std::string::npos) break;
                    word_start = word_end + 1;
                }
                if (!current.empty() || raw.empty()) lines.push_back(current);
            }
            if (line_end == std::string::npos) break;
            line_start = line_end + 1;
        }
        return lines;
    }

    void _draw_text_mesh(Entity& e) {
        auto& text = e["components"]["TextMeshPro2D"];
        if (!text.value("enabled", true)) return;
        const std::string raw = text.value("text", std::string());
        if (raw.empty()) return;

        const auto wt = transform::cached_world(e);
        const auto screen = _cam.world_to_screen(wt.x, wt.y);
        const std::string font = text.value("font", std::string());
        const float face = std::clamp(text.value("face_dilate", 0.f), -1.f, 1.f);
        float pixel_size = std::max(4.f, text.value("font_size", 24.f) *
            std::max(.01f, std::abs(wt.scale_y)) * std::max(.01f, _cam.zoom) * (1.f + face * .12f));
        const float width = std::max(0.f, text.value("bounds_w", 200.f) * std::abs(wt.scale_x) * _cam.zoom);
        const float height = std::max(0.f, text.value("bounds_h", 0.f) * std::abs(wt.scale_y) * _cam.zoom);

        const auto col = text.value("color", std::vector<int>{255,255,255,255});
        std::array<float,4> color = {
            (col.size() > 0 ? col[0] : 255) / 255.f,
            (col.size() > 1 ? col[1] : 255) / 255.f,
            (col.size() > 2 ? col[2] : 255) / 255.f,
            (col.size() > 3 ? col[3] : 255) / 255.f
        };
        const auto outline_col = text.value("outline_color", std::vector<int>{0,0,0,255});
        const std::array<float,4> outline = {
            (outline_col.size() > 0 ? outline_col[0] : 0) / 255.f,
            (outline_col.size() > 1 ? outline_col[1] : 0) / 255.f,
            (outline_col.size() > 2 ? outline_col[2] : 0) / 255.f,
            (outline_col.size() > 3 ? outline_col[3] : 255) / 255.f
        };
        const float outline_px = std::clamp(text.value("outline_width", 0.f) * pixel_size * .12f, 0.f, 4.f);
        const bool wraps = text.value("wrapping", true);
        const std::string source = _plain_text_markup(raw, text.value("rich_text", true));
        // Auto-size is a real render-time fitting pass: the Inspector's
        // min/max values choose the largest readable size that fits both
        // configured bounds.  It is intentionally bounded and deterministic
        // for Play mode and standalone export.
        if (text.value("auto_size", false) && width > 0.f && height > 0.f) {
            const float min_size = std::max(4.f, text.value("min_size", 8.f) * std::abs(wt.scale_y) * _cam.zoom);
            const float max_size = std::max(min_size, text.value("max_size", 72.f) * std::abs(wt.scale_y) * _cam.zoom);
            pixel_size = std::clamp(pixel_size, min_size, max_size);
            for (; pixel_size > min_size; pixel_size -= 1.f) {
                const auto fitted = _wrap_text_mesh(source, wraps ? width : 0.f, pixel_size, font);
                if ((float)fitted.size() * text_line_height_atlas(pixel_size, font) <= height) break;
            }
        }
        std::vector<std::string> lines = _wrap_text_mesh(source, wraps ? width : 0.f, pixel_size, font);
        const float line_height = text_line_height_atlas(pixel_size, font);
        if (height > 0.f) lines.resize(std::min(lines.size(), (size_t)std::max(1, (int)(height / line_height))));

        const std::string align = text.value("alignment", std::string("center"));
        float y = screen.second;
        for (const std::string& line : lines) {
            const float line_width = measure_text_atlas(line, pixel_size, font);
            float x = screen.first;
            if (align == "center") x -= line_width * .5f;
            else if (align == "right") x -= line_width;
            if (outline_px > 0.f) {
                for (int oy = -1; oy <= 1; ++oy) for (int ox = -1; ox <= 1; ++ox) {
                    if (ox == 0 && oy == 0) continue;
                    draw_text_atlas(line, x + ox * outline_px, y + oy * outline_px, pixel_size, outline, font);
                }
            }
            draw_text_atlas(line, x, y, pixel_size, color, font);
            y += line_height;
        }
    }

    // Wide, colour-interpolated world polyline used by LineRenderer2D.  The
    // old Inspector exposed a point list, widths, colors and texture field
    // without a Vulkan draw path; every authored control now reaches the
    // same quad batch used by sprites in Editor, Play, and standalone builds.
    void _push_line_strip_segment(float x1, float y1, float x2, float y2,
                                  float width, const std::array<float, 4>& color,
                                  vkr::Texture* texture, float u0, float u1) {
        const float dx = x2 - x1, dy = y2 - y1;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (!std::isfinite(length) || length < .001f || width <= 0.f) return;
        const float half_width = width * .5f;
        const float nx = -dy / length * half_width;
        const float ny =  dx / length * half_width;
        vkr::QuadCommand q;
        q.p0 = {x1 + nx, y1 + ny}; q.p1 = {x2 + nx, y2 + ny};
        q.p2 = {x2 - nx, y2 - ny}; q.p3 = {x1 - nx, y1 - ny};
        q.uv0 = {u0, 0.f}; q.uv1 = {u1, 1.f};
        q.color[0] = color[0]; q.color[1] = color[1]; q.color[2] = color[2]; q.color[3] = color[3];
        q.texture_view = texture ? texture->image.view : VK_NULL_HANDLE;
        q.sampler = texture ? texture->sampler : VK_NULL_HANDLE;
        q.blend = vkr::BlendMode::Blend;
        q.alpha_cutoff = -1.f;
        q.layer = _current_layer;
        _batch().push_quad(q);
    }

    void _draw_line_renderer(Entity& e) {
        auto& line = e["components"]["LineRenderer2D"];
        if (!line.value("enabled", true)) return;
        const auto points = line.value("points", std::vector<float>{});
        if (points.size() < 4) return;
        const bool world_space = line.value("use_world_space", true);
        const bool closed = line.value("loop", false);
        const size_t point_count = points.size() / 2;
        const size_t segment_count = closed ? point_count : point_count - 1;
        if (segment_count == 0) return;

        transform::WorldTRS wt{};
        if (!world_space) {
            if (!has_component(e, "Transform")) return;
            wt = transform::cached_world(e);
        }
        const float radians = wt.rotation * (float)M_PI / 180.f;
        const float cs = std::cos(radians), sn = std::sin(radians);
        const auto world_point = [&](size_t index) {
            float x = points[index * 2], y = points[index * 2 + 1];
            if (!world_space) {
                x *= wt.scale_x; y *= wt.scale_y;
                const float rotated_x = x * cs - y * sn;
                y = x * sn + y * cs;
                x = rotated_x + wt.x;
                y += wt.y;
            }
            return std::pair<float, float>{x, y};
        };
        const auto start_rgba = line.value("color_start", std::vector<int>{255, 255, 255, 255});
        const auto end_rgba = line.value("color_end", std::vector<int>{255, 255, 255, 0});
        const auto color_at = [&](float t) {
            std::array<float, 4> color{};
            for (int i = 0; i < 4; ++i) {
                const float first = (start_rgba.size() > (size_t)i ? start_rgba[i] : 255) / 255.f;
                const float last = (end_rgba.size() > (size_t)i ? end_rgba[i] : (i == 3 ? 255 : 255)) / 255.f;
                color[i] = first + (last - first) * t;
            }
            return color;
        };
        const float first_width = std::max(0.f, line.value("width_start", 4.f)) * _cam.zoom;
        const float last_width = std::max(0.f, line.value("width_end", 4.f)) * _cam.zoom;
        const std::string texture_name = line.value("texture", std::string());
        vkr::Texture* texture = texture_name.empty() ? nullptr : _textures->get(texture_name);

        // Subdividing each segment is how a quad batch represents a real
        // gradient/taper without adding a separate renderer pipeline.
        constexpr int kGradientSteps = 6;
        for (size_t segment = 0; segment < segment_count; ++segment) {
            const auto a = world_point(segment);
            const auto b = world_point((segment + 1) % point_count);
            for (int step = 0; step < kGradientSteps; ++step) {
                const float t0 = ((float)segment + (float)step / kGradientSteps) / (float)segment_count;
                const float t1 = ((float)segment + (float)(step + 1) / kGradientSteps) / (float)segment_count;
                const float local0 = (float)step / kGradientSteps;
                const float local1 = (float)(step + 1) / kGradientSteps;
                const float x0 = a.first + (b.first - a.first) * local0;
                const float y0 = a.second + (b.second - a.second) * local0;
                const float x1 = a.first + (b.first - a.first) * local1;
                const float y1 = a.second + (b.second - a.second) * local1;
                const auto screen0 = _cam.world_to_screen(x0, y0);
                const auto screen1 = _cam.world_to_screen(x1, y1);
                const float width = first_width + (last_width - first_width) * ((t0 + t1) * .5f);
                _push_line_strip_segment(screen0.first, screen0.second, screen1.first, screen1.second,
                    width, color_at((t0 + t1) * .5f), texture, t0, t1);
            }
        }
    }

    // TrailRenderer2D points are generated by TrailRenderer2DSystem.  Draw
    // each live pair as a camera-space tapered segment, reusing the line
    // renderer's quad batch so texture, layer and alpha work in all targets.
    void _draw_trail_renderer(Entity& e) {
        auto& trail = e["components"]["TrailRenderer2D"];
        if (!trail.value("enabled", true)) return;
        const auto& points = trail["_trail_points"];
        if (!points.is_array() || points.size() < 2) return;
        const std::string texture_name = trail.value("texture", std::string());
        vkr::Texture* texture = texture_name.empty() ? nullptr : _textures->get(texture_name);
        const float denom = (float)std::max<size_t>(1, points.size() - 1);
        for (size_t index = 0; index + 1 < points.size(); ++index) {
            const auto& current = points[index];
            const auto& next = points[index + 1];
            const auto first = _cam.world_to_screen(current.value("x", 0.f), current.value("y", 0.f));
            const auto last = _cam.world_to_screen(next.value("x", 0.f), next.value("y", 0.f));
            std::array<float, 4> color{};
            color[0] = current.value("r", 255) / 255.f;
            color[1] = current.value("g", 255) / 255.f;
            color[2] = current.value("b", 255) / 255.f;
            color[3] = current.value("a", 255) / 255.f;
            const float width = std::max(0.f, current.value("width", 0.f)) * _cam.zoom;
            const float u0 = (float)index / denom;
            const float u1 = (float)(index + 1) / denom;
            _push_line_strip_segment(first.first, first.second, last.first, last.second,
                width, color, texture, u0, u1);
        }
    }

    // Lightweight 2D occlusion pass.  Every shadow caster projects an
    // opacity-tapered silhouette away from each nearby Light2D.  It is a
    // real rendered result (and honours the Inspector's cast/self/layer
    // controls) while retaining the engine's existing single-pass sprite
    // lighting pipeline rather than pretending to have a stencil shadow map.
    void _draw_shadow_caster(Entity& caster, EntityList& entities) {
        if (!_global_lighting.enabled || _global_lighting.shadow_strength <= 0.f) return;
        auto& settings = caster["components"]["Shadow2DCaster"];
        if (!settings.value("enabled", true) || !settings.value("cast_shadows", true)) return;
        const auto caster_world = transform::cached_world(caster);
        float half_extent = std::max(2.f, settings.value("silhouette_radius", 24.f));
        if (settings.value("use_renderer_silhouette", true) && has_component(caster, "SpriteRenderer")) {
            const auto& sprite = caster["components"]["SpriteRenderer"];
            const float width = std::max(4.f, sprite.value("width", 64.f) * std::abs(caster_world.scale_x));
            const float height = std::max(4.f, sprite.value("height", 64.f) * std::abs(caster_world.scale_y));
            half_extent = std::max(width, height) * .5f;
        }
        const float strength = std::clamp(settings.value("shadow_strength", .55f), 0.f, 1.f)
                               * _global_lighting.shadow_strength;
        const int caster_mask = settings.value("layer_mask", 65535);
        for (auto& light_entity : entities) {
            if (!entity_active(light_entity) || !has_component(light_entity, "Light2D") ||
                !has_component(light_entity, "Transform")) continue;
            const auto& light = light_entity["components"]["Light2D"];
            if (!light.value("enabled", true) || !light.value("cast_shadows", true)) continue;
            const int light_layer = light.value("layer", 0);
            if ((caster_mask & (1 << std::clamp(light_layer, 0, 30))) == 0) continue;
            const auto light_world = transform::cached_world(light_entity);
            const float dx = caster_world.x - light_world.x;
            const float dy = caster_world.y - light_world.y;
            const float distance = std::hypot(dx, dy);
            const float radius = std::max(1.f, light.value("radius", 200.f));
            if (distance < .001f || distance >= radius) continue;
            const float nx = dx / distance, ny = dy / distance;
            const float start_x = caster_world.x + nx * half_extent;
            const float start_y = caster_world.y + ny * half_extent;
            const float length = std::max(0.f, std::min(radius - distance, settings.value("max_distance", radius)));
            if (length < .5f) continue;
            const float end_x = start_x + nx * length;
            const float end_y = start_y + ny * length;
            const auto start = _cam.world_to_screen(start_x, start_y);
            const auto end = _cam.world_to_screen(end_x, end_y);
            const float fade = 1.f - distance / radius;
            const float alpha = strength * fade * (settings.value("self_shadows", false) ? .85f : .65f);
            _push_line_strip_segment(start.first, start.second, end.first, end.second,
                half_extent * 2.f * _cam.zoom, {0.f, 0.f, 0.f, alpha}, nullptr, 0.f, 1.f);
        }
    }

    std::pair<float,float> _interp_pos(const Entity& e, const transform::WorldTRS& wt, float alpha) {
        float px = e.value("_prev_x", wt.x);
        float py = e.value("_prev_y", wt.y);
        return { px + (wt.x-px)*alpha, py + (wt.y-py)*alpha };
    }

    // ── _draw_sprite_mask ─────────────────────────────────────────────────────
    void _draw_sprite_mask(Entity& e) {
        auto& sm = e["components"]["SpriteMask"];
        std::string tex_name = sm.value("texture", std::string(""));
        auto resolved = tex_name.empty() ? ResolvedSprite{} : _textures->get_sprite(tex_name);

        auto wt = transform::cached_world(e);
        auto [sx, sy] = _cam.world_to_screen(wt.x, wt.y);

        int base_w = 32, base_h = 32;
        if (resolved.texture) {
            int sw2 = sm.value("src_w", 0), sh2 = sm.value("src_h", 0);
            if (sw2>0&&sh2>0){base_w=sw2;base_h=sh2;}
            else{base_w=(int)resolved.texture->width; base_h=(int)resolved.texture->height;}
        }
        const float raw_w = base_w * std::abs(wt.scale_x) * _cam.zoom;
        const float raw_h = base_h * std::abs(wt.scale_y) * _cam.zoom;
        const VkExtent2D extent = _cmd_extent();

        // Dynamic Vulkan scissor rectangles must stay inside the active render
        // target.  A moving camera can legitimately put a mask partly or wholly
        // offscreen; feeding that raw negative/oversized rectangle to a driver
        // is not legitimate and has caused device-loss-style failures on some
        // Vulkan stacks.  An empty mask simply clips all following masked sprites.
        if (!std::isfinite(sx) || !std::isfinite(sy) || !std::isfinite(raw_w) ||
            !std::isfinite(raw_h) || raw_w <= 0.f || raw_h <= 0.f ||
            extent.width == 0 || extent.height == 0) {
            _active_scissor = VkRect2D{{0, 0}, {0, 0}};
            _mask_active = true;
            return;
        }

        const double left   = (double)sx - (double)raw_w * 0.5;
        const double top    = (double)sy - (double)raw_h * 0.5;
        const double right  = left + (double)raw_w;
        const double bottom = top  + (double)raw_h;
        const int64_t x0 = std::max<int64_t>(0, (int64_t)std::floor(left));
        const int64_t y0 = std::max<int64_t>(0, (int64_t)std::floor(top));
        const int64_t x1 = std::min<int64_t>((int64_t)extent.width,  (int64_t)std::ceil(right));
        const int64_t y1 = std::min<int64_t>((int64_t)extent.height, (int64_t)std::ceil(bottom));
        const uint32_t clipped_w = x1 > x0 ? (uint32_t)(x1 - x0) : 0u;
        const uint32_t clipped_h = y1 > y0 ? (uint32_t)(y1 - y0) : 0u;
        _active_scissor = VkRect2D{{(int32_t)x0, (int32_t)y0}, {clipped_w, clipped_h}};
        _mask_active = true;
    }

    // Builds the CPU-authored portion of CustomRenderTexture2D into a normal
    // GPU sampled texture.  The component owns the update cadence; this render
    // helper only uploads when its revision changes, so an on-demand texture
    // never turns into an accidental per-frame Vulkan allocation.
    vkr::Texture* _resolve_custom_render_texture(Entity& e) {
        if (!has_component(e, "CustomRenderTexture2D")) return nullptr;
        auto& crt = e["components"]["CustomRenderTexture2D"];
        if (!crt.value("enabled", true)) return nullptr;

        const int id = e.value("id", 0);
        const std::string key = "__runtime/crt/" + std::to_string(id);
        const int revision = std::max(1, crt.value("_runtime_revision", 1));
        const int uploaded = crt.value("_runtime_uploaded_revision", 0);
        if (uploaded == revision) return _textures->get(key);

        const int width = std::clamp(crt.value("width", 256), 1, 2048);
        const int height = std::clamp(crt.value("height", 256), 1, 2048);
        const auto clear = crt.value("clear_color", std::vector<int>{255,255,255,255});
        const auto channel = [&](int index) -> uint8_t {
            return static_cast<uint8_t>(std::clamp(index < (int)clear.size() ? clear[index] : 255, 0, 255));
        };
        const uint8_t base_r = channel(0), base_g = channel(1), base_b = channel(2), base_a = channel(3);
        const std::string generator = crt.value("generator", std::string("solid"));
        const int checker_size = std::clamp(crt.value("checker_size", 16), 1, 512);
        const float phase = crt.value("_runtime_phase", 0.f);
        const uint32_t seed = static_cast<uint32_t>(crt.value("seed", 1));
        std::vector<uint8_t> pixels((size_t)width * (size_t)height * 4u);

        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                uint8_t r = base_r, g = base_g, b = base_b, a = base_a;
                if (generator == "checker") {
                    const bool dark = (((x / checker_size) + (y / checker_size)) & 1) != 0;
                    if (dark) { r = (uint8_t)(r * 0.42f); g = (uint8_t)(g * 0.42f); b = (uint8_t)(b * 0.42f); }
                } else if (generator == "radial") {
                    const float nx = ((float)x + .5f) / (float)width * 2.f - 1.f;
                    const float ny = ((float)y + .5f) / (float)height * 2.f - 1.f;
                    const float falloff = std::clamp(1.f - std::sqrt(nx * nx + ny * ny), 0.f, 1.f);
                    a = static_cast<uint8_t>((float)a * falloff);
                } else if (generator == "noise") {
                    uint32_t n = (uint32_t)x * 1973u ^ (uint32_t)y * 9277u ^ seed * 26699u
                                 ^ (uint32_t)(phase * 60.f) * 104729u;
                    n = (n << 13) ^ n;
                    const float grain = .58f + (float)((n * (n * n * 15731u + 789221u) + 1376312589u) & 0xffu) / 255.f * .42f;
                    r = static_cast<uint8_t>(std::clamp((int)(r * grain), 0, 255));
                    g = static_cast<uint8_t>(std::clamp((int)(g * grain), 0, 255));
                    b = static_cast<uint8_t>(std::clamp((int)(b * grain), 0, 255));
                }
                const size_t offset = ((size_t)y * (size_t)width + (size_t)x) * 4u;
                pixels[offset] = r; pixels[offset + 1] = g; pixels[offset + 2] = b; pixels[offset + 3] = a;
            }
        }

        const auto filter = crt.value("filter_mode", std::string("bilinear")) == "point"
            ? texture::FilterMode::Point : texture::FilterMode::Bilinear;
        const auto wrap = crt.value("wrap_mode", std::string("clamp")) == "repeat"
            ? texture::WrapMode::Repeat : texture::WrapMode::Clamp;
        vkr::Texture* texture = _textures->replace_dynamic(key, pixels.data(), (uint32_t)width, (uint32_t)height,
                                                            filter, wrap, false);
        if (texture) crt["_runtime_uploaded_revision"] = revision;
        return texture;
    }

    // ── _draw_sprite ──────────────────────────────────────────────────────────
    void _draw_sprite(Entity& e, float alpha) {
        auto& sr = e["components"]["SpriteRenderer"];
        if (!sr.value("enabled", true)) return;

        float opacity = std::max(0.f, std::min(1.f, sr.value("opacity", 1.f)));
        if (opacity <= 0) return;

        std::shared_ptr<material::MaterialAsset> mat;
        {
            std::string mp = sr.value("material", std::string(""));
            if (!mp.empty()) mat = _materials->get(mp);
        }

        // Runtime texture producers intentionally override the source texture
        // while retaining material tint/blend behaviour. Video takes priority
        // when both are attached because it is an authored presentation layer.
        vkr::Texture* component_texture = _resolve_custom_render_texture(e);
        if (has_component(e, "VideoPlayer2D")) {
            auto& video = e["components"]["VideoPlayer2D"];
            const std::string clip = video.value("clip", std::string());
            if (video.value("enabled", true) && !clip.empty()) {
                const float time = video.value("playback_time", 0.f);
                if (vkr::Texture* frame = _textures->get_animated_gif_frame(clip, time, video.value("loop", false)))
                    component_texture = frame;
            }
        }
        std::string tex_name = (mat && !mat->texture.empty()) ? mat->texture : sr.value("texture", std::string(""));
        ResolvedSprite resolved = tex_name.empty() ? ResolvedSprite{} : _textures->get_sprite(tex_name);
        vkr::Texture* tex = component_texture ? component_texture : resolved.texture;
        if (component_texture) resolved = ResolvedSprite{};

        auto wt = transform::cached_world(e);
        auto [wx, wy] = _interp_pos(e, wt, alpha);
        auto [sx, sy] = _cam.world_to_screen(wx, wy);
        float scale_x = wt.scale_x, scale_y = wt.scale_y;
        float rotation = wt.rotation;
        bool flip_x = sr.value("flip_x", false);
        bool flip_y = sr.value("flip_y", false);

        auto color = sr.value("color", std::vector<int>{255,255,255,255});
        float cr=1,cg=1,cb=1;
        if (color.size()>=3){cr=color[0]/255.f;cg=color[1]/255.f;cb=color[2]/255.f;}

        if (mat) {
            cr *= mat->color[0]/255.f;
            cg *= mat->color[1]/255.f;
            cb *= mat->color[2]/255.f;
            opacity *= mat->color[3]/255.f;
            if (opacity <= 0) return;
        }
        std::array<float,4> col = {cr, cg, cb, opacity};

        int base_w = 32, base_h = 32;
        int src_x=0,src_y=0,src_w=0,src_h=0;
        bool use_src = false;

        if (tex) {
            base_w = (int)tex->width; base_h = (int)tex->height;
            if (resolved.has_src) {
                src_x=resolved.src.x; src_y=resolved.src.y;
                src_w=resolved.src.w; src_h=resolved.src.h;
                use_src=true; base_w=src_w; base_h=src_h;
            }
            if (sr.value("use_source_rect", false)) {
                src_x=sr.value("source_x",0); src_y=sr.value("source_y",0);
                src_w=sr.value("source_w",0); src_h=sr.value("source_h",0);
                if (src_w>0&&src_h>0){use_src=true;base_w=src_w;base_h=src_h;}
            }
        }

        // Blend mode
        vkr::BlendMode blend = vkr::BlendMode::Blend;
        if (mat && mat->shader == material::Shader::SpriteAdditive) blend = vkr::BlendMode::Additive;

        // Alpha cutout
        float alpha_cutoff = -1.f;
        if (mat && mat->shader == material::Shader::SpriteCutout)
            alpha_cutoff = mat->alpha_cutoff;

        // Scissor from SpriteMask
        bool use_scissor = false;
        VkRect2D scissor{};
        std::string mask_mode = sr.value("mask_interaction", std::string("none"));
        if (_mask_active && mask_mode == "visible_inside_mask") {
            use_scissor = true;
            scissor = _active_scissor;
        }

        // Pivot
        float pivot_x = std::max(0.f,std::min(1.f,(float)sr.value("pivot_x", resolved.pivot_x)));
        float pivot_y = std::max(0.f,std::min(1.f,(float)sr.value("pivot_y", resolved.pivot_y)));

        std::string draw_mode = sr.value("draw_mode", std::string("simple"));

        if (!tex) {
            int dw=std::max(1,(int)(base_w*scale_x*_cam.zoom));
            int dh=std::max(1,(int)(base_h*scale_y*_cam.zoom));
            float x=sx-dw*pivot_x, y=sy-dh*(1.f-pivot_y);
            _push_fill_rect(x,y,dw,dh,col,blend,use_scissor,scissor);
            push_draw_rect(x,y,dw,dh,{1,1,1,col[3]});
            return;
        }

        // UV from source rect (or full texture)
        float u0=0,v0=0,u1=1,v1=1;
        if (use_src && tex) _src_to_uv((int)tex->width,(int)tex->height,src_x,src_y,src_w,src_h,u0,v0,u1,v1);

        if (draw_mode == "tiled") {
            _draw_sprite_tiled(tex, sr, u0,v0,u1,v1, base_w,base_h,
                                sx,sy,pivot_x,pivot_y,rotation,flip_x,flip_y,opacity,blend);
            return;
        }
        if (draw_mode == "sliced") {
            _draw_sprite_sliced(tex, sr, src_x,src_y,src_w,src_h, use_src, base_w,base_h,
                                 sx,sy,pivot_x,pivot_y,rotation,flip_x,flip_y,alpha_cutoff);
            return;
        }

        // ── Sprite-Lit (task 6) ───────────────────────────────────────────────
        // Determine if this sprite uses the lit pipeline and look up the
        // optional normal-map texture.  Additive-blended sprites are never
        // lit — the unlit path keeps them unchanged.
        // Also force unlit if this entity IS a light source — its own sprite
        // (e.g. a lantern icon) must not be affected by its own light UBO,
        // which would cause it to go black when the light is disabled.
        bool is_light_entity = has_component(e, "Light2D");
        bool is_lit = _global_lighting.enabled && !is_light_entity
                      && mat && mat->shader == material::Shader::SpriteLit
                      && blend != vkr::BlendMode::Additive;

        // GPU instancing (task 10): only for the "simple" draw path — tiled/sliced
        // have their own CPU-driven multi-quad expansion which doesn't benefit.
        // Also disabled for lit, cutout, additive, and scissored sprites since
        // those all have per-quad state the instanced pipeline doesn't encode.
        bool use_instancing = sr.value("gpu_instancing", false)
                              && draw_mode == "simple"
                              && !is_lit
                              && alpha_cutoff < 0.f
                              && blend == vkr::BlendMode::Blend
                              && !use_scissor;

        VkImageView normal_view   = VK_NULL_HANDLE;
        VkSampler   normal_smplr  = VK_NULL_HANDLE;
        if (is_lit && mat && !mat->normal_map.empty()) {
            vkr::Texture* ntex = _textures->get(mat->normal_map);
            if (ntex) {
                normal_view  = ntex->image.view;
                normal_smplr = ntex->sampler;
            }
        }

        // ── Custom shader (task 13) ───────────────────────────────────────────
        // Only applied on the simple unlit path — lit sprites always use the
        // sprite_lit pipeline; instanced sprites have their own pipeline.
        std::string custom_vert_spv, custom_frag_spv;
        if (mat && mat->has_custom_shader() && !is_lit && !use_instancing) {
            // Resolve relative to project base dir (same logic as texture paths)
            custom_vert_spv = _materials->resolve_path(mat->custom_vert_spv);
            custom_frag_spv = _materials->resolve_path(mat->custom_frag_spv);
        }

        // "simple"
        float dw=(float)std::max(1,(int)(base_w*scale_x*_cam.zoom));
        float dh=(float)std::max(1,(int)(base_h*scale_y*_cam.zoom));
        // pivot point in screen space
        float pivot_sx = sx;
        float pivot_sy = sy;
        _push_quad_rotated(tex, u0,v0,u1,v1,
            pivot_sx, pivot_sy, dw*0.5f, dh*0.5f,
            pivot_x, 1.f-pivot_y,
            rotation, flip_x, flip_y,
            col, blend, alpha_cutoff, use_scissor, scissor,
            is_lit, normal_view, normal_smplr,
            (mat && is_lit) ? mat->light_strength : 1.0f,
            use_instancing,
            custom_vert_spv, custom_frag_spv);
    }
    // ── Tiled draw mode ───────────────────────────────────────────────────────
    void _draw_sprite_tiled(vkr::Texture* tex, Entity& sr,
                             float u0, float v0, float u1, float v1,
                             int base_w, int base_h,
                             float sx, float sy, float pivot_x, float pivot_y,
                             float rotation, bool flip_x, bool flip_y,
                             float opacity, vkr::BlendMode blend) {
        float ppu = std::max(1.f,(float)sr.value("pixels_per_unit",100.0));
        float world_tile_w = (float)sr.value("tile_width",128);
        float world_tile_h = (float)sr.value("tile_height",128);

        float unit_per_px = 100.f / ppu;
        float cell_w = base_w * unit_per_px * _cam.zoom;
        float cell_h = base_h * unit_per_px * _cam.zoom;
        cell_w = std::max(1.f, cell_w);
        cell_h = std::max(1.f, cell_h);

        float box_w = std::max(1.f, world_tile_w * _cam.zoom);
        float box_h = std::max(1.f, world_tile_h * _cam.zoom);
        float box_x = sx - box_w * pivot_x;
        float box_y = sy - box_h * (1.f - pivot_y);

        bool need_rotate = std::fabs(rotation) > 0.001f;

        if (need_rotate) {
            // Render to offscreen, then blit rotated
            auto* rt = _acquire_temp_rt((uint32_t)box_w, (uint32_t)box_h);
            VkCommandBuffer cmd = _get_backend().begin_one_shot();
            rt->begin(cmd, 0,0,0,0);
            // Temporarily redirect the batch to this offscreen pass
            _batch().begin_frame(0, {(uint32_t)box_w, (uint32_t)box_h});
            _blit_tile_grid(tex, u0,v0,u1,v1, cell_w,cell_h, 0,0,(int)box_w,(int)box_h,
                            {1,1,1,opacity}, blend, flip_x, flip_y);
            _batch().flush(cmd);
            rt->end(cmd);
            _get_backend().end_one_shot(cmd);

            // Blit the offscreen as a rotated textured quad into the main batch
            float dw=box_w, dh=box_h;
            float pivot_sx = sx;
            float pivot_sy = sy;
            // Repoint batch to main frame dimensions
            _batch().begin_frame(_get_backend().frame_index() % vkr::kMaxFramesInFlight,
                                  _cmd_extent());
            vkr::QuadCommand q;
            float rad = rotation*(float)M_PI/180.f;
            float ca=std::cos(rad), sa=std::sin(rad);
            float tlx=-dw*pivot_x, tly=-dh*(1.f-pivot_y);
            auto rot=[&](float lx,float ly)->std::array<float,2>{
                return{pivot_sx+lx*ca-ly*sa, pivot_sy+lx*sa+ly*ca};};
            q.p0=rot(tlx,    tly);
            q.p1=rot(tlx+dw, tly);
            q.p2=rot(tlx+dw, tly+dh);
            q.p3=rot(tlx,    tly+dh);
            q.uv0={0,0}; q.uv1={1,1};
            q.color[0]=1;q.color[1]=1;q.color[2]=1;q.color[3]=opacity;
            q.texture_view = rt->image_view();
            // Use nearest sampler from the white texture's sampler as a stand-in
            // (the offscreen is already filtered). In practice the white texture
            // sampler is point — fine for this composed blit.
            q.sampler = _batch().white_texture().sampler;
            q.blend = blend;
            q.layer = _current_layer;
            _batch().push_quad(q);
            _release_temp_rt(rt); // safe: kept alive until next frame, see _acquire_temp_rt
        } else {
            _blit_tile_grid(tex, u0,v0,u1,v1, cell_w,cell_h,
                            (int)box_x,(int)box_y,(int)box_w,(int)box_h,
                            {1,1,1,opacity}, blend, flip_x, flip_y);
        }
    }

    void _blit_tile_grid(vkr::Texture* tex,
                          float u0, float v0, float u1, float v1,
                          float cell_w, float cell_h,
                          int box_x, int box_y, int box_w, int box_h,
                          std::array<float,4> col, vkr::BlendMode blend,
                          bool flip_x, bool flip_y) {
        if (box_w<=0||box_h<=0||cell_w<=0||cell_h<=0) return;
        float fu0=u0, fu1=u1;
        if (flip_x) std::swap(fu0,fu1);
        float fv0=v0, fv1=v1;
        if (flip_y) std::swap(fv0,fv1);

        VkRect2D clip{};
        clip.offset={box_x,box_y};
        clip.extent={(uint32_t)box_w,(uint32_t)box_h};

        int cols = (int)std::ceil(box_w / cell_w) + 1;
        int rows = (int)std::ceil(box_h / cell_h) + 1;
        for (int row=0;row<rows;++row) {
            for (int tile_col=0;tile_col<cols;++tile_col) {
                float dx = box_x + tile_col*cell_w;
                float dy = box_y + row*cell_h;
                if (dx >= box_x+box_w || dy >= box_y+box_h) continue;
                _push_textured_rect(tex, fu0,fv0,fu1,fv1, dx,dy,
                    (float)std::ceil(cell_w),(float)std::ceil(cell_h),
                    col, blend, -1.f, true, clip);
            }
        }
    }

    // ── 9-slice (sliced) draw mode ────────────────────────────────────────────
    void _draw_sprite_sliced(vkr::Texture* tex, Entity& sr,
                              int src_x, int src_y, int src_w, int src_h, bool use_src,
                              int base_w, int base_h,
                              float sx, float sy, float pivot_x, float pivot_y,
                              float rotation, bool flip_x, bool flip_y, float alpha_cutoff) {
        int sl=std::max(0,(int)sr.value("border_left",0));
        int sr_=std::max(0,(int)sr.value("border_right",0));
        int st=std::max(0,(int)sr.value("border_top",0));
        int sb=std::max(0,(int)sr.value("border_bottom",0));
        bool fill_center=sr.value("sliced_fill_center",true);

        int srcx=use_src?src_x:0, srcy=use_src?src_y:0;
        int srcw=use_src?src_w:base_w, srch=use_src?src_h:base_h;
        sl=std::min(sl,srcw/2); sr_=std::min(sr_,srcw/2);
        st=std::min(st,srch/2); sb=std::min(sb,srch/2);
        int mid_src_w=std::max(0,srcw-sl-sr_);
        int mid_src_h=std::max(0,srch-st-sb);

        float box_w=std::max(1.f,(float)sr.value("tile_width",128)*_cam.zoom);
        float box_h=std::max(1.f,(float)sr.value("tile_height",128)*_cam.zoom);
        float box_x=sx-box_w*pivot_x;
        float box_y=sy-box_h*(1.f-pivot_y);

        float bl=sl*_cam.zoom, br=sr_*_cam.zoom;
        float bt=st*_cam.zoom, bb=sb*_cam.zoom;
        if(bl+br>box_w&&bl+br>0){float s=box_w/(bl+br);bl*=s;br*=s;}
        if(bt+bb>box_h&&bt+bb>0){float s=box_h/(bt+bb);bt*=s;bb*=s;}
        float mid_w=std::max(0.f,box_w-bl-br);
        float mid_h=std::max(0.f,box_h-bt-bb);

        bool need_rotate=std::fabs(rotation)>0.001f;
        vkr::RenderTarget* rt = nullptr;
        float ox=box_x, oy=box_y;

        if (need_rotate) {
            rt=_acquire_temp_rt((uint32_t)box_w,(uint32_t)box_h);
            VkCommandBuffer cmd=_get_backend().begin_one_shot();
            rt->begin(cmd,0,0,0,0);
            _batch().begin_frame(0,{(uint32_t)box_w,(uint32_t)box_h});
            ox=0; oy=0;
        }

        int tw=(int)tex->width, th=(int)tex->height;
        bool tile_edges=sr.value("sliced_tile_edges",false);
        std::array<float,4> col={1,1,1,1};

        auto blit=[&](int ssx,int ssy,int ssw,int ssh,
                      float dx,float dy,float dw,float dh,bool tile){
            if(ssw<=0||ssh<=0||dw<=0||dh<=0) return;
            float u0,v0,u1,v1;
            _src_to_uv(tw,th,ssx,ssy,ssw,ssh,u0,v0,u1,v1);
            if(flip_x) std::swap(u0,u1);
            if(flip_y) std::swap(v0,v1);
            if(!tile){
                _push_textured_rect(tex,u0,v0,u1,v1,ox+dx,oy+dy,
                    (float)std::ceil(dw),(float)std::ceil(dh),col,
                    vkr::BlendMode::Blend,alpha_cutoff,false,{});
            } else {
                float cw=ssw*_cam.zoom,ch=ssh*_cam.zoom;
                // Reuse _blit_tile_grid with the source sub-rect UVs
                VkRect2D clip{};
                clip.offset={(int32_t)(ox+dx),(int32_t)(oy+dy)};
                clip.extent={(uint32_t)std::max(1,(int)std::ceil(dw)),
                             (uint32_t)std::max(1,(int)std::ceil(dh))};
                int cx2=(int)(ox+dx),cy2=(int)(oy+dy);
                int cw2=(int)std::ceil(dw),ch2=(int)std::ceil(dh);
                int c_cols=(int)std::ceil(dw/cw)+1;
                int c_rows=(int)std::ceil(dh/ch)+1;
                for(int r=0;r<c_rows;++r)
                    for(int c=0;c<c_cols;++c)
                        _push_textured_rect(tex,u0,v0,u1,v1,
                            cx2+c*cw,cy2+r*ch,(float)std::ceil(cw),(float)std::ceil(ch),
                            col,vkr::BlendMode::Blend,alpha_cutoff,true,clip);
                (void)cw2;(void)ch2;
            }
        };

        // Corners
        blit(srcx,             srcy,             sl,  st,  0,       0,       bl,    bt,    false);
        blit(srcx+sl+mid_src_w,srcy,             sr_,  st, bl+mid_w,0,       br,    bt,    false);
        blit(srcx,             srcy+st+mid_src_h, sl,  sb, 0,       bt+mid_h,bl,    bb,    false);
        blit(srcx+sl+mid_src_w,srcy+st+mid_src_h,sr_, sb, bl+mid_w,bt+mid_h,br,    bb,    false);
        // Edges
        blit(srcx+sl, srcy,              mid_src_w, st,  bl, 0,        mid_w, bt,    tile_edges);
        blit(srcx+sl, srcy+st+mid_src_h, mid_src_w, sb,  bl, bt+mid_h, mid_w, bb,    tile_edges);
        blit(srcx,    srcy+st,           sl,  mid_src_h,  0,  bt,       bl,    mid_h, tile_edges);
        blit(srcx+sl+mid_src_w, srcy+st, sr_, mid_src_h,  bl+mid_w,bt, br,    mid_h, tile_edges);
        // Center
        if(fill_center) blit(srcx+sl,srcy+st,mid_src_w,mid_src_h,bl,bt,mid_w,mid_h,tile_edges);

        if (rt) {
            VkCommandBuffer cmd=_get_backend().begin_one_shot();
            _batch().flush(cmd);
            rt->end(cmd);
            _get_backend().end_one_shot(cmd);

            // Blit rotated
            _batch().begin_frame(_get_backend().frame_index()%vkr::kMaxFramesInFlight,_cmd_extent());
            vkr::QuadCommand q;
            float rad=rotation*(float)M_PI/180.f;
            float ca=std::cos(rad),sa=std::sin(rad);
            float tlx=-box_w*pivot_x,tly=-box_h*(1.f-pivot_y);
            auto rot2=[&](float lx,float ly)->std::array<float,2>{
                return{sx+lx*ca-ly*sa,sy+lx*sa+ly*ca};};
            q.p0=rot2(tlx,     tly);
            q.p1=rot2(tlx+box_w,tly);
            q.p2=rot2(tlx+box_w,tly+box_h);
            q.p3=rot2(tlx,     tly+box_h);
            q.uv0={0,0};q.uv1={1,1};
            q.color[0]=q.color[1]=q.color[2]=q.color[3]=1;
            q.texture_view=rt->image_view();
            q.sampler=_batch().white_texture().sampler;
            q.blend=vkr::BlendMode::Blend;
            q.alpha_cutoff=alpha_cutoff;
            q.layer=_current_layer;
            _batch().push_quad(q);
            _release_temp_rt(rt); // safe: kept alive until next frame, see _acquire_temp_rt
        }
    }

    // ── Particles ─────────────────────────────────────────────────────────────
    void _draw_particles(Entity& e) {
        auto& emitter = e["components"]["ParticleEmitter"];
        int eid = e.value("id", -1);

        // Atlas texture (optional) — looked up once per emitter per frame.
        // cols/rows define a uniform grid; "frame" on each particle (stamped
        // by ParticleSystem) indexes into it row-major.
        std::string atlas_name = emitter.value("atlas_texture", std::string(""));
        vkr::Texture* atlas_tex = (!atlas_name.empty() && _textures) ? _textures->get(atlas_name) : nullptr;
        int atlas_cols = std::max(1, (int)emitter.value("atlas_cols", 1.0));
        int atlas_rows = std::max(1, (int)emitter.value("atlas_rows", 1.0));

        // ── GPU path: emitter was promoted above kGpuThreshold ──────────────
        // Read particle data directly from the host-visible SSBO (no extra
        // GPU round-trip — the buffer is VMA_MEMORY_USAGE_AUTO_PREFER_HOST and
        // already mapped). resolve_count() has updated _live_count by this
        // point (called earlier in the frame, after the per-frame fence).
        //
        // Task 12: GPU-path particles also carry a "frame" field (set at
        // spawn time, see core.cpp) and use the same curve evaluators as the
        // CPU path, so size/color curves and atlas frames look identical
        // whichever path an emitter is on. Sub-emitters stay CPU-only (see
        // class doc comment in systems.hpp) — a death/spawn burst from a
        // GPU-resident particle is rare and not worth a second compute pass.
        if (_gpu_emitters) {
            auto it = _gpu_emitters->find(eid);
            if (it != _gpu_emitters->end()) {
                auto& gpu = *it->second;
                const vkr::GpuParticle* pts = gpu.particles();
                uint32_t count = gpu.live_count();
                for (uint32_t i = 0; i < count; ++i) {
                    const auto& p = pts[i];
                    float t = (p.lifetime > 0.f) ? p.age / p.lifetime : 1.f;
                    float size = ParticleSystem::eval_size(emitter, t);
                    if (size <= 0.f) continue;
                    auto colf = ParticleSystem::eval_color(emitter, t);
                    std::array<float,4> col = {
                        std::max(0.f,std::min(1.f,colf[0]/255.f)),
                        std::max(0.f,std::min(1.f,colf[1]/255.f)),
                        std::max(0.f,std::min(1.f,colf[2]/255.f)),
                        std::max(0.f,std::min(1.f,colf[3]/255.f))
                    };
                    auto [psx,psy] = _cam.world_to_screen(p.x, p.y);
                    float r = std::max(1.f, size * _cam.zoom * 0.5f);
                    _draw_one_particle(psx, psy, r, col, atlas_tex, atlas_cols, atlas_rows, p.frame);
                }
                return; // GPU path handled — skip CPU array below
            }
        }

        // ── CPU path: small emitter (below kGpuThreshold) ───────────────────
        if (!e.contains("_particles")) return;
        for (auto& p : e["_particles"]) {
            float age=p.value("age",0.f),lifetime=p.value("lifetime",1.f);
            float t=(lifetime>0)?age/lifetime:1.f;

            bool is_sub = p.value("_is_sub", false);
            float size; std::array<float,4> col;
            if (is_sub) {
                // Sub-emitter particles use their own flat size/color (set
                // once at spawn time) rather than the parent emitter's
                // curve — they're a short, simple visual kick, not a
                // miniature copy of the main emitter's lifecycle.
                size = p.value("_sub_size", 3.f) * (1.f - t); // simple fade-to-nothing
                auto cv = p.value("_sub_color", std::vector<float>{255,255,255,255});
                auto at = [](const std::vector<float>& v, size_t i, float def){ return i<v.size()?v[i]:def; };
                col = {at(cv,0,255)/255.f, at(cv,1,255)/255.f, at(cv,2,255)/255.f,
                       std::max(0.f,std::min(1.f,(at(cv,3,255)/255.f)*(1.f-t)))};
            } else {
                size = ParticleSystem::eval_size(emitter, t);
                auto colf = ParticleSystem::eval_color(emitter, t);
                col = {std::max(0.f,std::min(1.f,colf[0]/255.f)),
                       std::max(0.f,std::min(1.f,colf[1]/255.f)),
                       std::max(0.f,std::min(1.f,colf[2]/255.f)),
                       std::max(0.f,std::min(1.f,colf[3]/255.f))};
            }
            if (size<=0) continue;
            auto [psx,psy]=_cam.world_to_screen(p.value("x",0.f),p.value("y",0.f));
            float r=std::max(1.f,size*_cam.zoom*0.5f);
            int frame = is_sub ? 0 : (int)p.value("frame", 0);
            _draw_one_particle(psx, psy, r, col, atlas_tex, atlas_cols, atlas_rows, frame);
        }
    }

    // Shared draw helper: either an atlas-textured quad (if an atlas texture
    // is set on the emitter) or the original flat-colored circle fallback.
    void _draw_one_particle(float cx, float cy, float r, std::array<float,4> col,
                             vkr::Texture* atlas_tex, int cols, int rows, int frame) {
        if (atlas_tex) {
            int total = std::max(1, cols * rows);
            frame = ((frame % total) + total) % total; // wrap negative/overflow safely
            int fx = frame % cols;
            int fy = frame / cols;
            float u0 = (float)fx / (float)cols, u1 = (float)(fx+1) / (float)cols;
            float v0 = (float)fy / (float)rows, v1 = (float)(fy+1) / (float)rows;
            float d = r * 2.f;
            _push_textured_rect(atlas_tex, u0, v0, u1, v1, cx - r, cy - r, d, d,
                                 col, vkr::BlendMode::Blend, -1.f, false, {});
        } else {
            push_fill_circle(cx, cy, r, col, vkr::BlendMode::Blend);
        }
    }

    // ── Light2D ───────────────────────────────────────────────────────────────
    // Screen-space light gizmo for the editor/runtime. The actual lighting
    // contribution is handled by the Sprite-Lit shader path; this overlay is
    // just a soft halo so the Light2D entity feels like a real light source.
    //
    // A single additive quad with a pre-baked radial texture is much cheaper
    // than the old many-shell approximation, and the texture's smooth falloff
    // removes the visible ring boundaries between successive layers.
    void _draw_light(Entity& e) {
        if (!_global_lighting.enabled) return;
        auto& light = e["components"]["Light2D"];
        auto wt = transform::cached_world(e);
        auto [cx, cy] = _cam.world_to_screen(wt.x, wt.y);

        // radius is stored in world-space pixels (at zoom=1).
        // Multiply by zoom to get screen-space size so the light disc
        // stays fixed in world space regardless of viewport zoom level.
        float radius_world = std::max(0.f, light.value("radius", 200.f));
        float radius = radius_world * std::max(_cam.zoom, 0.001f);
        float intensity = std::max(0.f, light.value("intensity", 1.f));
        if (radius <= 0.f || intensity <= 0.f) return;

        auto lcol = light.value("color", std::vector<int>{255, 230, 150, 255});
        float r = (lcol.size() > 0 ? lcol[0] : 255) / 255.f;
        float g = (lcol.size() > 1 ? lcol[1] : 230) / 255.f;
        float b = (lcol.size() > 2 ? lcol[2] : 150) / 255.f;
        float a = std::min(1.f, 0.28f * intensity);

        vkr::Texture& tex = _batch().light_texture();
        float x = cx - radius;
        float y = cy - radius;
        float d = radius * 2.f;
        _push_textured_rect(&tex, 0.f, 0.f, 1.f, 1.f, x, y, d, d,
                            {r, g, b, a}, vkr::BlendMode::Additive,
                            -1.f, false, {});
    }

    // ── Tilemap ───────────────────────────────────────────────────────────────
    void _draw_tilemap(Entity& e) {
        auto& tm=e["components"]["Tilemap"];
        auto wt=transform::cached_world(e);
        float tx=wt.x,ty=wt.y;
        const std::string palette_ref = tm.value("tile_palette", std::string(""));
        const tilepalette::Palette* palette = palette_ref.empty()
            ? nullptr : _tile_palette_cache.get(_asset_dir, palette_ref);
        const int tile_size=std::max(1, palette ? palette->cell_width : tm.value("tile_size",32));
        const int tile_height=std::max(1, palette ? palette->cell_height : tile_size);
        // A palette owns its cell geometry. Legacy maps can retain stale
        // _grid_cell_* values (often 64px) from a sprite-sheet workflow; using
        // those made art render in different cells from the palette brush.
        const float cell_width=palette ? (float)tile_size : std::max(1.f,tm.value("_grid_cell_width",(float)tile_size));
        const float cell_height=palette ? (float)tile_height : std::max(1.f,tm.value("_grid_cell_height",(float)tile_height));
        const float stride_x=std::max(1.f,cell_width+tm.value("_grid_cell_gap_x",0.f));
        const float stride_y=std::max(1.f,cell_height+tm.value("_grid_cell_gap_y",0.f));
        int origin_x=tm.value("origin_x",0);
        int origin_y=tm.value("origin_y",0);
        auto& grid=tm["grid"];
        std::string tileset_name=palette ? palette->atlas : tm.value("tileset","");
        vkr::Texture* tileset = tileset_name.empty() ? nullptr : _textures->get(tileset_name);
        int ts_w=0,ts_h=0;
        if(tileset){ts_w=(int)tileset->width;ts_h=(int)tileset->height;}

        // ── Tile-level frustum culling ────────────────────────────────────────
        // Compute the grid row/col range that overlaps the visible world rect
        // instead of looping every cell.  One extra tile of padding on each
        // edge avoids any clipping artefact at the border.
        const Camera::WorldBounds vis = _cam.visible_world_bounds(0.f);
        int num_rows = (int)grid.size();
        int num_cols = (num_rows > 0 && grid[0].is_array()) ? (int)grid[0].size() : 0;

        int col_min = 0, col_max = num_cols - 1;
        int row_min = 0, row_max = num_rows - 1;
        if (stride_x > 0.f && stride_y > 0.f) {
            col_min = std::max(0, (int)std::floor((vis.min_x - tx) / stride_x - origin_x) - 1);
            col_max = std::min(num_cols - 1, (int)std::ceil((vis.max_x - tx) / stride_x - origin_x) + 1);
            row_min = std::max(0, (int)std::floor((vis.min_y - ty) / stride_y - origin_y) - 1);
            row_max = std::min(num_rows - 1, (int)std::ceil((vis.max_y - ty) / stride_y - origin_y) + 1);
        }

        for(int row=row_min;row<=row_max;++row){
            if (row >= (int)grid.size()) break;
            for(int col=col_min;col<=col_max;++col){
                if(grid[row][col].is_null()||grid[row][col]<0) continue;
                int tile_id=grid[row][col].get<int>();
                float wx=tx+(col+origin_x)*stride_x+cell_width*0.5f, wy=ty+(row+origin_y)*stride_y+cell_height*0.5f;
                auto [tsx,tsy]=_cam.world_to_screen(wx,wy);
                float draw_w=std::max(1.f,cell_width*_cam.zoom), draw_h=std::max(1.f,cell_height*_cam.zoom);
                float dx=tsx-draw_w*0.5f, dy=tsy-draw_h*0.5f;

                if(tileset&&ts_w>0){
                    int srx = 0, sry = 0, src_w = tile_size, src_h = tile_size;
                    bool has_source = false;
                    if (palette) {
                        const auto found = palette->tiles.find(tile_id);
                        if (found != palette->tiles.end()) {
                            srx = found->second.atlas_x;
                            sry = found->second.atlas_y;
                            src_w = found->second.atlas_w;
                            src_h = found->second.atlas_h;
                            has_source = true;
                        }
                    } else {
                        const int cols_in_sheet = std::max(1, ts_w / tile_size);
                        srx = (tile_id % cols_in_sheet) * tile_size;
                        sry = (tile_id / cols_in_sheet) * tile_size;
                        has_source = true;
                    }
                    if(has_source && srx+src_w<=ts_w && sry+src_h<=ts_h){
                        float u0,v0,u1,v1;
                        _src_to_uv(ts_w,ts_h,srx,sry,src_w,src_h,u0,v0,u1,v1);
                        _push_textured_rect(tileset,u0,v0,u1,v1,dx,dy,draw_w,draw_h,
                            {1,1,1,1},vkr::BlendMode::Blend,-1.f,false,{});
                        continue;
                    }
                }
                // Fallback colored rect
                float shade=((60+(tile_id*17)%120)/255.f);
                _push_fill_rect(dx,dy,draw_w,draw_h,{shade,shade,shade,1.f},vkr::BlendMode::Blend);
                push_draw_rect(dx,dy,draw_w,draw_h,{20/255.f,20/255.f,20/255.f,1.f});
            }
        }
    }

    // ── Debug lines ───────────────────────────────────────────────────────────
    void _draw_debug_lines(EntityList& entities) {
        const auto draw_world_line = [this](float x1, float y1, float x2, float y2,
                                            const std::array<float,4>& color) {
            if (!std::isfinite(x1) || !std::isfinite(y1) ||
                !std::isfinite(x2) || !std::isfinite(y2)) return;
            auto [sx1, sy1] = _cam.world_to_screen(x1, y1);
            auto [sx2, sy2] = _cam.world_to_screen(x2, y2);
            if (!std::isfinite(sx1) || !std::isfinite(sy1) ||
                !std::isfinite(sx2) || !std::isfinite(sy2)) return;
            _push_line(sx1, sy1, sx2, sy2, color);
        };

        for(auto& e:entities){
            if(!e.contains("_debug_lines")) continue;
            for(auto& line:e["_debug_lines"]){
                auto col=line.value("color",std::vector<int>{255,255,0});
                std::array<float,4> c={
                    (col.size()>0?col[0]:255)/255.f,
                    (col.size()>1?col[1]:255)/255.f,
                    (col.size()>2?col[2]:0)/255.f, 1.f};
                draw_world_line(line.value("x1",0.f), line.value("y1",0.f),
                                line.value("x2",0.f), line.value("y2",0.f), c);
            }
        }
        for (const DebugLine& line : Debug::lines()) {
            draw_world_line(line.x1, line.y1, line.x2, line.y2,
                            {line.r / 255.f, line.g / 255.f,
                             line.b / 255.f, line.a / 255.f});
        }
    }

    // ── Temp render targets for rotated tiled/sliced sprites ─────────────────
    // Rotating a tiled or 9-sliced sprite needs an offscreen target to render
    // the unrotated tile grid/slice into before blitting it back rotated
    // (see _draw_sprite_tiled / _draw_sprite_sliced). Allocating a fresh
    // vkr::RenderTarget per call would mean a new VkImage/VkFramebuffer per
    // rotated sprite per frame — fine for a couple of platforms, a real leak
    // for a scene with many. This pool reuses targets by (width,height),
    // handing out at most one in-use instance per bucket per frame: a quad
    // pushed against rt->image_view() this frame is only flushed (sampled by
    // the GPU) later in this same frame's end_frame()/present(), so the
    // entry must not be reused again until that flush has gone out — calling
    // _release_temp_rt() right after push_quad() (as both call sites do)
    // marks it free for the *next* frame's first request of that size, not
    // immediately, via the frame-tag check below.
    struct TempRT {
        std::unique_ptr<vkr::RenderTarget> rt;
        uint64_t last_used_frame = (uint64_t)-1;
        bool in_use = false;
    };
    std::vector<TempRT> _temp_rt_pool;

    vkr::RenderTarget* _acquire_temp_rt(uint32_t w, uint32_t h) {
        uint64_t frame = _get_backend().frame_index();
        for (auto& e : _temp_rt_pool) {
            if (e.rt->width() == w && e.rt->height() == h &&
                !(e.in_use && e.last_used_frame == frame)) {
                e.in_use = true;
                e.last_used_frame = frame;
                return e.rt.get();
            }
        }
        TempRT entry;
        entry.rt = std::make_unique<vkr::RenderTarget>();
        entry.rt->create(_get_backend().ctx(), w, h);
        entry.in_use = true;
        entry.last_used_frame = frame;
        _temp_rt_pool.push_back(std::move(entry));
        return _temp_rt_pool.back().rt.get();
    }

    void _release_temp_rt(vkr::RenderTarget* rt) {
        for (auto& e : _temp_rt_pool) {
            if (e.rt.get() == rt) { e.in_use = false; return; }
        }
    }

    static float _lerp(float a, float b, float t) { return a+(b-a)*t; }
};
