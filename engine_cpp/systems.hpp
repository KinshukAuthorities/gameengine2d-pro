#pragma once
/*
 * systems.hpp — C++ ports of AnimatorSystem, ParticleSystem, AudioSystem,
 *               ScriptSystem, UISystem from systems.py / ui_system.py
 */

#include "entity.hpp"
#include "camera.hpp"
#include "input_system.hpp"
#include "determinism.hpp"
#include <SDL2/SDL.h>
#ifndef NO_SDL_MIXER
#include <SDL2/SDL_mixer.h>
#endif
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cmath>
#include <iostream>
#include <random>
#include <functional>
#include <algorithm>
#include <mutex>

// ─── AnimatorSystem ───────────────────────────────────────────────────────────
// C++ port of AnimatorSystem.update() from systems.py, extended with:
//   - a transition graph (Unity-style: from/to states gated by parameter
//     conditions and/or exit time, with a transition duration)
//   - 1D and 2D blend trees (Unity-style: a virtual "state" whose clip is
//     chosen/blended from child clips based on one or two float parameters)
//
// Honesty note on "blending": this renderer draws exactly one texture per
// SpriteRenderer per frame (see RenderSystem::_draw_sprite) — there is no
// second sprite layer to alpha-dissolve against. So:
//   - Blend trees pick the single closest child clip by parameter distance
//     (nearest-neighbor) rather than true inter-frame interpolation. This
//     matches the visual result Unity blend trees are usually used for in
//     2D (e.g. snapping between walk/run frame sets based on speed) even
//     though it isn't continuous frame interpolation.
//   - Transitions are a timed, deterministic switch to the target state
//     once the transition's `duration` has elapsed (or instantly if
//     duration is 0) — not a cross-dissolve. `duration` still gates *when*
//     the switch completes, so scripts/animations relying on "this
//     transition takes 0.2s" behave correctly; what doesn't happen is two
//     sprites visually blending mid-transition.
class AnimatorSystem {
public:
    void update(EntityList& entities, float dt) {
        for (auto& e : entities) {
            if (!entity_active(e)) continue;
            auto* anim_ptr = has_component(e,"Animator") ? &e["components"]["Animator"] : nullptr;
            if (!anim_ptr) continue;
            auto& anim = *anim_ptr;

            _evaluate_transitions(anim, dt);

            if (!anim.value("playing",false)) continue;

            std::string cur = _resolve_state(anim);
            Entity inline_override_clip;
            Entity* clip = _resolve_clip_with_override(e, anim, cur, inline_override_clip);

            float fps   = anim.value("default_fps",12.f) * anim.value("speed",1.f) * anim.value("speed_multiplier",1.f);
            bool  loop  = anim.value("loop",true);
            bool  pp    = anim.value("ping_pong",false);
            Entity frames = Entity::array();

            if (clip) {
                if (clip->is_object()) {
                    frames = clip->value("frames", clip->value("textures", Entity::array()));
                    fps    = clip->value("fps", fps);
                    loop   = clip->value("loop", loop);
                    pp     = clip->value("ping_pong", pp);
                } else if (clip->is_array()) {
                    frames = *clip;
                }
            }

            // Sprite sheet mode
            if (anim.value("use_sprite_sheet",false)) {
                std::string sheet = anim.value("sprite_sheet","");
                if (sheet.empty() && !frames.empty() && frames[0].is_string())
                    sheet = frames[0].get<std::string>();
                if (sheet.empty()) continue;
                int fw = anim.value("frame_width",0);
                int fh = anim.value("frame_height",0);
                int cols = std::max(1,anim.value("sheet_columns",1));
                int rows = std::max(1,anim.value("sheet_rows",1));
                if (fw<=0||fh<=0) continue;
                int total = std::max(1,cols*rows);
                if (frames.empty()) { frames=Entity::array(); for(int i=0;i<total;++i) frames.push_back(i); }

                anim["frame"] = anim.value("frame",0.f) + std::max(0.f,fps)*dt;
                int raw = (int)anim["frame"].get<float>();
                int idx = _frame_index(raw, (int)frames.size(), loop, pp, anim);
                int frame_num = frames[idx].is_number() ? frames[idx].get<int>() : idx;
                int sp  = anim.value("sheet_spacing",0);
                int pad = anim.value("sheet_padding",0);
                int sx = (frame_num % cols) * (fw+sp) + pad;
                int sy = (frame_num / cols) * (fh+sp) + pad;
                auto& sr = e["components"]["SpriteRenderer"];
                sr["texture"]        = sheet;
                sr["use_source_rect"]= true;
                sr["source_x"]       = sx;
                sr["source_y"]       = sy;
                sr["source_w"]       = fw;
                sr["source_h"]       = fh;
                if (clip && clip->is_object()) _fire_clip_events(e, anim, *clip, cur, idx);
                continue;
            }

            if (frames.empty()) continue;
            anim["frame"] = anim.value("frame",0.f) + std::max(0.f,fps)*dt;
            int raw = (int)anim["frame"].get<float>();
            int idx = _frame_index(raw, (int)frames.size(), loop, pp, anim);
            std::string frame_name = frames[idx].is_string() ? frames[idx].get<std::string>() : "";
            auto& sr = e["components"]["SpriteRenderer"];
            sr["texture"]        = frame_name;
            sr["use_source_rect"]= false;
            sr["source_w"]       = 0;
            sr["source_h"]       = 0;

            if (clip && clip->is_object()) _fire_clip_events(e, anim, *clip, cur, idx);
        }
        _evaluate_layers(entities, dt);
    }

