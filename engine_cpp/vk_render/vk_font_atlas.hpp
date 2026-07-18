#pragma once
/*
 * vk_font_atlas.hpp — TTF -> single GPU texture atlas, baked once.
 *
 * Replaces draw_pixel_text()'s hardcoded 5x7 bitmap font as the *real*
 * text-rendering path for UIText/UIButton labels. This is the piece the
 * Vulkan migration notes flagged as "SDL_ttf not ported" — but it isn't
 * actually SDL_ttf-shaped. SDL_ttf re-rasterizes a fresh SDL_Surface for
 * every distinct (font, size, string) triple, which is the part that has
 * no Vulkan equivalent (no SDL_CreateTextureFromSurface). Instead, this
 * bakes every glyph in the font's character range into ONE texture atlas a
 * single time at load, using stb_truetype.h (public domain, header-only,
 * github.com/nothings/stb) — then every text draw is just picking UV
 * rects out of that atlas and pushing textured quads through the exact
 * same push_textured_rect() pipeline sprites and UI panels already use.
 *
 * Tradeoffs vs SDL_ttf, to set expectations:
 *   - Baked at ONE pixel size (kBakePixelHeight below). UIText's
 *     "font_size" field scales the baked glyphs up/down via the quad's
 *     destination size, same as scaling any other sprite — crisp at/near
 *     the bake size, softens the further a requested size drifts from it.
 *     SDL_ttf would re-rasterize per exact size instead. Pushing
 *     kBakePixelHeight up (or adding multiple bake sizes / MSDF) is the
 *     natural next step if large scaled-up text needs to look sharper.
 *   - One GPU texture, built once at startup — no per-frame/per-string
 *     CPU rasterization cost, unlike SDL_ttf's surface-per-draw-call
 *     pattern (which most SDL_ttf integrations end up caching by hand
 *     anyway).
 *   - ASCII 32..126 only (covers the UIText content seen in this project's
 *     scenes). Extend kFirstChar/kNumChars for more coverage.
 */

#include "vk_context.hpp"
#include "vk_texture.hpp"

// NOTE: STB_TRUETYPE_IMPLEMENTATION is intentionally NOT defined here.
// stb_truetype.h is a single-header library — exactly one translation unit
// in the whole link may define STB_TRUETYPE_IMPLEMENTATION before including
// it, or every stbtt_* function is multiply-defined at link time. This
// header (vk_font_atlas.hpp) is included from several TUs (render_system_vk.cpp,
// core.cpp, panels.hpp's TU, editor_main.cpp's TU via render_system_vk.hpp),
// so defining the implementation inline here would break the link the same
// way omitting vk_mem_alloc_impl.cpp once did for VMA (see that file's
// history in MIGRATION_PROGRESS.md). The implementation instead lives in
// its own tiny TU: vk_render/stb_truetype_impl.cpp — registered once in
// ENGINE_SOURCES in both CMakeLists.txt files, same pattern as VMA.
#include "../third_party/stb_truetype.h"

#include <array>
#include <fstream>
#include <string>
#include <vector>
#include <iostream>

namespace vkr {

class FontAtlas {
public:
    static constexpr int kFirstChar       = 32;
    static constexpr int kNumChars        = 95; // ' ' (32) .. '~' (126)
    static constexpr int kAtlasSize       = 512; // px, square
    static constexpr float kBakePixelHeight = 48.f; // see header note above

    // Loads `path`, bakes the atlas, uploads it via `uploader`. Returns
    // false (atlas stays !valid()) on any failure — callers should fall
    // back to draw_pixel_text() in that case rather than hard-fail, same
    // spirit as the engine's existing "missing texture -> tinted rect"
    // fallbacks elsewhere in render_system_vk.hpp.
    bool load(Context& ctx, const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            std::cerr << "[FontAtlas] Cannot open font file: " << path << "\n";
            return false;
        }
        std::vector<unsigned char> ttf_buf((std::istreambuf_iterator<char>(f)),
                                            std::istreambuf_iterator<char>());
        if (ttf_buf.empty()) {
            std::cerr << "[FontAtlas] Font file empty: " << path << "\n";
            return false;
        }

        // Single-channel (alpha-only) bitmap from stb_truetype; expanded to
        // RGBA8 below since the existing TextureUploader/sampler/descriptor
        // pipeline is RGBA-only (matches how every other texture in this
        // engine — sprites, UI panels — already flows through upload()).
        std::vector<unsigned char> alpha_bitmap(kAtlasSize * kAtlasSize, 0);
        _baked.resize(kNumChars);

        int bake_result = stbtt_BakeFontBitmap(
            ttf_buf.data(), 0, kBakePixelHeight,
            alpha_bitmap.data(), kAtlasSize, kAtlasSize,
            kFirstChar, kNumChars, _baked.data());

