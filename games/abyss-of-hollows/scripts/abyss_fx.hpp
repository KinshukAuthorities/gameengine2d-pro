#pragma once
// abyss_fx.hpp — shared "juice" toolkit: particle bursts, trails, hit-sparks,
// and small UI-punch helpers, usable from every script without touching the
// engine. Everything here builds plain Entity JSON at runtime and either
// Instantiate()s a short-lived burst object (driven by AbyssFxBurst, see
// game_scripts.hpp) or calls Debug::draw_line for free-floating trail
// segments that fade on their own (engine already ages these out).
//
// Nothing here needs scene authoring: SpawnBurst builds a self-contained
// ParticleEmitter entity in code and instantiates it directly, so no prefab
// has to exist in any .json scene file.

#include "../../../engine_cpp/script_system.hpp"
#include <algorithm>
#include <cmath>

namespace AbyssFx {

struct Color { int r, g, b, a; };

inline Entity BuildBurstTemplate(float rate, float lifetime, float speed, float spread_deg,
                                  float size_start, float size_end,
                                  Color c_start, Color c_end, float burst_life) {
    Entity tpl = Entity::object();
    tpl["name"] = string("FxBurst(Clone)");
    tpl["active"] = true;
    tpl["tags"] = Entity::array();
    tpl["components"] = Entity::object();

    Entity transform_comp = Entity::object();
    transform_comp["x"] = 0.f;
    transform_comp["y"] = 0.f;
    transform_comp["rotation"] = 0.f;
    transform_comp["scale_x"] = 1.f;
    transform_comp["scale_y"] = 1.f;
    tpl["components"]["Transform"] = transform_comp;

    Entity emitter = Entity::object();
    // One-shot bursts are bounded at the source.  The older helper used a
    // looping rate emitter and relied on a script callback to stop it; if a
    // hot reload interrupted that callback, every combat spark kept emitting
    // forever.  A non-looping burst is self-contained in ParticleSystem and
    // remains finite even while scripts are being swapped.
    emitter["emitting"] = true;
    emitter["looping"] = false;
    emitter["burst"] = true;
    emitter["burst_count"] = std::max(1, std::min(96, (int)std::lround(rate * std::max(0.025f, burst_life))));
    emitter["max_particles"] = 128;
    emitter["rate"] = rate;
    emitter["spread"] = spread_deg;
    emitter["speed"] = speed;
    emitter["speed_variation"] = 0.34f;
    emitter["lifetime"] = lifetime;
    emitter["lifetime_variation"] = 0.18f;
    emitter["gravity_scale"] = 0.08f;
    emitter["size_start"] = size_start;
    emitter["size_end"] = size_end;

    Entity cs = Entity::array();
    cs.push_back(c_start.r); cs.push_back(c_start.g); cs.push_back(c_start.b); cs.push_back(c_start.a);
    Entity ce = Entity::array();
    ce.push_back(c_end.r); ce.push_back(c_end.g); ce.push_back(c_end.b); ce.push_back(c_end.a);
    emitter["color_start"] = cs;
    emitter["color_end"] = ce;
    tpl["components"]["ParticleEmitter"] = emitter;

    Entity script_comp = Entity::object();
    Entity script_list = Entity::array();
    script_list.push_back(string("abyss_fx_burst"));
    script_comp["scripts"] = script_list;
    tpl["components"]["ScriptComponent"] = script_comp;

    tpl["fx_burst_life"] = burst_life;
    tpl["_fx_transient"] = true;
    // Script reload can temporarily leave the burst behaviour unavailable.
    // The engine timer is therefore the authoritative cleanup path.
    tpl["_destroy_timer"] = std::max(0.05f, burst_life + lifetime + 0.08f);
    return tpl;
}

template <class T>
inline void SpawnBurst(T self, float x, float y,
                        float rate, float lifetime, float speed, float spread_deg,
                        float size_start, float size_end,
                        Color c_start, Color c_end, float burst_life = 0.12f) {
    if (!self) return;
    Entity tpl = BuildBurstTemplate(rate, lifetime, speed, spread_deg, size_start, size_end,
                                     c_start, c_end, burst_life);
    self->Instantiate(tpl, x, y);
}

template <class T>
inline void HitSpark(T self, float x, float y, Color tint, float power = 1.0f) {
    SpawnBurst(self, x, y,
               90.0f * power, 0.22f, 260.0f * power,
               360.0f, 7.0f, 0.0f,
               tint, Color{tint.r, tint.g, tint.b, 0}, 0.07f);
}

template <class T>
inline void Dust(T self, float x, float y, float power = 1.0f) {
    SpawnBurst(self, x, y,
               70.0f * power, 0.28f, 90.0f * power, 140.0f,
               10.0f, 1.0f,
               Color{210, 200, 190, 160}, Color{170, 160, 150, 0}, 0.05f);
}

template <class T>
inline void Explosion(T self, float x, float y, Color tint, float power = 1.0f) {
    SpawnBurst(self, x, y,
               160.0f * power, 0.4f, 380.0f * power, 360.0f,
               14.0f * power, 0.0f,
               tint, Color{tint.r, tint.g, tint.b, 0}, 0.09f);
    SpawnBurst(self, x, y,
               40.0f * power, 0.6f, 120.0f * power, 360.0f,
               6.0f, 0.0f,
               Color{255, 255, 255, 220}, Color{255, 255, 255, 0}, 0.05f);
}

template <class T>
inline void EnergyPuff(T self, float x, float y, Color tint) {
    SpawnBurst(self, x, y,
               60.0f, 0.3f, 150.0f, 360.0f,
               8.0f, 0.0f,
               tint, Color{tint.r, tint.g, tint.b, 0}, 0.05f);
}

// ── Trails (free, no entities) ──────────────────────────────────────────
// Debug::draw_line already supports a fade duration and is drawn in world
// space every frame by the renderer, so it's a perfect zero-cost trail/
// streak primitive — call this once per frame from Update() with the
// current + previous position to leave a fading ribbon segment behind.
inline void TrailSegment(float x1, float y1, float x2, float y2,
                          Color c, float duration = 0.12f, int thickness = 1) {
    for (int i = 0; i < thickness; ++i) {
        float off = (thickness > 1) ? (i - (thickness - 1) * 0.5f) : 0.0f;
        Debug::draw_line(x1, y1 + off, x2, y2 + off,
                          (Uint8)c.r, (Uint8)c.g, (Uint8)c.b, (Uint8)c.a, duration);
    }
}

// Multi-segment afterimage "fan" — used for the sword arc / dash streak.
// Draws several short fading lines radiating slightly so a single-frame
// call still reads as a solid streak rather than a thin line.
inline void Streak(float x, float y, float dir_x, float dir_y, float length,
                    Color c, float duration = 0.1f) {
    float len = sqrt(dir_x*dir_x + dir_y*dir_y);
    if (len < 0.0001f) return;
    dir_x /= len; dir_y /= len;
    float px = -dir_y, py = dir_x;
    for (int i = -2; i <= 2; ++i) {
        float t = i * 0.18f;
        float ox = px * t * length * 0.5f;
        float oy = py * t * length * 0.5f;
        Debug::draw_line(x + ox, y + oy,
                          x + ox - dir_x * length, y + oy - dir_y * length,
                          (Uint8)c.r, (Uint8)c.g, (Uint8)c.b,
                          (Uint8)max(0, c.a - abs(i) * 35), duration);
    }
}

} // namespace AbyssFx