    // Sets SpriteRenderer's source_rect to ONE static frame (whatever
    // anim["frame"] currently holds, or frame 0 if the clip has never
    // played yet) for every animated entity — WITHOUT advancing time,
    // evaluating transitions, or firing animation events. This exists
    // purely so the editor's scene view can show a normal-looking single
    // sprite while not playing.
    //
    // Without this, a sprite-sheet-mode Animator's SpriteRenderer never
    // gets use_source_rect/source_x/source_y applied at all outside Play
    // (those are only ever written by the dt-driven block above, which is
    // skipped whenever anim["playing"] is false — and editor browsing
    // never sets that to true). RenderSystem::_draw_sprite then falls back
    // to drawing the texture at its full raw pixel size — which, for a
    // multi-frame horizontal spritesheet like crawler_abyss.png (256x64 =
    // four 64x64 frames), looks exactly like four copies of the enemy
    // standing in a row instead of one animated sprite. This call corrects
    // that by cropping to a single frame even while fully static/paused.
    void apply_static_frame(EntityList& entities) {
        for (auto& e : entities) {
            if (!entity_active(e)) continue;
            if (!has_component(e,"Animator")) continue;
            auto& anim = e["components"]["Animator"];
            if (!anim.value("use_sprite_sheet",false)) continue; // non-sheet mode already stores a real per-frame texture name; nothing to crop

            std::string sheet = anim.value("sprite_sheet","");
            std::string cur = _resolve_state(anim);
            Entity inline_override_clip;
            Entity* clip = _resolve_clip_with_override(e, anim, cur, inline_override_clip);
            Entity frames = Entity::array();
            if (clip) {
                if (clip->is_object()) frames = clip->value("frames", clip->value("textures", Entity::array()));
                else if (clip->is_array()) frames = *clip;
            }
            if (sheet.empty() && !frames.empty() && frames[0].is_string()) sheet = frames[0].get<std::string>();
            if (sheet.empty()) continue;

            int fw = anim.value("frame_width",0);
            int fh = anim.value("frame_height",0);
            if (fw<=0||fh<=0) continue;
            int cols = std::max(1,anim.value("sheet_columns",1));
            int total = std::max(1,cols*std::max(1,anim.value("sheet_rows",1)));
            if (frames.empty()) { frames=Entity::array(); for(int i=0;i<total;++i) frames.push_back(i); }

            // Use whatever frame index playback last stopped on (so pausing
            // mid-animation, or stopping after a one-shot clip, still shows
            // that exact frame rather than always snapping back to 0) — but
            // clamp into range in case the clip/frame-count changed since.
            int raw = (int)anim.value("frame",0.f);
            int idx = ((raw % (int)frames.size()) + (int)frames.size()) % std::max(1,(int)frames.size());
            int frame_num = frames[idx].is_number() ? frames[idx].get<int>() : idx;
            int sp  = anim.value("sheet_spacing",0);
            int pad = anim.value("sheet_padding",0);
            int sx = (frame_num % cols) * (fw+sp) + pad;
            int sy = (frame_num / cols) * (fh+sp) + pad;

            auto& sr = e["components"]["SpriteRenderer"];
            sr["texture"]         = sheet;
            sr["use_source_rect"] = true;
            sr["source_x"]        = sx;
            sr["source_y"]        = sy;
            sr["source_w"]        = fw;
            sr["source_h"]        = fh;
        }
    }

private:
    // Animator Override Controller maps an Animator state to either another
    // named clip in the same controller or an inline replacement clip.  Keep
    // this selection transient: the base controller remains untouched, so
    // removing an override instantly restores the original animation.
    static Entity* _resolve_clip_with_override(Entity& entity, Entity& anim,
                                                std::string& current_state,
                                                Entity& inline_clip) {
        auto& animations = anim["animations"];
        Entity* clip = (!current_state.empty() && animations.contains(current_state))
            ? &animations[current_state] : nullptr;
        if (!has_component(entity, "AnimatorOverrideController")) return clip;
        const auto& controller = entity["components"]["AnimatorOverrideController"];
        if (!controller.contains("overrides") || !controller["overrides"].is_object() ||
            !controller["overrides"].contains(current_state)) return clip;
        const auto& replacement = controller["overrides"][current_state];
        if (replacement.is_string()) {
            const std::string replacement_name = replacement.get<std::string>();
            if (animations.contains(replacement_name)) {
                current_state = replacement_name;
                return &animations[replacement_name];
            }
            return clip;
        }
        if (replacement.is_array() || replacement.is_object()) {
            inline_clip = replacement;
            return &inline_clip;
        }
        return clip;
    }

    // Queues ScriptBase::on_animation_event(name) (via the same
    // _pending_events mechanism collision/trigger callbacks use — see
    // ScriptSystem::update) for every event attached to `frameIdx` on this
    // clip, the frame AnimatorSystem just advanced onto. Guards against
    // re-firing every frame while held on a non-looping last frame, or
    // (for looping/ping-pong clips) re-firing on every repeated visit to the
    // same index within one continuous play — only a genuine *new* arrival
    // at that index (tracked per clip name) fires it.
    void _fire_clip_events(Entity& e, Entity& anim, Entity& clip, const std::string& clipName, int frameIdx) {
        if (!clip.contains("events") || !clip["events"].is_array() || clip["events"].empty()) return;

        std::string lastKey = "_last_event_frame__" + clipName;
        int lastIdx = anim.value(lastKey, -1);
        anim[lastKey] = frameIdx;
        if (lastIdx == frameIdx) return; // already fired for this index

        for (auto& ev : clip["events"]) {
            if (ev.value("frame", -1) != frameIdx) continue;
            std::string name = ev.value("name", std::string());
            if (name.empty()) continue;
            if (!e.contains("_pending_events") || !e["_pending_events"].is_array())
                e["_pending_events"] = Entity::array();
            Entity pending = Entity::object();
            pending["method"] = "on_animation_event";
            pending["event_name"] = name;
            e["_pending_events"].push_back(pending);
        }
    }

    // ── Override layers ─────────────────────────────────────────────────────
    // Advances every authored layer (anim["layers"], written by
    // Animator::AddLayer/PlayOnLayer) as its own independent flipbook state
    // machine — same frame-advance/loop/ping-pong rules as the base layer,
    // just without transitions/blend trees, which only apply to the base
    // state. Every frame, the highest-index layer with weight > 0 and a
    // non-empty current_animation wins and overwrites the SpriteRenderer the
    // base layer just wrote (Unity "override" blend mode — see the Animator
    // class header comment in unity2d_script_api.hpp for why this engine
    // can't do true mask-based compositing of two animations).
    void _evaluate_layers(EntityList& entities, float dt) {
        for (auto& e : entities) {
            if (!entity_active(e)) continue;
            if (!has_component(e,"Animator")) continue;
            auto& anim = e["components"]["Animator"];
            if (!anim.contains("layers") || !anim["layers"].is_array() || anim["layers"].empty()) continue;

            Entity* winner = nullptr;
            for (auto& layer : anim["layers"]) {
                if (!layer.is_object()) continue;
                float weight = layer.value("weight", 1.f);
                std::string lcur = layer.value("current_animation", std::string());
                if (!layer.value("playing", false) || lcur.empty()) continue;

                Entity* lclip = anim.contains("animations") && anim["animations"].contains(lcur)
                                 ? &anim["animations"][lcur] : nullptr;
                Entity lframes = Entity::array();
                float lfps = anim.value("default_fps", 12.f);
                bool lloop = true, lpp = false;
                if (lclip) {
                    if (lclip->is_object()) {
                        lframes = lclip->value("frames", lclip->value("textures", Entity::array()));
                        lfps = lclip->value("fps", lfps);
                        lloop = lclip->value("loop", lloop);
                        lpp = lclip->value("ping_pong", lpp);
                    } else if (lclip->is_array()) {
                        lframes = *lclip;
                    }
                }
                if (lframes.empty()) continue;

                layer["frame"] = layer.value("frame", 0.f) + std::max(0.f, lfps) * dt;
                int raw = (int)layer["frame"].get<float>();
                int idx = _frame_index(raw, (int)lframes.size(), lloop, lpp, layer);
                std::string fname = lframes[idx].is_string() ? lframes[idx].get<std::string>() : "";
                if (fname.empty()) continue;

                if (lclip && lclip->is_object()) _fire_clip_events(e, anim, *lclip, lcur, idx);

                // Highest-index layer with weight > 0 wins; keep scanning so
                // a later (higher) layer can still override an earlier one.
                if (weight > 0.f) {
                    layer["_resolved_texture"] = fname;
                    winner = &layer;
                }
            }
            if (winner) {
                auto& sr = e["components"]["SpriteRenderer"];
                sr["texture"] = winner->value("_resolved_texture", std::string());
                sr["use_source_rect"] = false;
                sr["source_w"] = 0;
                sr["source_h"] = 0;
            }
        }
    }
    int _frame_index(int raw, int n, bool loop, bool pp, Entity& anim) {
        if (pp && n>1) {
            int cycle = n*2-2;
            int idx = raw % cycle;
            if (idx>=n) idx=cycle-idx;
            return std::max(0,std::min(n-1,idx));
        }
        if (loop) return raw % n;
        int idx = std::min(n-1, raw);
        if (idx>=n-1) anim["playing"]=false;
        return idx;
    }