        if (bake_result <= 0) {
            std::cerr << "[FontAtlas] stbtt_BakeFontBitmap failed for " << path
                       << " (atlas too small for this font/size — try a smaller "
                          "kBakePixelHeight or larger kAtlasSize)\n";
            return false;
        }

        // Alpha-only -> RGBA8, white text (color comes from the per-draw
        // tint, same convention push_fill_rect/draw_pixel_text already use
        // — UIText's "color" field multiplies in at draw time).
        std::vector<unsigned char> rgba(kAtlasSize * kAtlasSize * 4);
        for (size_t i = 0; i < alpha_bitmap.size(); ++i) {
            rgba[i*4+0] = 255;
            rgba[i*4+1] = 255;
            rgba[i*4+2] = 255;
            rgba[i*4+3] = alpha_bitmap[i];
        }

        TextureUploader uploader(ctx);
        if (_valid) {
            // Re-load: free the previous atlas's GPU image/sampler first —
            // Texture is a plain struct with no RAII (see vk_texture.hpp),
            // so without this, calling load() a second time (e.g. a future
            // per-project font override) would leak the old VkImage/VkSampler.
            uploader.destroy(_texture);
            _valid = false;
        }
        _texture = uploader.upload(rgba.data(), kAtlasSize, kAtlasSize,
                                    FilterMode::Bilinear, false /*gen_mipmaps*/);
        _valid = _texture.valid();
        if (!_valid)
            std::cerr << "[FontAtlas] GPU upload failed for " << path << "\n";
        else
            std::cout << "[FontAtlas] Loaded " << path << " (" << kAtlasSize << "x"
                       << kAtlasSize << " atlas, bake size " << kBakePixelHeight << "px)\n";
        return _valid;
    }

    bool valid() const { return _valid; }
    Texture* texture() { return _valid ? &_texture : nullptr; }

    // Frees the atlas's GPU image/sampler. Must be called (by the owning
    // RenderSystem's destructor) while the Vulkan device/allocator are
    // still alive — Texture has no RAII of its own (see vk_texture.hpp),
    // so without this the atlas's VkImage/VkSampler leak at shutdown,
    // same class of bug load()'s re-bake path already guards against.
    void destroy(Context& ctx) {
        if (!_valid) return;
        TextureUploader uploader(ctx);
        uploader.destroy(_texture);
        _valid = false;
    }

    // One glyph's placement, in pixels, for cursor position `(x, y)` where
    // y is the text BASELINE (stb_truetype convention) — caller advances
    // `x` by the returned advance and keeps `y` fixed across a line.
    struct GlyphQuad {
        float u0, v0, u1, v1; // atlas UVs, 0..1
        float x0, y0, x1, y1; // destination pixel rect (relative to caller's origin)
    };

    // Mirrors stbtt_GetBakedQuad's pen-advance convention: pass &pen_x/&pen_y
    // (pen_y is the baseline), get back the glyph's screen quad, pen_x is
    // advanced for you. Unsupported/out-of-range characters advance pen_x by
    // a space-width fallback and return false (caller should skip drawing).
    bool get_glyph_quad(int ch, float* pen_x, float* pen_y, GlyphQuad& out) const {
        if (!_valid || ch < kFirstChar || ch >= kFirstChar + kNumChars) {
            // unknown glyph: fall back to the baked space's advance if we
            // have one, else a fixed guess, so layout doesn't collapse.
            *pen_x += (_valid && kNumChars > 0) ? _baked[0].xadvance : 8.f;
            return false;
        }
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(_baked.data(), kAtlasSize, kAtlasSize, ch - kFirstChar,
                            pen_x, pen_y, &q, 1 /*opengl_fillrule — also correct for our top-left UV convention*/);
        out.u0 = q.s0; out.v0 = q.t0; out.u1 = q.s1; out.v1 = q.t1;
        out.x0 = q.x0; out.y0 = q.y0; out.x1 = q.x1; out.y1 = q.y1;
        return true;
    }

    // Width in pixels of `text` if drawn at the atlas's native bake size
    // (i.e. scale == 1). Callers wanting a different font_size should
    // multiply the result by (font_size / kBakePixelHeight).
    float measure_text(const std::string& text) const {
        if (!_valid) return 0.f;
        float pen_x = 0.f, pen_y = 0.f;
        for (unsigned char ch : text) {
            GlyphQuad q;
            get_glyph_quad((int)ch, &pen_x, &pen_y, q);
        }
        return pen_x;
    }

    // Vertical distance from one line's baseline to the next, native scale.
    float line_height() const { return kBakePixelHeight * 1.15f; }

private:
    bool _valid = false;
    Texture _texture;
    std::vector<stbtt_bakedchar> _baked;
};

} // namespace vkr