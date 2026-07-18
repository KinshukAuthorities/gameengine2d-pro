/*
 * render_system_vk.cpp — implements RenderSystem::draw_ui() and
 * RenderSystem::draw_pixel_text() for the Vulkan backend (render_system_vk.hpp).
 *
 * Same split as the SDL2 engine: draw_ui() lives in its own translation unit
 * because UISystem-adjacent code wants input_system.hpp, which would create
 * a circular include if pulled into render_system_vk.hpp directly.
 *
 * Image decoding (PNG/JPG/etc) is handled by stb_image.h (see
 * texture_system_vk.hpp / vk_render/stb_image_impl.cpp) — no SDL_image,
 * no Windows WIC COM interop, fully cross-platform.
 *
 * Text rendering uses a real Vulkan-uploaded font atlas (see
 * vk_render/vk_font_atlas.hpp, stb_truetype-based), falling back to the
 * built-in 5x7 pixel font below only if no TTF could be loaded.
 */

#include "render_system_vk.hpp"
#include "systems.hpp"
#include "input_system.hpp"
#include <cmath>
#include <string>
#include <vector>

// ─── helper: draw 5x7 pixel-font glyph (no external deps) ───────────────────
// Identical bitmap font to the SDL2 build's render_system.cpp — only the
// final "set a pixel" step differs (push_fill_rect instead of SDL_RenderFillRect).
static const uint8_t FONT5x7[95][7] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x04,0x04,0x04,0x04,0x00,0x04,0x00}, // '!'
    {0x0A,0x0A,0x00,0x00,0x00,0x00,0x00}, // '"'
    {0x0A,0x1F,0x0A,0x1F,0x0A,0x00,0x00}, // '#'
    {0x0E,0x15,0x0E,0x15,0x0E,0x00,0x00}, // '$'
    {0x19,0x1A,0x04,0x0B,0x13,0x00,0x00}, // '%'
    {0x0C,0x12,0x0C,0x15,0x0A,0x00,0x00}, // '&'
    {0x04,0x04,0x00,0x00,0x00,0x00,0x00}, // '''
    {0x02,0x04,0x04,0x04,0x02,0x00,0x00}, // '('
    {0x08,0x04,0x04,0x04,0x08,0x00,0x00}, // ')'
    {0x00,0x15,0x0E,0x15,0x00,0x00,0x00}, // '*'
    {0x00,0x04,0x0E,0x04,0x00,0x00,0x00}, // '+'
    {0x00,0x00,0x00,0x04,0x08,0x00,0x00}, // ','
    {0x00,0x00,0x0E,0x00,0x00,0x00,0x00}, // '-'
    {0x00,0x00,0x00,0x00,0x04,0x00,0x00}, // '.'
    {0x01,0x02,0x04,0x08,0x10,0x00,0x00}, // '/'
    {0x0E,0x13,0x15,0x19,0x0E,0x00,0x00}, // '0'
    {0x04,0x0C,0x04,0x04,0x0E,0x00,0x00}, // '1'
    {0x0E,0x11,0x06,0x08,0x1F,0x00,0x00}, // '2'
    {0x0E,0x11,0x06,0x11,0x0E,0x00,0x00}, // '3'
    {0x02,0x06,0x0A,0x1F,0x02,0x00,0x00}, // '4'
    {0x1F,0x10,0x1E,0x01,0x1E,0x00,0x00}, // '5'
    {0x06,0x08,0x1E,0x11,0x0E,0x00,0x00}, // '6'
    {0x1F,0x01,0x02,0x04,0x04,0x00,0x00}, // '7'
    {0x0E,0x11,0x0E,0x11,0x0E,0x00,0x00}, // '8'
    {0x0E,0x11,0x0F,0x01,0x0E,0x00,0x00}, // '9'
    {0x00,0x04,0x00,0x04,0x00,0x00,0x00}, // ':'
    {0x00,0x04,0x00,0x04,0x08,0x00,0x00}, // ';'
    {0x02,0x04,0x08,0x04,0x02,0x00,0x00}, // '<'
    {0x00,0x1F,0x00,0x1F,0x00,0x00,0x00}, // '='
    {0x08,0x04,0x02,0x04,0x08,0x00,0x00}, // '>'
    {0x0E,0x11,0x06,0x00,0x04,0x00,0x00}, // '?'
    {0x0E,0x11,0x17,0x16,0x0F,0x00,0x00}, // '@'
    {0x04,0x0A,0x11,0x1F,0x11,0x00,0x00}, // 'A'
    {0x1E,0x11,0x1E,0x11,0x1E,0x00,0x00}, // 'B'
    {0x0E,0x11,0x10,0x11,0x0E,0x00,0x00}, // 'C'
    {0x1C,0x12,0x11,0x12,0x1C,0x00,0x00}, // 'D'
    {0x1F,0x10,0x1E,0x10,0x1F,0x00,0x00}, // 'E'
    {0x1F,0x10,0x1E,0x10,0x10,0x00,0x00}, // 'F'
    {0x0E,0x10,0x17,0x11,0x0E,0x00,0x00}, // 'G'
    {0x11,0x11,0x1F,0x11,0x11,0x00,0x00}, // 'H'
    {0x0E,0x04,0x04,0x04,0x0E,0x00,0x00}, // 'I'
    {0x01,0x01,0x01,0x11,0x0E,0x00,0x00}, // 'J'
    {0x11,0x12,0x1C,0x12,0x11,0x00,0x00}, // 'K'
    {0x10,0x10,0x10,0x10,0x1F,0x00,0x00}, // 'L'
    {0x11,0x1B,0x15,0x11,0x11,0x00,0x00}, // 'M'
    {0x11,0x19,0x15,0x13,0x11,0x00,0x00}, // 'N'
    {0x0E,0x11,0x11,0x11,0x0E,0x00,0x00}, // 'O'
    {0x1E,0x11,0x1E,0x10,0x10,0x00,0x00}, // 'P'
    {0x0E,0x11,0x11,0x13,0x0D,0x00,0x00}, // 'Q'
    {0x1E,0x11,0x1E,0x12,0x11,0x00,0x00}, // 'R'
    {0x0F,0x10,0x0E,0x01,0x1E,0x00,0x00}, // 'S'
    {0x1F,0x04,0x04,0x04,0x04,0x00,0x00}, // 'T'
    {0x11,0x11,0x11,0x11,0x0E,0x00,0x00}, // 'U'
    {0x11,0x11,0x11,0x0A,0x04,0x00,0x00}, // 'V'
    {0x11,0x11,0x15,0x1B,0x11,0x00,0x00}, // 'W'
    {0x11,0x0A,0x04,0x0A,0x11,0x00,0x00}, // 'X'
    {0x11,0x0A,0x04,0x04,0x04,0x00,0x00}, // 'Y'
    {0x1F,0x02,0x04,0x08,0x1F,0x00,0x00}, // 'Z'
    {0x0E,0x08,0x08,0x08,0x0E,0x00,0x00}, // '['
    {0x10,0x08,0x04,0x02,0x01,0x00,0x00}, // '\'
    {0x0E,0x02,0x02,0x02,0x0E,0x00,0x00}, // ']'
    {0x04,0x0A,0x00,0x00,0x00,0x00,0x00}, // '^'
    {0x00,0x00,0x00,0x00,0x1F,0x00,0x00}, // '_'
    {0x08,0x04,0x00,0x00,0x00,0x00,0x00}, // '`'
    {0x00,0x0E,0x01,0x0F,0x0F,0x00,0x00}, // 'a'
    {0x10,0x1E,0x11,0x11,0x1E,0x00,0x00}, // 'b'
    {0x00,0x0E,0x10,0x10,0x0E,0x00,0x00}, // 'c'
    {0x01,0x0F,0x11,0x11,0x0F,0x00,0x00}, // 'd'
    {0x00,0x0E,0x11,0x1F,0x0E,0x00,0x00}, // 'e'
    {0x06,0x04,0x0E,0x04,0x04,0x00,0x00}, // 'f'
    {0x00,0x0F,0x11,0x0F,0x01,0x0E,0x00}, // 'g'
    {0x10,0x1E,0x11,0x11,0x11,0x00,0x00}, // 'h'
    {0x04,0x00,0x04,0x04,0x04,0x00,0x00}, // 'i'
    {0x02,0x00,0x02,0x02,0x12,0x0C,0x00}, // 'j'
    {0x10,0x11,0x16,0x1C,0x12,0x00,0x00}, // 'k'
    {0x0C,0x04,0x04,0x04,0x0E,0x00,0x00}, // 'l'
    {0x00,0x1A,0x15,0x15,0x11,0x00,0x00}, // 'm'
    {0x00,0x1E,0x11,0x11,0x11,0x00,0x00}, // 'n'
    {0x00,0x0E,0x11,0x11,0x0E,0x00,0x00}, // 'o'
    {0x00,0x1E,0x11,0x1E,0x10,0x10,0x00}, // 'p'
    {0x00,0x0F,0x11,0x0F,0x01,0x01,0x00}, // 'q'
    {0x00,0x16,0x19,0x10,0x10,0x00,0x00}, // 'r'
    {0x00,0x0E,0x10,0x0E,0x01,0x0E,0x00}, // 's'
    {0x04,0x0E,0x04,0x04,0x06,0x00,0x00}, // 't'
    {0x00,0x11,0x11,0x11,0x0F,0x00,0x00}, // 'u'
    {0x00,0x11,0x11,0x0A,0x04,0x00,0x00}, // 'v'
    {0x00,0x11,0x15,0x15,0x0A,0x00,0x00}, // 'w'
    {0x00,0x11,0x0A,0x04,0x0A,0x11,0x00}, // 'x' (adjusted)
    {0x00,0x11,0x0A,0x04,0x08,0x10,0x00}, // 'y'
    {0x00,0x1F,0x02,0x04,0x1F,0x00,0x00}, // 'z'
    {0x06,0x04,0x08,0x04,0x06,0x00,0x00}, // '{'
    {0x04,0x04,0x04,0x04,0x04,0x00,0x00}, // '|'
    {0x0C,0x04,0x02,0x04,0x0C,0x00,0x00}, // '}'
    {0x08,0x15,0x02,0x00,0x00,0x00,0x00}, // '~'
};