    // ── Transition graph ────────────────────────────────────────────────────
    // anim["transitions"] = [ { from, to, duration, has_exit_time, exit_time,
    //                            conditions: [ {param, op, value} ... ] } ]
    // `from` may be "*" / "Any" to mean "from any state" (Unity's "Any State").
    // All listed conditions must hold (AND) for the transition to fire. Param
    // ops: "trigger" (bool-trigger-style, value ignored, auto-consumed),
    // "greater", "less", "equals", "notequal", "bool_true", "bool_false".
    // A transition with no conditions and has_exit_time fires once normalized
    // time reaches exit_time (Unity's exit-time-only transition).
    void _evaluate_transitions(Entity& anim, float dt) {
        if (!anim.contains("transitions") || !anim["transitions"].is_array()) return;
        if (!anim.contains("parameters") || anim["parameters"].is_null()) anim["parameters"] = Entity::object();
        auto& params = anim["parameters"];

        // Advance an in-flight transition's timer; once it reaches duration,
        // commit the switch to `to` (resetting frame) and clear it.
        bool mid_transition = false;
        if (anim.contains("_transition") && anim["_transition"].is_object() && !anim["_transition"].is_null()) {
            auto& tr = anim["_transition"];
            float t = tr.value("t",0.f) + dt;
            float dur = tr.value("duration",0.f);
            if (t >= dur) {
                std::string to = tr.value("to", std::string());
                anim["current_animation"] = to;
                anim["frame"] = 0.f;
                anim["playing"] = true;
                anim["_transition"] = nullptr;
            } else {
                tr["t"] = t;
                // Block new transitions unless the in-flight one allows interruption.
                if (!tr.value("can_interrupt", false)) mid_transition = true;
            }
        }

        std::string cur = _resolve_state(anim);
        float norm_t = _normalized_time(anim, cur);

        for (auto& t : anim["transitions"]) {
            std::string from = t.value("from", std::string("*"));
            bool is_any_state = (from == "*" || from == "Any" || from == "AnyState");
            // Mid-transition: only Any State transitions marked can_interrupt may fire
            if (mid_transition && !(is_any_state && t.value("can_interrupt", false))) continue;
            if (!is_any_state && from != cur) continue;
            std::string to = t.value("to", std::string());
            if (to.empty() || to == cur) continue;

            bool conditions_ok = true;
            bool consumed_any_trigger = false;
            if (t.contains("conditions") && t["conditions"].is_array() && !t["conditions"].empty()) {
                for (auto& c : t["conditions"]) {
                    if (!_check_condition(params, c, consumed_any_trigger)) { conditions_ok = false; break; }
                }
            } else if (!t.value("has_exit_time", false)) {
                // No conditions and no exit-time configured: never auto-fires
                // (Unity requires at least one of these to leave a state).
                conditions_ok = false;
            }

            bool exit_ok = true;
            if (t.value("has_exit_time", false)) {
                exit_ok = norm_t >= t.value("exit_time", 1.f);
            }

            if (conditions_ok && exit_ok) {
                float duration = std::max(0.f, t.value("duration", 0.f));
                if (duration <= 0.f) {
                    anim["current_animation"] = to;
                    anim["frame"] = 0.f;
                    anim["playing"] = true;
                    anim["_transition"] = nullptr;
                } else {
                    Entity tr = Entity::object();
                    tr["to"] = to;
                    tr["duration"] = duration;
                    tr["t"] = 0.f;
                    tr["can_interrupt"] = t.value("can_interrupt", false);
                    anim["_transition"] = tr;
                }
                return; // one transition per frame, first match wins (Unity order)
            }
        }
    }

    bool _check_condition(Entity& params, Entity& c, bool& consumed_trigger) {
        std::string param = c.value("param", std::string());
        std::string op = c.value("op", std::string("trigger"));
        if (param.empty()) return false;

        if (op == "trigger") {
            std::string key = "__trig_" + param;
            bool fired = params.value(key, false);
            if (fired) { params[key] = false; consumed_trigger = true; }
            return fired;
        }
        if (op == "bool_true")  return params.value(param, false);
        if (op == "bool_false") return !params.value(param, false);

        float pv = params.value(param, 0.f);
        float cv = c.value("value", 0.f);
        if (op == "greater")  return pv >  cv;
        if (op == "less")     return pv <  cv;
        if (op == "equals")   return pv == cv;
        if (op == "notequal") return pv != cv;
        return false;
    }

    // ── Blend trees ─────────────────────────────────────────────────────────
    // anim["blend_trees"][stateName] = {
    //   type: "1d" | "2d",
    //   param_x: "Speed", param_y: "Direction" (2d only),
    //   children: [ {clip: "Walk", threshold: 0.5} ]                  (1d)
    //           | [ {clip: "WalkN", pos_x: 0, pos_y: 1} ]              (2d)
    // }
    // Resolves to the nearest child's clip name by parameter distance. If
    // `current_animation` names a blend tree, that name is used as the
    // logical "state" for transitions, but the actual clip drawn each frame
    // is whichever child is nearest right now — so blend trees stay reactive
    // to parameter changes without needing their own transition.
    std::string _resolve_state(Entity& anim) {
        std::string cur = anim.value("current_animation",
                          anim.value("autoplay_clip",
                          anim.value("state",std::string(""))));
        if (cur.empty()) return cur;
        if (!anim.contains("blend_trees") || !anim["blend_trees"].contains(cur)) return cur;

        auto& tree = anim["blend_trees"][cur];
        if (!tree.is_object() || !tree.contains("children") || !tree["children"].is_array() || tree["children"].empty())
            return cur;

        auto& params = anim.contains("parameters") ? anim["parameters"] : (anim["parameters"]=Entity::object());
        std::string type = tree.value("type", std::string("1d"));
        std::string best_clip = cur;
        float best_dist = std::numeric_limits<float>::infinity();

        if (type == "2d") {
            float px = params.value(tree.value("param_x", std::string("X")), 0.f);
            float py = params.value(tree.value("param_y", std::string("Y")), 0.f);
            for (auto& ch : tree["children"]) {
                float cx = ch.value("pos_x", 0.f), cy = ch.value("pos_y", 0.f);
                float d = (px-cx)*(px-cx) + (py-cy)*(py-cy);
                if (d < best_dist) { best_dist = d; best_clip = ch.value("clip", cur); }
            }
        } else {
            float pv = params.value(tree.value("param_x", tree.value("param", std::string("X"))), 0.f);
            for (auto& ch : tree["children"]) {
                float thr = ch.value("threshold", 0.f);
                float d = std::abs(pv - thr);
                if (d < best_dist) { best_dist = d; best_clip = ch.value("clip", cur); }
            }
        }
        return best_clip;
    }

    float _normalized_time(Entity& anim, const std::string& stateName) {
        if (stateName.empty() || !anim.contains("animations") || !anim["animations"].contains(stateName)) return 0.f;
        auto& clip = anim["animations"][stateName];
        Entity frames = clip.is_array() ? clip : clip.value("frames", clip.value("textures", Entity::array()));
        int len = (int)frames.size();
        if (len <= 0) return 0.f;
        bool loop = clip.is_object() ? clip.value("loop", anim.value("loop", true)) : anim.value("loop", true);
        bool pp   = clip.is_object() ? clip.value("ping_pong", anim.value("ping_pong", false)) : anim.value("ping_pong", false);
        int raw = (int)anim.value("frame", 0.f);
        if (pp && len > 1) {
            int cycle = len*2-2;
            int idx = raw % cycle; if (idx >= len) idx = cycle - idx;
            return (float)std::max(0, std::min(len-1, idx)) / (float)len;
        }
        if (loop) return (float)(raw % len) / (float)len;
        return (float)std::min(len-1, raw) / (float)len;
    }
};

// ─── ParticleSystem ───────────────────────────────────────────────────────────
// Task 12: curves (size/color over lifetime, 3-4+ keyframe lerp), sub-emitters
// (burst on particle spawn/death), and atlas frame assignment (spritesheet
// particles instead of a flat circle — actual rendering of the atlas frame
// happens in RenderSystem::_draw_particles, which reads the "frame" field
// this system stamps onto each particle).
//
// Curve format (kept intentionally simple — not full Bezier, see task doc):
//   size_curve:  [t0,size0, t1,size1, t2,size2, ...]   (t in [0,1], any count >=2)
//   color_curve: [t0,r0,g0,b0,a0, t1,r1,g1,b1,a1, ...] (same t domain)
// An empty curve array means "fall back to the 2-point start/end fields",
// so existing scenes authored before this change keep working unmodified.
class ParticleSystem {
public:
    // CPU particles are deliberately bounded. Scene JSON and visual graphs are
    // editable at runtime, so accepting an arbitrary max_particles value here
    // can otherwise turn a typo into a multi-second frame or an allocation
    // failure. This is well above the built-in sample effects (which use tens
    // to low thousands of particles).
    static constexpr int kMaxCpuParticles = 16384;
    ParticleSystem() = default;

    // Evaluate a size curve (or 2-point fallback) at normalized time t in [0,1].
    static float eval_size(const Entity& em, float t) {
        if (em.contains("size_curve") && em["size_curve"].is_array() && em["size_curve"].size() >= 4) {
            return _eval_keyframes(em["size_curve"], t, 1);
        }
        float a = em.value("size_start", 10.f);
        float b = em.value("size_end", 0.f);
        return a + (b - a) * t;
    }

    // Evaluate a color curve (or 2-point fallback) at normalized time t in [0,1].
    // Returns RGBA in 0-255 range (matches color_start/color_end convention).
    static std::array<float,4> eval_color(const Entity& em, float t) {
        if (em.contains("color_curve") && em["color_curve"].is_array() && em["color_curve"].size() >= 10) {
            return {_eval_keyframes(em["color_curve"], t, 4),
                    _eval_keyframes(em["color_curve"], t, 4, 1),
                    _eval_keyframes(em["color_curve"], t, 4, 2),
                    _eval_keyframes(em["color_curve"], t, 4, 3)};
        }
        auto cs = em.value("color_start", std::vector<float>{255,255,255,255});
        auto ce = em.value("color_end",   std::vector<float>{255,255,255,0});
        auto at = [](const std::vector<float>& v, size_t i, float def) { return i < v.size() ? v[i] : def; };
        float t0=at(cs,0,255), t1=at(cs,1,255), t2=at(cs,2,255), t3=at(cs,3,255);
        float e0=at(ce,0,255), e1=at(ce,1,255), e2=at(ce,2,255), e3=at(ce,3,0);
        return {t0+(e0-t0)*t, t1+(e1-t1)*t, t2+(e2-t2)*t, t3+(e3-t3)*t};
    }