// ─── RenderSystem::draw_pixel_text ───────────────────────────────────────────
void RenderSystem::draw_pixel_text(const std::string& text, float x, float y, int scale,
                                    std::array<float,4> color) {
    auto draw_pass = [&](float ox, float oy, std::array<float,4> tint) {
        float cx = x + ox;
        for (unsigned char ch : text) {
            if (ch < 32 || ch > 126) { cx += 6*scale; continue; }
            const uint8_t* glyph = FONT5x7[ch - 32];
            for (int row = 0; row < 7; ++row) {
                for (int col = 0; col < 5; ++col) {
                    if (glyph[row] & (1 << (4 - col))) {
                        push_fill_rect(cx + col*scale, y + oy + row*scale, (float)scale, (float)scale, tint);
                    }
                }
            }
            cx += 6*scale;
        }
    };

    std::array<float,4> shadow = {0.f, 0.f, 0.f, std::min(0.75f, color[3] * 0.75f)};
    draw_pass(1.f, 1.f, shadow);
    draw_pass(0.f, 0.f, color);
}

static int pixel_text_width(const std::string& text, int scale) {
    return (int)text.size() * 6 * scale;
}

// ─── RenderSystem::draw_ui ────────────────────────────────────────────────────
void RenderSystem::draw_ui(EntityList& entities, int mouse_x, int mouse_y,
                             bool mouse_down, bool mouse_just_down) {
    _current_layer = 2; // UI is always on top of world sprites and parallax
    VkExtent2D ext = _cmd_extent();
    int sw = (int)ext.width, sh = (int)ext.height;

    // Collect active UI entities
    std::vector<Entity*> ui_ents;
    for (auto& e : entities)
        if (entity_active(e) && e.contains("components"))
            for (auto& [k,v] : e["components"].items())
                if (k.rfind("UI",0)==0) { ui_ents.push_back(&e); break; }

    // Sort by per-component "order" field so layering works
    std::stable_sort(ui_ents.begin(), ui_ents.end(), [](Entity* a, Entity* b){
        int la=0, lb=0;
        for (auto& [k,v]:(*a)["components"].items()) if(k.rfind("UI",0)==0) la=v.value("order",0);
        for (auto& [k,v]:(*b)["components"].items()) if(k.rfind("UI",0)==0) lb=v.value("order",0);
        return la < lb;
    });

    // UI authored for a reference canvas can opt into uniform responsive
    // scaling.  This deliberately is opt-in: legacy UI keeps its literal
    // pixel layout, while game UI can specify `responsive: true` plus a
    // reference_width/reference_height and remain inside a narrow editor
    // viewport, a 16:9 standalone window, or a high-resolution display.
    // Position, dimensions, font sizes, button hitboxes, and renderer output
    // all use the same scale so a resized UI cannot become unclickable.
    auto responsive_scale = [&](const Entity& comp) -> float {
        if (!comp.value("responsive", false)) return 1.0f;
        const float ref_w = std::max(1, comp.value("reference_width", 1280));
        const float ref_h = std::max(1, comp.value("reference_height", 720));
        const float raw = std::min((float)sw / ref_w, (float)sh / ref_h);
        // Full-screen game overlays must be allowed to become smaller than a
        // designer's readability floor when the editor viewport is narrow.
        // Otherwise a 1000px HUD hint can *never* fit in a 500px embedded
        // viewport because min_scale forces it back outside the screen.
        // `responsive_fit` is opt-in so compact inspector widgets retain
        // their existing minimum readable size.
        if (comp.value("responsive_fit", false))
            return std::max(0.10f, std::min(comp.value("max_scale", 1.5f), raw));
        const float min_scale = comp.value("min_scale", 0.55f);
        const float max_scale = comp.value("max_scale", 1.5f);
        return std::max(min_scale, std::min(max_scale, raw));
    };

    auto resolve = [&](const Entity& comp) -> SDL_Rect {
        float ax=comp.value("anchor_x",0.5f), ay=comp.value("anchor_y",0.5f);
        float px=comp.value("pivot_x",0.5f),  py=comp.value("pivot_y",0.5f);
        const float scale = responsive_scale(comp);
        int w=std::max(1, (int)std::lround(comp.value("width",200) * scale));
        int h=std::max(1, (int)std::lround(comp.value("height",40) * scale));
        int x=(int)std::lround(ax*sw + comp.value("pos_x",0.f) * scale - px*w);
        int y=(int)std::lround(ay*sh + comp.value("pos_y",0.f) * scale - py*h);
        return {x, y, w, h};
    };

    auto col_of = [&](const std::vector<int>& col, float def_a=1.f) -> std::array<float,4> {
        float r=(col.size()>0?col[0]:255)/255.f;
        float g=(col.size()>1?col[1]:255)/255.f;
        float b=(col.size()>2?col[2]:255)/255.f;
        float a=(col.size()>3?col[3]/255.f:def_a);
        return {r,g,b,a};
    };

    for (auto* ep : ui_ents) {
        auto& e = *ep;

        // ── UIPanel ──────────────────────────────────────────────────────────
        if (has_component(e,"UIPanel")) {
            auto& c=e["components"]["UIPanel"];
            SDL_Rect r=resolve(c);
            std::string panel_tex_name = c.value("texture",std::string(""));
            vkr::Texture* panel_tex = panel_tex_name.empty() ? nullptr : _textures->get(panel_tex_name);
            if (panel_tex) {
                auto col = c.value("color", std::vector<int>{255,255,255,255});
                std::array<float,4> tint = col_of(col);
                if (c.value("draw_mode", std::string("simple")) == "sliced") {
                    draw_nineslice(panel_tex, r,
                        c.value("border_left",0), c.value("border_right",0),
                        c.value("border_top",0),  c.value("border_bottom",0),
                        true, tint);
                } else {
                    push_textured_rect(panel_tex, 0,0,1,1, (float)r.x,(float)r.y,(float)r.w,(float)r.h, tint);
                }
            } else {
                push_fill_rect((float)r.x,(float)r.y,(float)r.w,(float)r.h,
                               col_of(c.value("color",std::vector<int>{30,30,40,200}), 200.f/255.f));
            }
            if (c.contains("border_color")) {
                std::array<float,4> bc = col_of(c["border_color"].get<std::vector<int>>());
                int bw=std::max(1, (int)std::lround(c.value("border_width",1) * responsive_scale(c)));
                push_draw_rect((float)r.x,(float)r.y,(float)r.w,(float)r.h, bc, bw);
            }
        }

        // ── UIProgressBar ───────────────────────────────────────────────────
        if (has_component(e,"UIProgressBar")) {
            auto& c=e["components"]["UIProgressBar"];
            SDL_Rect r=resolve(c);
            push_fill_rect((float)r.x,(float)r.y,(float)r.w,(float)r.h,
                           col_of(c.value("bg_color",std::vector<int>{30,30,30,255})));
            float mn=c.value("min",0.f),mx=c.value("max",1.f),val=c.value("value",0.5f);
            float t=(mx>mn)?(val-mn)/(mx-mn):0.f;
            int fw=std::max(0,(int)(r.w*std::max(0.f,std::min(1.f,t))));
            push_fill_rect((float)r.x,(float)r.y,(float)fw,(float)r.h,
                           col_of(c.value("fill_color",std::vector<int>{80,200,80,255})));
            push_draw_rect((float)r.x,(float)r.y,(float)r.w,(float)r.h, {100/255.f,100/255.f,100/255.f,1.f});
        }

        // ── UIButton ─────────────────────────────────────────────────────────
        if (has_component(e,"UIButton")) {
            auto& c=e["components"]["UIButton"];
            SDL_Rect r=resolve(c);
            bool hover=(mouse_x>=r.x&&mouse_x<r.x+r.w&&mouse_y>=r.y&&mouse_y<r.y+r.h);
            auto col=hover?(mouse_down?c.value("pressed_color",std::vector<int>{50,50,90,255})
                                      :c.value("hover_color",std::vector<int>{110,110,180,255}))
                          :c.value("normal_color",std::vector<int>{80,80,120,255});
            push_fill_rect((float)r.x,(float)r.y,(float)r.w,(float)r.h, col_of(col));
            push_draw_rect((float)r.x,(float)r.y,(float)r.w,(float)r.h, {150/255.f,150/255.f,200/255.f,1.f});
            if (hover && mouse_just_down) {
                // Keep the existing polling surface for native scripts, but
                // also enqueue a real event.  The latter lets entity-owned
                // Visual Scripts receive On UI Click reliably on the next
                // update, after the render pass has performed hit-testing.
                const std::string action = c.value("on_click", std::string());
                e["_ui_clicked"] = action;
                if (!e.contains("_pending_events") || !e["_pending_events"].is_array())
                    e["_pending_events"] = Entity::array();
                Entity event = Entity::object();
                event["method"] = "on_ui_click";
                event["action"] = action;
                e["_pending_events"].push_back(std::move(event));
            }
            // Draw button label — real TTF atlas when loaded (see
            // vk_font_atlas.hpp), falls back to the built-in pixel font
            // automatically (draw_text_atlas/measure_text_atlas both check
            // has_font_atlas() internally) if no font could be loaded.
            std::string label=c.value("text",std::string(""));
            if (!label.empty()) {
                float fsize=(float)c.value("font_size",20) * responsive_scale(c);
                float tw=measure_text_atlas(label,fsize), th=fsize;
                float tx=r.x+(r.w-tw)/2.f, ty=r.y+(r.h-th)/2.f;
                draw_text_atlas(label,tx,ty,fsize,{1,1,1,1});
            }
        }

        // ── UISlider ─────────────────────────────────────────────────────────
        if (has_component(e,"UISlider")) {
            auto& c=e["components"]["UISlider"];
            SDL_Rect r=resolve(c);
            int th=std::max(4,r.h/3);
            push_fill_rect((float)r.x,(float)(r.y+r.h/2-th/2),(float)r.w,(float)th, {60/255.f,60/255.f,60/255.f,1.f});
            float mn=c.value("min",0.f),mx=c.value("max",1.f),val=c.value("value",0.5f);
            float t=(mx>mn)?(val-mn)/(mx-mn):0.f;
            float hx=r.x+r.w*t, hy=(float)(r.y+r.h/2);
            float hr=(float)(r.h/2);
            push_fill_circle(hx,hy,hr, {200/255.f,200/255.f,200/255.f,1.f});
        }

        // ── UIImage ──────────────────────────────────────────────────────────
        if (has_component(e,"UIImage")) {
            auto& c=e["components"]["UIImage"];
            SDL_Rect r=resolve(c);
            // Accept either "sprite" (legacy runtime field) or "texture"
            // (Inspector field name), same fallback as the SDL2 build.
            std::string src=c.value("sprite",std::string(""));
            if (src.empty()) src=c.value("texture",std::string(""));
            vkr::Texture* tex = src.empty() ? nullptr : _textures->get(src);
            if (tex) {
                auto col=c.value("color",std::vector<int>{255,255,255,255});
                std::array<float,4> tint = col_of(col);
                if (c.value("draw_mode", std::string("simple")) == "sliced") {
                    draw_nineslice(tex, r,
                        c.value("border_left",0), c.value("border_right",0),
                        c.value("border_top",0),  c.value("border_bottom",0),
                        true, tint);
                } else {
                    push_textured_rect(tex, 0,0,1,1, (float)r.x,(float)r.y,(float)r.w,(float)r.h, tint);
                }
            } else {
                push_fill_rect((float)r.x,(float)r.y,(float)r.w,(float)r.h,
                               col_of(c.value("color",std::vector<int>{255,255,255,80}), 80.f/255.f));
                push_draw_rect((float)r.x,(float)r.y,(float)r.w,(float)r.h, {200/255.f,200/255.f,200/255.f,180/255.f});
            }
        }

        // ── UIText ───────────────────────────────────────────────────────────
        if (has_component(e,"UIText")) {
            auto& c=e["components"]["UIText"];
            SDL_Rect r=resolve(c);
            std::string text=c.value("text",std::string(""));
            if (text.empty()) continue;
            auto col=c.value("color",std::vector<int>{255,255,255,255});
            std::array<float,4> tc = col_of(col);

            float fsize=(float)c.value("font_size",16) * responsive_scale(c);
            float lh=text_line_height_atlas(fsize);
            const float pad_l = (float)c.value("padding_left", c.value("padding", 0));
            const float pad_r = (float)c.value("padding_right", c.value("padding", 0));
            const float pad_t = (float)c.value("padding_top", c.value("padding", 0));
            const float pad_b = (float)c.value("padding_bottom", c.value("padding", 0));
            const float content_x = (float)r.x + pad_l;
            const float content_w = std::max(1.f, (float)r.w - pad_l - pad_r);
            const float content_h = std::max(1.f, (float)r.h - pad_t - pad_b);
            const bool word_wrap = c.value("word_wrap", false);

            // Build every visual line before drawing.  Apart from making
            // vertical alignment correct, this avoids the old stream-as-you-
            // go behaviour where a wrapped title could draw part of itself
            // outside a panel before the renderer noticed it had run out of
            // vertical space.
            std::vector<std::string> lines;
            auto append_wrapped = [&](const std::string& source) {
                if (!word_wrap || content_w <= 0.f) { lines.push_back(source); return; }
                std::string line;
                size_t cursor = 0;
                while (cursor <= source.size()) {
                    const size_t space = source.find(' ', cursor);
                    const std::string word = source.substr(cursor,
                        space == std::string::npos ? std::string::npos : space - cursor);
                    const std::string candidate = line.empty() ? word : line + " " + word;
                    if (!line.empty() && measure_text_atlas(candidate, fsize) > content_w) {
                        lines.push_back(line);
                        line = word;
                    } else if (line.empty() && measure_text_atlas(word, fsize) > content_w) {
                        // Long identifiers and paths are broken at a glyph
                        // boundary rather than bleeding into another card.
                        std::string fragment;
                        for (char ch : word) {
                            const std::string next = fragment + ch;
                            if (!fragment.empty() && measure_text_atlas(next, fsize) > content_w) {
                                lines.push_back(fragment);
                                fragment.assign(1, ch);
                            } else {
                                fragment = next;
                            }
                        }
                        line = fragment;
                    } else {
                        line = candidate;
                    }
                    if (space == std::string::npos) break;
                    cursor = space + 1;
                }
                if (!line.empty() || source.empty()) lines.push_back(line);
            };
            size_t pos = 0, found = 0;
            while ((found = text.find('\n', pos)) != std::string::npos) {
                append_wrapped(text.substr(pos, found - pos));
                pos = found + 1;
            }
            append_wrapped(text.substr(pos));

            const int visible_lines = std::min((int)lines.size(),
                std::max(0, (int)std::floor((content_h + 0.5f) / std::max(1.f, lh))));
            if (visible_lines <= 0) continue;
            const float block_h = visible_lines * lh;
            const std::string v_align = c.value("v_align", c.value("vertical_alignment", std::string("top")));
            float cy = (float)r.y + pad_t;
            if (v_align == "middle" || v_align == "center") cy += std::max(0.f, (content_h - block_h) * 0.5f);
            else if (v_align == "bottom") cy += std::max(0.f, content_h - block_h);

            const std::string align=c.value("align",c.value("alignment",std::string("left")));
            for (int i = 0; i < visible_lines; ++i) {
                const float tw = measure_text_atlas(lines[(size_t)i], fsize);
                float tx = content_x;
                if (align == "center") tx = content_x + (content_w - tw) * 0.5f;
                else if (align == "right") tx = content_x + content_w - tw;
                draw_text_atlas(lines[(size_t)i], tx, cy, fsize, tc);
                cy += lh;
            }
        }
    }

    // SceneTransition is deliberately drawn after all UI so the outgoing
    // scene fades as one coherent frame.  The transition system keeps alpha
    // and RGB on the component; consuming it here makes the inspector's
    // duration/colour controls visible in both standalone and editor Play.
    float transition_alpha = 0.f;
    std::array<float,4> transition_color{0.f, 0.f, 0.f, 0.f};
    for (auto& e : entities) {
        if (!entity_active(e) || !has_component(e, "SceneTransition")) continue;
        const auto& transition = e["components"]["SceneTransition"];
        const float alpha = std::clamp(transition.value("_alpha", 0.f), 0.f, 1.f);
        if (alpha <= transition_alpha) continue;
        transition_alpha = alpha;
        transition_color = {
            std::clamp(transition.value("fade_r", 0), 0, 255) / 255.f,
            std::clamp(transition.value("fade_g", 0), 0, 255) / 255.f,
            std::clamp(transition.value("fade_b", 0), 0, 255) / 255.f,
            alpha
        };
    }
    if (transition_alpha > 0.f)
        push_fill_rect(0.f, 0.f, (float)sw, (float)sh, transition_color);
}