    void update(EntityList& entities, float dt) {
        for (auto& e : entities) {
            if (!entity_active(e)) continue;
            auto* em = has_component(e,"ParticleEmitter") ? &e["components"]["ParticleEmitter"] : nullptr;
            if (!em) continue;

            // Treat this as a hard runtime budget, not merely a limit for the
            // next emission.  Particle data is editable JSON and can also be
            // written by scripts, so a malformed saved array or a sub-emitter
            // configuration used to let an arbitrarily large array survive
            // forever.  The next update would then spend a multi-second frame
            // walking it before the normal emission guard got a chance to run.
            const int max_particles = std::clamp(
                (int)em->value("max_particles", 1000.0), 1, kMaxCpuParticles);

            float ex = has_component(e,"Transform") ? transform::world_x(e) : 0.f;
            float ey = has_component(e,"Transform") ? transform::world_y(e) : 0.f;

            bool sub_on_death = em->value("sub_emitter_on_death", false);
            bool sub_on_spawn = em->value("sub_emitter_on_spawn", false);

            // Edge-detect "emitting" so toggling it off then on again replays
            // a burst (mirrors calling Play() on a Unity ParticleSystem).
            bool emitting_now = em->value("emitting", true);
            bool was_emitting = e.value("_was_emitting", true);
            if (emitting_now && !was_emitting) e["_burst_fired"] = false;
            e["_was_emitting"] = emitting_now;

            // Advance existing particles
            if (!e.contains("_particles")) e["_particles"] = Entity::array();
            Entity alive = Entity::array();
            float grav = em->value("gravity_scale", 0.3f) * 980.f; // world-units/s^2
            const std::size_t source_particle_count = std::min(
                e["_particles"].size(), static_cast<std::size_t>(max_particles));
            // Defer death bursts until the complete surviving set is known.
            // Emitting a burst immediately can reserve capacity before later
            // particles prove to be alive, accidentally exceeding the budget.
            std::vector<std::pair<float, float>> death_burst_positions;
            // Most emitters do not use a death sub-emitter. Avoid allocating
            // a transient vector for each of those every frame.
            if (sub_on_death) death_burst_positions.reserve(source_particle_count);
            for (std::size_t particle_index = 0; particle_index < source_particle_count; ++particle_index) {
                auto& p = e["_particles"][particle_index];
                float age = p.value("age",0.f) + dt;
                if (age >= p.value("lifetime",1.f)) {
                    if (sub_on_death && !p.value("_is_sub", false)) {
                        death_burst_positions.emplace_back(
                            p.value("x",0.f), p.value("y",0.f));
                    }
                    continue;
                }
                p["age"] = age;
                // Apply gravity to vertical velocity each tick
                p["vy"] = p.value("vy",0.f) + grav * dt;
                p["x"]  = p.value("x",0.f) + p.value("vx",0.f)*dt;
                p["y"]  = p.value("y",0.f) + p.value("vy",0.f)*dt;
                // Spin: rotate particle by its angular velocity (degrees/s)
                p["rotation"] = p.value("rotation",0.f) + p.value("angular_velocity",0.f)*dt;
                alive.push_back(p);
            }
            e["_particles"] = alive;
            for (const auto& [x, y] : death_burst_positions) {
                const int remaining = std::max(0, max_particles -
                    static_cast<int>(e["_particles"].size()));
                if (remaining == 0) break;
                _spawn_sub_burst(*em, x, y, e["_particles"], remaining);
            }

            // Emit new
            bool burst_mode = em->value("burst", false);
            bool already_burst = e.value("_burst_fired", false);
            bool looping = em->value("looping", true);
            bool should_emit = em->value("emitting",true) && (looping || !already_burst);

            int cur_count = (int)e["_particles"].size();

            if (should_emit && burst_mode && !already_burst) {
                float spread      = em->value("spread",360.f) * (float)M_PI / 180.f;
                float dir_angle   = em->value("direction_angle", -90.f) * (float)M_PI / 180.f;
                float speed       = em->value("speed",100.f);
                float speed_var   = em->value("speed_variation", 0.3f); // fraction ±
                float lifetime    = em->value("lifetime",1.f);
                float lifetime_var= em->value("lifetime_variation", 0.f);
                float rot_start   = em->value("rotation_start", 0.f);
                float rot_var     = em->value("rotation_variation", 0.f);
                float ang_vel     = em->value("angular_velocity", 0.f);
                float ang_vel_var = em->value("angular_velocity_variation", 0.f);
                int   count       = std::max(0, (int)em->value("burst_count", 20.0));
                int   cols        = std::max(1, (int)em->value("atlas_cols",1.0));
                int   rows        = std::max(1, (int)em->value("atlas_rows",1.0));
                bool  rand_fr     = em->value("atlas_random_frame", true);
                int   frame_count = cols * rows;
                count = std::min(count, max_particles - cur_count);
                for (int i = 0; i < count; ++i) {
                    float half = spread * 0.5f;
                    float angle = dir_angle + _uniform(-half, half);
                    float sp2 = speed * (1.f + _uniform(-speed_var, speed_var));
                    float life = lifetime * (1.f + _uniform(-lifetime_var, lifetime_var));
                    life = std::max(0.01f, life);
                    float rot = rot_start + _uniform(-rot_var, rot_var);
                    float av  = ang_vel   + _uniform(-ang_vel_var, ang_vel_var);
                    int frame = frame_count <= 1 ? 0 : (rand_fr ? (int)_uniform(0.f,(float)frame_count) : i % frame_count);
                    e["_particles"].push_back({
                        {"x",ex},{"y",ey},
                        {"vx",std::cos(angle)*sp2},{"vy",std::sin(angle)*sp2},
                        {"age",0.f},{"lifetime",life},
                        {"rotation",rot},{"angular_velocity",av},
                        {"frame",frame}
                    });
                }
                e["_burst_fired"] = true;
            } else if (should_emit && !burst_mode) {
                float rate        = em->value("rate",10.f);
                float spread      = em->value("spread",360.f) * (float)M_PI / 180.f;
                float dir_angle   = em->value("direction_angle", -90.f) * (float)M_PI / 180.f;
                float speed       = em->value("speed",100.f);
                float speed_var   = em->value("speed_variation", 0.3f);
                float lifetime    = em->value("lifetime",1.f);
                float lifetime_var= em->value("lifetime_variation", 0.f);
                float rot_start   = em->value("rotation_start", 0.f);
                float rot_var     = em->value("rotation_variation", 0.f);
                float ang_vel     = em->value("angular_velocity", 0.f);
                float ang_vel_var = em->value("angular_velocity_variation", 0.f);
                int   cols        = std::max(1, (int)em->value("atlas_cols",1.0));
                int   rows        = std::max(1, (int)em->value("atlas_rows",1.0));
                bool  rand_fr     = em->value("atlas_random_frame", true);
                int   frame_count = cols * rows;

                e["_emit_accum"] = e.value("_emit_accum",0.f) + rate*dt;
                int seq = (int)e.value("_emit_seq", 0.f);
                while (e["_emit_accum"].get<float>() >= 1.f && (int)e["_particles"].size() < max_particles) {
                    e["_emit_accum"] = e["_emit_accum"].get<float>() - 1.f;
                    float half = spread * 0.5f;
                    float angle = dir_angle + _uniform(-half, half);
                    float sp2 = speed * (1.f + _uniform(-speed_var, speed_var));
                    float life = lifetime * (1.f + _uniform(-lifetime_var, lifetime_var));
                    life = std::max(0.01f, life);
                    float rot = rot_start + _uniform(-rot_var, rot_var);
                    float av  = ang_vel   + _uniform(-ang_vel_var, ang_vel_var);
                    int frame = frame_count <= 1 ? 0
                              : (rand_fr ? (int)_uniform(0.f,(float)frame_count)
                                         : (seq++ % frame_count));
                    e["_particles"].push_back({
                        {"x",ex},{"y",ey},
                        {"vx",std::cos(angle)*sp2},{"vy",std::sin(angle)*sp2},
                        {"age",0.f},{"lifetime",life},
                        {"rotation",rot},{"angular_velocity",av},
                        {"frame",frame}
                    });
                    if (sub_on_spawn) {
                        const int remaining = std::max(0, max_particles -
                            static_cast<int>(e["_particles"].size()));
                        _spawn_sub_burst(*em, ex, ey, e["_particles"], remaining);
                    }
                }
                e["_emit_seq"] = (float)seq;
            }
        }
    }

private:
    float _uniform(float lo, float hi) {
        return engine_det::uniform_float(lo, hi);
    }

    // Sample a flat keyframe array [t0,v0..vN-1, t1,v0..vN-1, ...] (stride =
    // 1 + n_components) for the value at component `comp_idx`, linearly
    // interpolating between the two bracketing keyframes for time t.
    static float _eval_keyframes(const Entity& curve, float t, int n_components, int comp_idx = 0) {
        int stride = 1 + n_components;
        int count = (int)curve.size() / stride;
        if (count <= 0) return 0.f;
        if (count == 1) return curve[comp_idx + 1].template get<float>();
        t = std::max(0.f, std::min(1.f, t));
        for (int i = 0; i < count - 1; ++i) {
            float t0 = curve[i*stride].template get<float>();
            float t1 = curve[(i+1)*stride].template get<float>();
            if (t <= t1 || i == count - 2) {
                float denom = (t1 - t0);
                float lt = denom > 1e-6f ? (t - t0) / denom : 0.f;
                lt = std::max(0.f, std::min(1.f, lt));
                float v0 = curve[i*stride     + 1 + comp_idx].template get<float>();
                float v1 = curve[(i+1)*stride + 1 + comp_idx].template get<float>();
                return v0 + (v1 - v0) * lt;
            }
        }
        return curve[(count-1)*stride + 1 + comp_idx].template get<float>();
    }

    // Spawn a small burst of simple (non-recursive — tagged "_is_sub" so they
    // never themselves trigger another sub-burst) particles at (x,y), pushed
    // into `out`. Used for both on-spawn and on-death sub-emitter triggers.
    void _spawn_sub_burst(Entity& em, float x, float y, Entity& out, int max_append) {
        int   count    = std::min(std::max(0, (int)em.value("sub_emitter_count", 6.0)),
                                  std::max(0, max_append));
        float speed    = em.value("sub_emitter_speed", 60.f);
        float lifetime = std::max(0.01f, em.value("sub_emitter_lifetime", 0.4f));
        float size     = em.value("sub_emitter_size", 3.f);
        auto  col_vec   = em.value("sub_emitter_color", std::vector<float>{255,255,255,255});
        runtime::Value::array_t col_arr;
        col_arr.reserve(col_vec.size());
        for (float c : col_vec) col_arr.push_back(runtime::Value(c));
        runtime::Value col(std::move(col_arr));
        for (int i = 0; i < count; ++i) {
            float angle = _uniform(0.f, 2.f * (float)M_PI);
            float sp2   = speed * _uniform(0.6f, 1.2f);
            out.push_back({
                {"x",x},{"y",y},
                {"vx",std::cos(angle)*sp2},{"vy",std::sin(angle)*sp2},
                {"age",0.f},{"lifetime",lifetime},
                {"frame",0},
                {"_is_sub",true},
                {"_sub_size",size},
                {"_sub_color",col}
            });
        }
    }
};

// ─── AudioSystem ─────────────────────────────────────────────────────────────
class AudioSystem {
public:
    AudioSystem() {
#ifndef NO_SDL_MIXER
        // Request OGG + MP3 support in addition to WAV (which is always available).
        // Mix_Init returns the flags it successfully initialised; a partial
        // success is fine — WAV still works without OGG/MP3.
        int requested = MIX_INIT_OGG | MIX_INIT_MP3;
        int got = Mix_Init(requested);
        if ((got & MIX_INIT_OGG) == 0)
            std::cerr << "[AudioSystem] OGG support unavailable: " << Mix_GetError() << "\n";
        if ((got & MIX_INIT_MP3) == 0)
            std::cerr << "[AudioSystem] MP3 support unavailable: " << Mix_GetError() << "\n";
        if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0)
            std::cerr << "[AudioSystem] Mix_OpenAudio failed: " << Mix_GetError() << "\n";
        else
            std::cerr << "[AudioSystem] Initialised (WAV"
                      << ((got & MIX_INIT_OGG) ? "+OGG" : "")
                      << ((got & MIX_INIT_MP3) ? "+MP3" : "") << ")\n";
#else
        // The editor intentionally has no SDL_mixer dependency.  Leaving an
        // AudioSource inert in that configuration made its Inspector lie.
        // SDL itself can decode WAV files, so keep a small, real fallback
        // mixer for WAV clips.  Projects with SDL_mixer still use the richer
        // OGG/MP3 path above; the fallback is deliberately a dependable base
        // rather than a fake success state.
        SDL_AudioSpec wanted{};
        wanted.freq = 44100;
        wanted.format = AUDIO_F32SYS;
        wanted.channels = 2;
        wanted.samples = 1024;
        wanted.callback = &AudioSystem::_fallback_audio_callback;
        wanted.userdata = this;
        _fallback_device = SDL_OpenAudioDevice(nullptr, 0, &wanted, &_fallback_spec, 0);
        if (_fallback_device == 0) {
            std::cerr << "[AudioSystem] SDL audio fallback unavailable: " << SDL_GetError() << "\n";
        } else {
            SDL_PauseAudioDevice(_fallback_device, 0);
            std::cerr << "[AudioSystem] SDL WAV fallback initialised (SDL_mixer unavailable).\n";
        }
#endif
    }
    ~AudioSystem() {
#ifndef NO_SDL_MIXER
        for (auto& [k,c] : _cache) if(c) Mix_FreeChunk(c);
        Mix_CloseAudio();
        Mix_Quit();
#else
        if (_fallback_device != 0) {
            SDL_CloseAudioDevice(_fallback_device);
            _fallback_device = 0;
        }
#endif
    }

    void update(EntityList& entities, float /*dt*/) {
#ifndef NO_SDL_MIXER
        for (auto& e : entities) {
            if (!entity_active(e)) continue;
            auto* au = has_component(e,"AudioSource") ? &e["components"]["AudioSource"] : nullptr;
            if (!au) continue;
            // AudioSource2D::PlayOneShot writes a transient clip so a combat
            // cue never steals the entity's looping/ambient source.  The old
            // mixer ignored these fields completely, making a real public
            // API silently do nothing.
            if (au->value("_oneshot_play", false)) {
                const std::string one_shot = au->value("_oneshot_clip", std::string());
                if (!one_shot.empty()) {
                    if (Mix_Chunk* snd = _load(one_shot)) {
                        const float vol = std::max(0.f, std::min(1.f, au->value("_oneshot_volume", 1.f)));
                        Mix_VolumeChunk(snd, (int)(vol * MIX_MAX_VOLUME));
                        Mix_PlayChannel(-1, snd, 0);
                    }
                }
                (*au)["_oneshot_play"] = false;
            }
            std::string clip = au->value("clip","");
            if (clip.empty()) continue;
            int key = e.value("id",0);

            // Stop request — halt the channel this entity owns
            if (au->value("_stop_now",false)) {
                (*au)["_stop_now"] = false;
                (*au)["_is_playing"] = false;
                if (_channel.count(key) && _channel[key] >= 0)
                    Mix_HaltChannel(_channel[key]);
                _channel.erase(key);
                _started.erase(key);
                continue;
            }

            bool awake = au->value("play_on_awake",false) && !_started.count(key);
            bool now   = au->value("_play_now",false);
            if (awake || now) {
                Mix_Chunk* snd = _load(clip);
                if (snd) {
                    float vol = std::max(0.f,std::min(1.f,au->value("volume",1.f)));

                    // Spatial: attenuate volume by distance to nearest Camera2D entity
                    if (au->value("spatial",false)) {
                        float src_x = has_component(e,"Transform") ? transform::world_x(e) : 0.f;
                        float src_y = has_component(e,"Transform") ? transform::world_y(e) : 0.f;
                        float min_dist = au->value("min_distance", 100.f);
                        float max_dist = au->value("max_distance", 1000.f);
                        float cam_x = src_x, cam_y = src_y; // fallback = on-source
                        for (auto& ce : entities) {
                            if (!entity_active(ce) || !has_component(ce,"Camera2D")) continue;
                            cam_x = has_component(ce,"Transform") ? transform::world_x(ce) : 0.f;
                            cam_y = has_component(ce,"Transform") ? transform::world_y(ce) : 0.f;
                            break;
                        }
                        float dx = src_x - cam_x, dy = src_y - cam_y;
                        float dist = std::sqrt(dx*dx + dy*dy);
                        if (dist > max_dist) {
                            vol = 0.f;
                        } else if (dist > min_dist) {
                            float t = (dist - min_dist) / std::max(0.001f, max_dist - min_dist);
                            vol *= 1.f - t; // linear falloff
                        }
                    }

                    Mix_VolumeChunk(snd, (int)(vol * MIX_MAX_VOLUME));

                    int loops = au->value("loop",false) ? -1 : 0;
                    int ch = Mix_PlayChannel(-1, snd, loops);
                    if (ch >= 0) {
                        // Pitch via Mix_SetPlaybackSpeedLocked — SDL_mixer 2.6+ only.
                        // Guard with a macro so it compiles cleanly on older versions.
#if SDL_MIXER_VERSION_ATLEAST(2,6,0)
                        float pitch = au->value("pitch", 1.f);
                        Mix_SetPitch(ch, pitch);
#endif
                        _channel[key] = ch;
                        (*au)["_channel"] = ch;
                    }
                    (*au)["_is_playing"] = true;
                    (*au)["_play_now"]   = false;
                    _started.insert(key);
                }
            }

            // Sync _is_playing to whether the channel is still active
            if (_channel.count(key)) {
                int ch = _channel[key];
                bool still_playing = (ch >= 0) && (Mix_Playing(ch) != 0);
                (*au)["_is_playing"] = still_playing;
                if (!still_playing) _channel.erase(key);
            }
        }
#endif
#ifdef NO_SDL_MIXER
        _update_fallback(entities);
#endif
    }

    void play(Entity& e)  { e["components"]["AudioSource"]["_play_now"] = true; }
    void stop(Entity& e)  { e["components"]["AudioSource"]["_stop_now"] = true; }

private:
#ifndef NO_SDL_MIXER
    std::unordered_map<std::string, Mix_Chunk*> _cache;
    std::unordered_set<int> _started;
    std::unordered_map<int,int> _channel; // entity id -> SDL_mixer channel
    Mix_Chunk* _load(const std::string& path) {
        auto it = _cache.find(path);
        if (it!=_cache.end()) return it->second;
        Mix_Chunk* c = Mix_LoadWAV(path.c_str());
        if (!c) std::cerr << "[AudioSystem] Cannot load: " << path << " " << Mix_GetError() << "\n";
        _cache[path] = c;
        return c;
    }
#else
    struct FallbackClip {
        std::vector<float> samples; // interleaved in the opened device format
    };
    struct FallbackPlayback {
        std::string clip;
        size_t cursor = 0;
        bool loop = false;
        float volume = 1.f;
    };

    SDL_AudioDeviceID _fallback_device = 0;
    SDL_AudioSpec _fallback_spec{};
    std::unordered_map<std::string, FallbackClip> _fallback_cache;
    std::unordered_map<int, FallbackPlayback> _fallback_playbacks;
    std::unordered_set<int> _fallback_started;
    std::mutex _fallback_mutex;

    static void _fallback_audio_callback(void* userdata, Uint8* stream, int bytes) {
        auto* self = static_cast<AudioSystem*>(userdata);
        if (!self || !stream || bytes <= 0) return;
        const size_t count = (size_t)bytes / sizeof(float);
        float* output = reinterpret_cast<float*>(stream);
        std::fill(output, output + count, 0.f);

        std::lock_guard<std::mutex> lock(self->_fallback_mutex);
        for (auto it = self->_fallback_playbacks.begin(); it != self->_fallback_playbacks.end();) {
            auto clip_it = self->_fallback_cache.find(it->second.clip);
            if (clip_it == self->_fallback_cache.end() || clip_it->second.samples.empty()) {
                it = self->_fallback_playbacks.erase(it);
                continue;
            }
            const auto& source = clip_it->second.samples;
            FallbackPlayback& playback = it->second;
            size_t written = 0;
            while (written < count) {
                if (playback.cursor >= source.size()) {
                    if (!playback.loop) break;
                    playback.cursor = 0;
                }
                const size_t available = source.size() - playback.cursor;
                const size_t chunk = std::min(available, count - written);
                for (size_t i = 0; i < chunk; ++i)
                    output[written + i] += source[playback.cursor + i] * playback.volume;
                playback.cursor += chunk;
                written += chunk;
            }
            if (written < count && !playback.loop) it = self->_fallback_playbacks.erase(it);
            else ++it;
        }
        for (size_t i = 0; i < count; ++i) output[i] = std::clamp(output[i], -1.f, 1.f);
    }

    bool _load_fallback_wav(const std::string& path) {
        if (_fallback_cache.find(path) != _fallback_cache.end()) return true;
        if (_fallback_device == 0) return false;
        SDL_AudioSpec source_spec{};
        Uint8* source_buffer = nullptr;
        Uint32 source_length = 0;
        if (!SDL_LoadWAV(path.c_str(), &source_spec, &source_buffer, &source_length)) {
            std::cerr << "[AudioSystem] WAV fallback cannot load '" << path << "': " << SDL_GetError() << "\n";
            return false;
        }
        SDL_AudioCVT convert{};
        if (SDL_BuildAudioCVT(&convert, source_spec.format, source_spec.channels, source_spec.freq,
                              AUDIO_F32SYS, _fallback_spec.channels, _fallback_spec.freq) < 0) {
            std::cerr << "[AudioSystem] WAV fallback cannot convert '" << path << "': " << SDL_GetError() << "\n";
            SDL_FreeWAV(source_buffer);
            return false;
        }
        const int multiplier = std::max(1, convert.len_mult);
        std::vector<Uint8> converted((size_t)source_length * (size_t)multiplier);
        std::memcpy(converted.data(), source_buffer, source_length);
        SDL_FreeWAV(source_buffer);
        convert.buf = converted.data();
        convert.len = (int)source_length;
        if (convert.needed && SDL_ConvertAudio(&convert) < 0) {
            std::cerr << "[AudioSystem] WAV fallback conversion failed for '" << path << "': " << SDL_GetError() << "\n";
            return false;
        }
        const size_t bytes = (size_t)(convert.needed ? convert.len_cvt : convert.len);
        if (bytes < sizeof(float)) return false;
        FallbackClip clip;
        clip.samples.resize(bytes / sizeof(float));
        std::memcpy(clip.samples.data(), converted.data(), clip.samples.size() * sizeof(float));
        _fallback_cache.emplace(path, std::move(clip));
        return true;
    }

    void _update_fallback(EntityList& entities) {
        if (_fallback_device == 0) return;
        for (auto& e : entities) {
            if (!entity_active(e) || !has_component(e, "AudioSource")) continue;
            auto& au = e["components"]["AudioSource"];
            const int key = e.value("id", 0);
            // The fallback device has one owned playback per source.  Route
            // a one-shot through that source (without reporting success
            // unless the WAV really loads) so Editor audio behaves the same
            // as SDL_mixer builds instead of silently ignoring PlayOneShot.
            if (au.value("_oneshot_play", false)) {
                const std::string one_shot = au.value("_oneshot_clip", std::string());
                if (!one_shot.empty()) {
                    au["clip"] = one_shot;
                    au["volume"] = au.value("_oneshot_volume", 1.f);
                    au["_play_now"] = true;
                }
                au["_oneshot_play"] = false;
            }
            const std::string clip = au.value("clip", std::string());
            const bool stop_now = au.value("_stop_now", false);
            const bool awake = au.value("play_on_awake", false) && !_fallback_started.count(key);
            const bool play_now = au.value("_play_now", false);

            std::lock_guard<std::mutex> lock(_fallback_mutex);
            if (stop_now) {
                _fallback_playbacks.erase(key);
                _fallback_started.erase(key);
                au["_stop_now"] = false;
                au["_is_playing"] = false;
                continue;
            }
            if (awake || play_now) {
                if (!clip.empty() && _load_fallback_wav(clip)) {
                    _fallback_playbacks[key] = {clip, 0, au.value("loop", false),
                        std::clamp(au.value("volume", 1.f), 0.f, 1.f)};
                }
                // A missing/bad clip should be reported once per explicit
                // play request, not retried every frame forever.
                _fallback_started.insert(key);
                au["_play_now"] = false;
            }
            auto playing = _fallback_playbacks.find(key);
            if (playing != _fallback_playbacks.end()) {
                playing->second.volume = std::clamp(au.value("volume", 1.f), 0.f, 1.f);
                playing->second.loop = au.value("loop", false);
                au["_is_playing"] = true;
            } else {
                au["_is_playing"] = false;
            }
        }
    }
#endif
};

// NOTE: ScriptSystem now lives in script_system.hpp (full native C++ scripting,
// replacing the old pybind11-era stub that used to be here).

// ─── UISystem ─────────────────────────────────────────────────────────────────
class UISystem {
public:
    UISystem(SDL_Renderer* r) : _renderer(r) {}

    void draw(EntityList& entities, InputSystem& input, int sw, int sh) {
        std::vector<Entity*> ui_ents;
        for (auto& e : entities)
            if (entity_active(e) && _has_ui(e))
                ui_ents.push_back(&e);

        // Sort by UILayer
        std::stable_sort(ui_ents.begin(), ui_ents.end(), [](Entity* a, Entity* b){
            int la=0,lb=0;
            for (auto& [k,v]:(*a)["components"].items()) if(k.rfind("UI",0)==0) la=v.value("order",0);
            for (auto& [k,v]:(*b)["components"].items()) if(k.rfind("UI",0)==0) lb=v.value("order",0);
            return la<lb;
        });

        for (auto* ep : ui_ents) {
            auto& e = *ep;
            if (has_component(e,"UIPanel"))       _draw_panel(e,sw,sh);
            if (has_component(e,"UIImage"))       _draw_image(e,sw,sh);
            if (has_component(e,"UIProgressBar")) _draw_progress_bar(e,sw,sh);
            if (has_component(e,"UISlider"))      _draw_slider(e,sw,sh);
            if (has_component(e,"UIButton"))      _draw_button(e,sw,sh,input);
            if (has_component(e,"UIText"))        _draw_text(e,sw,sh);
        }
    }

private:
    SDL_Renderer* _renderer;

    bool _has_ui(const Entity& e) {
        if (!e.contains("components")) return false;
        for (auto& [k,v]:e["components"].items())
            if (k.rfind("UI",0)==0) return true;
        return false;
    }

    std::tuple<int,int,int,int> _resolve(const Entity& comp, int sw, int sh) {
        float ax=comp.value("anchor_x",0.5f), ay=comp.value("anchor_y",0.5f);
        float px=comp.value("pivot_x",0.5f),  py=comp.value("pivot_y",0.5f);
        int w=comp.value("width",200), h=comp.value("height",40);
        int bx=(int)(ax*sw+comp.value("pos_x",0.f));
        int by=(int)(ay*sh+comp.value("pos_y",0.f));
        return {bx-(int)(px*w), by-(int)(py*h), w, h};
    }

    void _draw_rect_filled(int x,int y,int w,int h, const std::vector<int>& col, float opacity=1.f) {
        if (col.size()<3) return;
        SDL_SetRenderDrawColor(_renderer,(Uint8)col[0],(Uint8)col[1],(Uint8)col[2],(Uint8)(255*opacity));
        SDL_Rect r={x,y,w,h}; SDL_RenderFillRect(_renderer,&r);
    }
    void _draw_rect_outline(int x,int y,int w,int h, const std::vector<int>& col, int bw=1) {
        if (col.size()<3) return;
        SDL_SetRenderDrawColor(_renderer,(Uint8)col[0],(Uint8)col[1],(Uint8)col[2],255);
        for (int i=0;i<bw;++i){SDL_Rect r={x+i,y+i,w-i*2,h-i*2};SDL_RenderDrawRect(_renderer,&r);}
    }

    void _draw_panel(Entity& e, int sw, int sh) {
        auto& c=e["components"]["UIPanel"];
        auto [x,y,w,h]=_resolve(c,sw,sh);
        _draw_rect_filled(x,y,w,h, c.value("color",std::vector<int>{30,30,40,200}));
        if (c.contains("border_color"))
            _draw_rect_outline(x,y,w,h, c["border_color"].get<std::vector<int>>(), c.value("border_width",1));
    }

    void _draw_progress_bar(Entity& e, int sw, int sh) {
        auto& c=e["components"]["UIProgressBar"];
        auto [x,y,w,h]=_resolve(c,sw,sh);
        _draw_rect_filled(x,y,w,h, c.value("bg_color",std::vector<int>{30,30,30,255}));
        float mn=c.value("min",0.f), mx=c.value("max",1.f), val=c.value("value",0.5f);
        float t=(mx>mn)?(val-mn)/(mx-mn):0.f;
        int fw=std::max(0,(int)(w*std::max(0.f,std::min(1.f,t))));
        _draw_rect_filled(x,y,fw,h, c.value("fill_color",std::vector<int>{80,200,80,255}));
        _draw_rect_outline(x,y,w,h, std::vector<int>{100,100,100,255});
    }

    void _draw_slider(Entity& e, int sw, int sh) {
        auto& c=e["components"]["UISlider"];
        auto [x,y,w,h]=_resolve(c,sw,sh);
        int th=h/3;
        _draw_rect_filled(x,y+h/2-th/2,w,th, std::vector<int>{60,60,60,255});
        float mn=c.value("min",0.f), mx=c.value("max",1.f), val=c.value("value",0.5f);
        float t=(mx>mn)?(val-mn)/(mx-mn):0.f;
        int hx=x+(int)(w*t), hy=y+h/2, hr=h/2;
        SDL_SetRenderDrawColor(_renderer,200,200,200,255);
        for (int dy=-hr;dy<=hr;++dy) {
            int dx=(int)std::sqrt(std::max(0.f,(float)(hr*hr-dy*dy)));
            SDL_RenderDrawLine(_renderer,hx-dx,hy+dy,hx+dx,hy+dy);
        }
    }

    void _draw_button(Entity& e, int sw, int sh, InputSystem& input) {
        auto& c=e["components"]["UIButton"];
        auto [x,y,w,h]=_resolve(c,sw,sh);
        int mx=input.mouse_x, my=input.mouse_y;
        bool hover=(mx>=x&&mx<x+w&&my>=y&&my<y+h);
        bool pressed=hover&&input.get_mouse_button(1);
        bool clicked=hover&&input.get_mouse_button_down(1);

        auto col=hover?(pressed?c.value("pressed_color",std::vector<int>{50,50,90,255})
                               :c.value("hover_color",std::vector<int>{110,110,180,255}))
                      :c.value("normal_color",std::vector<int>{80,80,120,255});
        _draw_rect_filled(x,y,w,h,col);
        _draw_rect_outline(x,y,w,h,std::vector<int>{150,150,200,255});

        if (clicked && c.contains("on_click")) {
            std::string fn=c["on_click"].get<std::string>();
            // Queue click event for ScriptSystem to dispatch
            e["_ui_clicked"]=fn;
        }

        // Text label (simple SDL_RenderDrawPoint text not practical; mark for overlay)
        // In a real build, use SDL_ttf here
        (void)c.value("text","");
    }

    void _draw_image(Entity& e, int sw, int sh) {
        auto& c=e["components"]["UIImage"];
        auto [x,y,w,h]=_resolve(c,sw,sh);
        // Images require SDL_image; draw colored rect as fallback
        auto col=c.value("color",std::vector<int>{255,255,255,255});
        _draw_rect_filled(x,y,w,h,col);
    }

    void _draw_text(Entity& e, int sw, int sh) {
        // SDL_ttf text rendering requires linking SDL_ttf.
        // Without it, we draw a placeholder. Users should link SDL_ttf.
        (void)e; (void)sw; (void)sh;
        // TODO: SDL_ttf integration
    }
};
