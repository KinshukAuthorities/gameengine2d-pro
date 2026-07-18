#pragma once
/*
 * animator_panel.hpp — Unity-style Animator window.
 *
 * Edits the selected entity's "Animator" component: a node graph of states
 * (clips or blend trees) connected by transitions, a parameters list
 * (float/int/bool/trigger), per-state clip authoring (frame list, fps,
 * loop/ping-pong, animation events), 1D/2D blend tree editing, and override
 * layers — the same JSON shape engine_cpp/systems.hpp::AnimatorSystem and
 * engine_cpp/unity2d_script_api.hpp::Animator already read/write at runtime,
 * so anything authored here plays correctly in Play mode and in exported
 * games with zero translation step.
 *
 * JSON shape (component "Animator"):
 *   current_animation: string            -- active state name
 *   playing: bool
 *   speed: float, default_fps: float
 *   parameters: { name: float|int|bool, "__trig_"+name: bool }
 *   animations: { stateName: { frames:[...], fps, loop, ping_pong,
 *                               events:[{frame,name}] } }
 *   blend_trees: { stateName: { type:"1d"|"2d", param_x, param_y,
 *                                children:[{clip,threshold}|{clip,pos_x,pos_y}] } }
 *   transitions: [ { from, to, duration, has_exit_time, exit_time,
 *                     conditions:[{param,op,value}] } ]
 *   layers: [ { name, weight, current_animation, frame, playing } ]
 *   _editor_node_pos: { stateName: {x,y} }   -- editor-only graph layout,
 *                                                ignored by the runtime.
 */

#include "editor_state.hpp"
#include "panels.hpp"   // thumbnail_cache::Cache, apply_unity_theme, AssetsPanel conventions
#include "../../engine_cpp/entity.hpp"

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_vulkan.h>
#include <SDL2/SDL.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <string>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

// ─── sRGB-correct color helpers ─────────────────────────────────────────────
// ImGui colours are written as linear floats into the sRGB swapchain; the
// hardware applies the sRGB OETF on write, so raw sRGB byte values would be
// double-encoded and appear washed out.  These helpers pre-linearise the
// sRGB inputs so the round-trip leaves them perceptually correct.
namespace imgui_col {
    static inline float s2l(float v) {
        return (v <= 0.04045f) ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
    }
    // Drop-in replacement for AP_COL32(r,g,b,a) — inputs are sRGB 0-255.
    static inline ImU32 col32(int r, int g, int b, int a = 255) {
        return IM_COL32(
            (int)(s2l(r / 255.f) * 255 + 0.5f),
            (int)(s2l(g / 255.f) * 255 + 0.5f),
            (int)(s2l(b / 255.f) * 255 + 0.5f),
            a);  // alpha is always linear
    }
    // Drop-in replacement for ImVec4(r,g,b,a) where r,g,b are sRGB 0-1.
    static inline ImVec4 vec4(float r, float g, float b, float a = 1.f) {
        return ImVec4(s2l(r), s2l(g), s2l(b), a);
    }
}
#define AP_COL32(r,g,b,a)  imgui_col::col32(r,g,b,a)
#define AP_COL32A(r,g,b,a) imgui_col::col32(r,g,b,a)

// ─── Animator JSON helpers ──────────────────────────────────────────────────
// Small free functions so the rest of the panel reads like "anim.states()"
// rather than repeating .contains()/.is_object() guards everywhere. All take
// the raw "Animator" component Entity& (not the owning GameObject), matching
// how AnimatorSystem/Animator (unity2d_script_api.hpp) address it.
namespace anim_json {

inline Entity& ensure_object(Entity& parent, const char* key) {
    if (!parent.contains(key) || !parent[key].is_object()) parent[key] = Entity::object();
    return parent[key];
}
inline Entity& ensure_array(Entity& parent, const char* key) {
    if (!parent.contains(key) || !parent[key].is_array()) parent[key] = Entity::array();
    return parent[key];
}

inline Entity& animations(Entity& anim) { return ensure_object(anim, "animations"); }
inline Entity& parameters(Entity& anim) { return ensure_object(anim, "parameters"); }
inline Entity& blend_trees(Entity& anim) { return ensure_object(anim, "blend_trees"); }
inline Entity& transitions(Entity& anim) { return ensure_array(anim, "transitions"); }
inline Entity& layers(Entity& anim) { return ensure_array(anim, "layers"); }
inline Entity& node_positions(Entity& anim) { return ensure_object(anim, "_editor_node_pos"); }

// All state names: every key in "animations" plus every key in "blend_trees"
// (a blend tree is itself a selectable/transitionable state, same as Unity).
inline std::vector<std::string> all_state_names(Entity& anim) {
    std::vector<std::string> out;
    if (anim.contains("animations") && anim["animations"].is_object())
        for (auto& kv : anim["animations"].items()) out.push_back(kv.first);
    if (anim.contains("blend_trees") && anim["blend_trees"].is_object())
        for (auto& kv : anim["blend_trees"].items())
            if (std::find(out.begin(), out.end(), kv.first) == out.end()) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

inline bool is_blend_tree(Entity& anim, const std::string& name) {
    return anim.contains("blend_trees") && anim["blend_trees"].is_object() && anim["blend_trees"].contains(name);
}

inline ImVec2 get_node_pos(Entity& anim, const std::string& name, ImVec2 fallback) {
    auto& pos = node_positions(anim);
    if (pos.contains(name) && pos[name].is_object())
        return { pos[name].value("x", fallback.x), pos[name].value("y", fallback.y) };
    pos[name] = Entity::object();
    pos[name]["x"] = fallback.x;
    pos[name]["y"] = fallback.y;
    return fallback;
}
inline void set_node_pos(Entity& anim, const std::string& name, ImVec2 p) {
    auto& pos = node_positions(anim);
    pos[name] = Entity::object();
    pos[name]["x"] = p.x;
    pos[name]["y"] = p.y;
}

// Renames a state everywhere it's referenced: animations/blend_trees keys,
// current_animation, transitions' from/to, blend tree children's clip refs,
// layers' current_animation, and the saved node position — so a rename never
// silently breaks an existing transition or blend tree.
inline void rename_state(Entity& anim, const std::string& from, const std::string& to) {
    if (from.empty() || to.empty() || from == to) return;

    if (anim.contains("animations") && anim["animations"].contains(from)) {
        anim["animations"][to] = anim["animations"][from];
        anim["animations"].erase(from);
    }
    if (anim.contains("blend_trees") && anim["blend_trees"].contains(from)) {
        anim["blend_trees"][to] = anim["blend_trees"][from];
        anim["blend_trees"].erase(from);
        if (anim["blend_trees"][to].contains("children"))
            for (auto& ch : anim["blend_trees"][to]["children"])
                if (ch.value("clip", std::string()) == from) ch["clip"] = to;
    }
    // Other blend trees may reference `from` as a child clip too.
    if (anim.contains("blend_trees"))
        for (auto& [tname, tree] : anim["blend_trees"].items()) {
            if (!tree.contains("children")) continue;
            for (auto& ch : tree["children"])
                if (ch.value("clip", std::string()) == from) ch["clip"] = to;
        }
    if (anim.value("current_animation", std::string()) == from) anim["current_animation"] = to;
    if (anim.contains("transitions"))
        for (auto& t : anim["transitions"]) {
            if (t.value("from", std::string()) == from) t["from"] = to;
            if (t.value("to", std::string()) == from) t["to"] = to;
        }
    if (anim.contains("layers"))
        for (auto& l : anim["layers"])
            if (l.value("current_animation", std::string()) == from) l["current_animation"] = to;
    if (anim.contains("_editor_node_pos") && anim["_editor_node_pos"].contains(from)) {
        anim["_editor_node_pos"][to] = anim["_editor_node_pos"][from];
        anim["_editor_node_pos"].erase(from);
    }
}

// Deletes a state and every transition that referenced it (Any-State
// transitions targeting it included). Blend-tree children that referenced it
// are left as dangling clip names (same as Unity leaving a "missing" motion
// reference) rather than silently deleting blend tree entries out from under
// the user.
inline void delete_state(Entity& anim, const std::string& name) {
    if (anim.contains("animations")) anim["animations"].erase(name);
    if (anim.contains("blend_trees")) anim["blend_trees"].erase(name);
    if (anim.contains("_editor_node_pos")) anim["_editor_node_pos"].erase(name);
    if (anim.value("current_animation", std::string()) == name) anim["current_animation"] = std::string();
    if (anim.contains("transitions") && anim["transitions"].is_array()) {
        Entity kept = Entity::array();
        for (auto& t : anim["transitions"])
            if (t.value("from", std::string()) != name && t.value("to", std::string()) != name)
                kept.push_back(t);
        anim["transitions"] = kept;
    }
}

// Renames a parameter everywhere it's referenced: the parameters object key
// itself (handling the "__trig_" prefix transparently — pass/return display
// names without it), every transition condition's `param` field, and every
// blend tree's param_x/param_y axis — so renaming a parameter never silently
// breaks an existing transition condition or blend tree axis. `from`/`to`
// are display names (no "__trig_" prefix); the trigger-ness of the existing
// parameter is preserved automatically since it's the same key, just renamed.
inline void rename_param(Entity& anim, const std::string& from, const std::string& to) {
    if (from.empty() || to.empty() || from == to) return;
    auto& params = parameters(anim);

    std::string from_key = from, to_key = to;
    bool is_trigger = false;
    if (params.contains("__trig_" + from)) { from_key = "__trig_" + from; to_key = "__trig_" + to; is_trigger = true; }

    if (!params.contains(from_key)) return; // nothing to rename
    if (params.contains(to_key)) return;    // name collision — caller should check first and surface an error

    params[to_key] = params[from_key];
    params.erase(from_key);

    if (anim.contains("transitions"))
        for (auto& t : anim["transitions"]) {
            if (!t.contains("conditions") || !t["conditions"].is_array()) continue;
            for (auto& c : t["conditions"])
                if (c.value("param", std::string()) == from) c["param"] = to;
        }
    if (anim.contains("blend_trees"))
        for (auto& [tname, tree] : anim["blend_trees"].items()) {
            if (tree.value("param_x", std::string()) == from) tree["param_x"] = to;
            if (tree.value("param_y", std::string()) == from) tree["param_y"] = to;
        }
    (void)is_trigger;
}

} // namespace anim_json

// ─── AnimatorPanel ───────────────────────────────────────────────────────────
class AnimatorPanel {
public:
    void init(vkr::RendererBackend& backend) { _thumbs.init(backend); }

    void draw(EditorState& st) {
        if (!_open) return;
        ImGui::SetNextWindowSize({980, 620}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Animator##win", &_open)) { ImGui::End(); return; }

        _window_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows | ImGuiFocusedFlags_RootWindow);

        Entity* e = st.selected_entity();
        if (!e) {
            ImGui::TextDisabled("Select an entity to edit its Animator.");
            ImGui::End();
            return;
        }
        if (!has_component(*e, "Animator")) {
            ImGui::TextDisabled("\"%s\" has no Animator component.", e->value("name", std::string("Entity")).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Add Animator")) {
                (*e)["components"]["Animator"] = Entity::object();
                auto& a = (*e)["components"]["Animator"];
                a["current_animation"] = std::string();
                a["playing"] = true;
                a["loop"] = true;
                a["speed"] = 1.0;
                a["default_fps"] = 12;
                a["animations"] = Entity::object();
                st.undo.push_deep(st.entities);
            }
            ImGui::End();
            return;
        }

        Entity& anim = (*e)["components"]["Animator"];
        std::string eid_tag = "##e" + std::to_string(e->value("id", 0));

        // Reset drag/connect state if selection changed (stale screen-space
        // coordinates from a different entity's graph would otherwise leak in).
        int cur_eid = e->value("id", 0);
        if (cur_eid != _last_entity_id) { _connecting_from.clear(); _pan = {0,0}; _zoom = 1.f; _last_entity_id = cur_eid; _selected_states.clear(); }

        _draw_toolbar(st, *e, anim);
        ImGui::Separator();

        // Three-pane Unity layout: parameters (left) | graph or blend-tree
        // editor (center) | state inspector (right).
        float total_w = ImGui::GetContentRegionAvail().x;
        ImGui::BeginChild((std::string("ParamsCol")+eid_tag).c_str(), {std::min(220.f, total_w*0.22f), 0}, true);
        _draw_parameters(st, anim);
        ImGui::EndChild();
        ImGui::SameLine();

        ImGui::BeginChild((std::string("CenterCol")+eid_tag).c_str(), {std::max(300.f, total_w*0.52f), 0}, true,
                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (!_editing_blend_tree.empty() && anim_json::is_blend_tree(anim, _editing_blend_tree)) {
            _draw_blend_tree_editor(st, *e, anim, _editing_blend_tree);
        } else {
            _draw_graph(st, *e, anim);
        }
        ImGui::EndChild();
        ImGui::SameLine();

        ImGui::BeginChild((std::string("InspectorCol")+eid_tag).c_str(), {0, 0}, true);
        _draw_state_inspector(st, *e, anim);
        ImGui::EndChild();

        ImGui::End();
    }

    bool* open_flag() { return &_open; }
    bool  has_focus()  const { return _window_focused; }

private:
    bool _open = false;
    int  _last_entity_id = -1;
    bool _window_focused = false;  // true when the Animator window has ImGui focus
    std::string _selected_state;       // primary state (shown in inspector)
    std::vector<std::string> _selected_states; // full multi-select set
    std::string _editing_blend_tree;   // non-empty -> center pane shows blend tree editor for this state
    std::string _connecting_from;      // non-empty while dragging a transition arrow out of a node
    ImVec2 _pan{0, 0};                 // graph pan offset in canvas-space pixels (Unity: right-drag)
    float  _zoom = 1.f;                // graph zoom factor (Unity: scroll wheel), clamped [0.35, 2.5]
    char _new_state_buf[64] = "NewState";
    char _rename_buf[64] = {};
    bool _renaming = false;
    thumbnail_cache::Cache _thumbs;

    // ── Toolbar ──────────────────────────────────────────────────────────────
    void _draw_toolbar(EditorState& st, Entity& e, Entity& anim) {
        bool playing = anim.value("playing", false);
        if (ImGui::Button(playing ? "Pause" : "Play")) { anim["playing"] = !playing; st.undo.push_deep(st.entities); }
        ImGui::SameLine();
        std::string cur = anim.value("current_animation", std::string());
        ImGui::TextDisabled("Current: %s", cur.empty() ? "(none)" : cur.c_str());

        ImGui::SameLine(0, 24);
        float speed = anim.value("speed", 1.0f);
        ImGui::SetNextItemWidth(90);
        if (ImGui::DragFloat("Speed", &speed, 0.01f, 0.f, 10.f, "%.2f")) { anim["speed"] = speed; }
        if (ImGui::IsItemDeactivatedAfterEdit()) st.undo.push_deep(st.entities);

        ImGui::SameLine(0, 24);
        float fps = anim.value("default_fps", 12.0f);
        ImGui::SetNextItemWidth(80);
        if (ImGui::DragFloat("Default FPS", &fps, 0.5f, 1.f, 120.f, "%.0f")) { anim["default_fps"] = fps; }
        if (ImGui::IsItemDeactivatedAfterEdit()) st.undo.push_deep(st.entities);

        if (!_editing_blend_tree.empty()) {
            ImGui::SameLine(0, 24);
            if (ImGui::Button("< Back to Graph")) _editing_blend_tree.clear();
        }

        // Multi-select state operations (only visible when states are selected)
        if (!_selected_states.empty()) {
            ImGui::SameLine(0, 24);
            ImGui::TextDisabled("%d state(s) selected", (int)_selected_states.size());
            ImGui::SameLine(0, 8);
            if (ImGui::SmallButton("Duplicate Sel")) {
                st.undo.push_deep(st.entities);
                std::vector<std::string> to_dup = _selected_states;
                for (auto& sname : to_dup) {
                    // Build unique name
                    std::string base = sname + "_copy";
                    std::string unique = base;
                    int n = 1;
                    auto all = anim_json::all_state_names(anim);
                    while (std::find(all.begin(), all.end(), unique) != all.end())
                        unique = base + std::to_string(n++);
                    // Deep-copy clip or blend tree
                    if (anim_json::is_blend_tree(anim, sname))
                        anim["blend_trees"][unique] = anim["blend_trees"][sname];
                    else if (anim.contains("animations") && anim["animations"].contains(sname))
                        anim["animations"][unique] = anim["animations"][sname];
                    // Place node slightly offset
                    ImVec2 pos = anim_json::get_node_pos(anim, sname, {100,100});
                    anim_json::set_node_pos(anim, unique, {pos.x + 20.f, pos.y + 20.f});
                }
            }
            ImGui::SameLine(0, 6);
            if (ImGui::SmallButton("Delete Sel")) {
                st.undo.push_deep(st.entities);
                for (auto& sname : _selected_states) {
                    anim_json::delete_state(anim, sname);
                    if (_editing_blend_tree == sname) _editing_blend_tree.clear();
                }
                _selected_states.clear();
                _selected_state.clear();
            }
        }
    }

    // ── Parameters pane ──────────────────────────────────────────────────────
    void _draw_parameters(EditorState& st, Entity& anim) {
        ImGui::TextUnformatted("Parameters");
        ImGui::Separator();
        auto& params = anim_json::parameters(anim);

        static char new_name[64] = "NewParam";
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##newparamname", new_name, sizeof(new_name));
        if (ImGui::SmallButton("+Float")) { _add_param(st, params, new_name, 0.f); }
        ImGui::SameLine();
        if (ImGui::SmallButton("+Int")) { _add_param(st, params, new_name, 0); }
        if (ImGui::SmallButton("+Bool")) { _add_param(st, params, new_name, false); }
        ImGui::SameLine();
        if (ImGui::SmallButton("+Trigger")) { _add_param(st, params, "__trig_" + std::string(new_name), false); }
        ImGui::Separator();

        std::vector<std::string> keys;
        for (auto& kv : params.items()) keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());

        for (auto& key : keys) {
            ImGui::PushID(key.c_str());
            bool is_trigger = key.rfind("__trig_", 0) == 0;
            std::string display = is_trigger ? key.substr(7) : key;
            Entity& v = params[key];

            // Double-click the name to rename it in place, same gesture Unity
            // uses for its Animator parameter list. While renaming, the label
            // becomes an InputText; Enter or clicking away commits it.
            // IMPORTANT: use `key` (the raw JSON key, always unique) as the
            // rename ID — NOT `display`. Two params can share the same display
            // name momentarily (e.g. one trigger "__trig_Speed" and one float
            // "Speed"), and if we compared against display we'd open the rename
            // widget for both rows at once, causing double-rename bugs.
            bool just_renamed = false;
            if (_renaming_param == key) {
                ImGui::SetNextItemWidth(116);
                bool enter = ImGui::InputText("##paramrename", _param_rename_buf, sizeof(_param_rename_buf), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                bool just_opened = _param_rename_just_opened;
                if (just_opened) {
                    ImGui::SetKeyboardFocusHere(-1);
                    _param_rename_just_opened = false;
                }
                bool committed = enter || ImGui::IsItemDeactivatedAfterEdit();
                // Click away without changing anything: same cancel check used
                // by the Hierarchy/Assets inline renames. Must use the
                // pre-clear `just_opened` value — checking the already-cleared
                // _param_rename_just_opened here would close the box on the
                // very frame it opens, since focus hasn't been applied yet.
                bool cancel = !ImGui::IsItemActive() && !just_opened && !enter;
                if (cancel) {
                    _renaming_param.clear();
                } else if (committed) {
                    std::string new_disp = _param_rename_buf;
                    if (!new_disp.empty() && new_disp != display) {
                        bool collides = params.contains(is_trigger ? "__trig_" + new_disp : new_disp);
                        if (collides) {
                            st.log_error("Parameter \"" + new_disp + "\" already exists.");
                        } else {
                            // rename_param() erases the old key, which invalidates
                            // `v` (a reference into params[key]) — bail out of this
                            // row immediately rather than touch it again below.
                            anim_json::rename_param(anim, display, new_disp);
                            st.undo.push_deep(st.entities);
                            just_renamed = true;
                        }
                    }
                    _renaming_param.clear();
                }
            } else {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(display.c_str());
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    _renaming_param = key;   // store the raw key, not the display name
                    _param_rename_just_opened = true;
                    strncpy(_param_rename_buf, display.c_str(), sizeof(_param_rename_buf) - 1);
                    _param_rename_buf[sizeof(_param_rename_buf) - 1] = 0;
                }
            }
            if (just_renamed) { ImGui::PopID(); continue; }
            ImGui::SameLine(120);

            if (is_trigger) {
                bool armed = v.is_boolean() ? v.get<bool>() : false;
                ImGui::TextColored(armed ? imgui_col::vec4(1,0.7f,0.2f) : imgui_col::vec4(0.5f,0.5f,0.5f), "Trigger");
                ImGui::SameLine();
                if (ImGui::SmallButton("Fire")) { v = true; st.undo.push_deep(st.entities); }
            } else if (v.is_boolean()) {
                bool b = v.get<bool>();
                if (ImGui::Checkbox("##val", &b)) { v = b; st.undo.push_deep(st.entities); }
            } else if (v.is_number_integer()) {
                int iv = v.get<int>();
                ImGui::SetNextItemWidth(-1);
                if (ImGui::DragInt("##val", &iv)) v = iv;
                if (ImGui::IsItemDeactivatedAfterEdit()) st.undo.push_deep(st.entities);
            } else {
                float fv = v.get<float>();
                ImGui::SetNextItemWidth(-1);
                if (ImGui::DragFloat("##val", &fv, 0.05f)) v = fv;
                if (ImGui::IsItemDeactivatedAfterEdit()) st.undo.push_deep(st.entities);
            }

            ImGui::SameLine(ImGui::GetWindowWidth() - 28);
            if (ImGui::SmallButton("x")) { params.erase(key); st.undo.push_deep(st.entities); }
            ImGui::PopID();
        }
    }

    std::string _renaming_param;
    char _param_rename_buf[64] = {};
    bool _param_rename_just_opened = false;

    template <typename T>
    void _add_param(EditorState& st, Entity& params, const std::string& name, T def) {
        std::string clean = name.empty() ? "Param" : name;
        if (clean.rfind("__trig_", 0) != 0 && params.contains(clean)) { st.log_error("Parameter \"" + clean + "\" already exists."); return; }
        params[clean] = def;
        st.undo.push_deep(st.entities);
    }

    // ── Graph (state machine) ────────────────────────────────────────────────
    // Hand-rolled node canvas: states are draggable boxes positioned via
    // anim["_editor_node_pos"] (editor-only, ignored by the runtime), Any
    // State is a fixed pseudo-node, transitions are arrows drawn between node
    // edges. Click a node to select/inspect it; drag from a node's right edge
    // onto another node to create a transition (mirrors Unity's Animator
    // window interaction).
    void _draw_graph(EditorState& st, Entity& e, Entity& anim) {
        ImGui::TextUnformatted("State Machine");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        ImGui::InputText("##newstatename", _new_state_buf, sizeof(_new_state_buf));
        ImGui::SameLine();
        if (ImGui::SmallButton("+ State")) _create_state(st, anim, _new_state_buf, /*as_blend_tree=*/false);
        ImGui::SameLine();
        if (ImGui::SmallButton("+ Blend Tree")) _create_state(st, anim, _new_state_buf, /*as_blend_tree=*/true);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset View")) { _pan = {0,0}; _zoom = 1.f; }
        ImGui::SameLine();
        ImGui::TextDisabled("%.0f%%", _zoom * 100.f);
        ImGui::TextDisabled("Right-drag to pan, wheel to zoom. Drag a node's right dot onto another node for a transition.");
        ImGui::Separator();

        ImVec2 canvas_p0 = ImGui::GetCursorScreenPos();
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (avail.x < 50) avail.x = 50;
        if (avail.y < 50) avail.y = 50;
        ImVec2 canvas_p1 = { canvas_p0.x + avail.x, canvas_p0.y + avail.y };

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(canvas_p0, canvas_p1, AP_COL32(30, 30, 32, 255));
        dl->PushClipRect(canvas_p0, canvas_p1, true);

        // Canvas-space -> screen-space: canvas-space is the coordinate system
        // node positions are stored in (anim["_editor_node_pos"]); screen-space
        // is what ImGui/SDL actually draw to. _pan/_zoom are applied once here
        // so every node/arrow/handle below just works in canvas-space and never
        // touches _pan/_zoom directly — there is exactly one place coordinates
        // can drift, which is what made the old version's panning unreliable.
        auto to_screen = [&](ImVec2 c) -> ImVec2 {
            return { canvas_p0.x + (c.x + _pan.x) * _zoom, canvas_p0.y + (c.y + _pan.y) * _zoom };
        };
        auto to_canvas = [&](ImVec2 s) -> ImVec2 {
            return { (s.x - canvas_p0.x) / _zoom - _pan.x, (s.y - canvas_p0.y) / _zoom - _pan.y };
        };

        _draw_grid(dl, canvas_p0, canvas_p1, _pan, _zoom);

        ImGui::InvisibleButton("##canvas", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        bool canvas_hovered = ImGui::IsItemHovered();

        // ── Pan (right-drag) / zoom (wheel), exactly Unity's Animator graph
        // convention. Handled here, explicitly, instead of relying on the
        // child window's own scroll — that's what made the canvas appear to
        // "scroll itself" before: the parent BeginChild auto-scrolled on
        // wheel/drag independently of node-position math, desyncing what was
        // drawn from what was clickable. The child now has
        // ImGuiWindowFlags_NoScrollbar | NoScrollWithMouse, so this is the
        // only thing that can move the view.
        if (canvas_hovered) {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
                ImVec2 d = ImGui::GetIO().MouseDelta;
                _pan.x += d.x / _zoom;
                _pan.y += d.y / _zoom;
            }
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.f) {
                ImVec2 before = to_canvas(ImGui::GetMousePos());
                _zoom = std::clamp(_zoom * (wheel > 0.f ? 1.1f : 1.f/1.1f), 0.35f, 2.5f);
                // Zoom around the cursor, not the canvas origin, so whatever
                // you're pointing at stays under the cursor as you zoom.
                ImVec2 after = to_canvas(ImGui::GetMousePos());
                _pan.x += (after.x - before.x);
                _pan.y += (after.y - before.y);
            }
        }

        std::vector<std::string> states = anim_json::all_state_names(anim);
        const ImVec2 node_size{150, 46};
        const ImVec2 any_state_canvas_pos{20, 20};

        // Assign a default grid layout to any state missing a saved position.
        for (size_t i = 0; i < states.size(); ++i) {
            ImVec2 fallback{ 220.f + (float)(i % 3) * 190.f, 30.f + (float)(i / 3) * 80.f };
            anim_json::get_node_pos(anim, states[i], fallback);
        }

        // ── Transition arrows (drawn first, under the nodes) ───────────────────
        auto node_rect = [&](const std::string& name) -> std::pair<ImVec2, ImVec2> {
            ImVec2 rel = anim_json::get_node_pos(anim, name, {0, 0});
            ImVec2 p0 = to_screen(rel);
            ImVec2 p1 = to_screen({ rel.x + node_size.x, rel.y + node_size.y });
            return { p0, p1 };
        };
        auto any_state_rect = std::make_pair(to_screen(any_state_canvas_pos), to_screen({any_state_canvas_pos.x + 110, any_state_canvas_pos.y + 36}));

        if (anim.contains("transitions") && anim["transitions"].is_array()) {
            int ti = 0;
            for (auto& t : anim["transitions"]) {
                std::string from = t.value("from", std::string());
                std::string to = t.value("to", std::string());
                bool from_any = (from == "*" || from == "Any" || from == "AnyState");
                if (to.empty()) { ++ti; continue; }
                if (!from_any && std::find(states.begin(), states.end(), from) == states.end()) { ++ti; continue; }
                if (std::find(states.begin(), states.end(), to) == states.end()) { ++ti; continue; }

                auto [fp0, fp1] = from_any ? any_state_rect : node_rect(from);
                auto [tp0, tp1] = node_rect(to);
                ImVec2 a{ (fp0.x + fp1.x) * 0.5f, (fp0.y + fp1.y) * 0.5f };
                ImVec2 b{ (tp0.x + tp1.x) * 0.5f, (tp0.y + tp1.y) * 0.5f };
                bool sel = (ti == _selected_transition);
                _draw_arrow(dl, a, b, sel ? AP_COL32(255, 200, 60, 255) : AP_COL32(150, 150, 160, 200), sel ? 3.f : 2.f);

                // Click target near arrow midpoint to select this transition.
                ImVec2 mid{ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f };
                if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    ImVec2 mp = ImGui::GetMousePos();
                    float d = std::hypot(mp.x - mid.x, mp.y - mid.y);
                    if (d < 10.f) { _selected_transition = ti; _selected_state.clear(); }
                }
                ++ti;
            }
        }

        // In-progress connection line, following the mouse.
        if (!_connecting_from.empty()) {
            auto [fp0, fp1] = (_connecting_from == "*") ? any_state_rect : node_rect(_connecting_from);
            ImVec2 a{ (fp0.x + fp1.x) * 0.5f, (fp0.y + fp1.y) * 0.5f };
            _draw_arrow(dl, a, ImGui::GetMousePos(), AP_COL32(255, 200, 60, 180), 2.f);
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                std::string drop_target = _hit_test_node(states, [&](const std::string& n){ return node_rect(n); }, ImGui::GetMousePos());
                if (!drop_target.empty() && drop_target != _connecting_from) {
                    auto& trs = anim_json::transitions(anim);
                    Entity t = Entity::object();
                    t["from"] = _connecting_from;
                    t["to"] = drop_target;
                    t["duration"] = 0.f;
                    t["has_exit_time"] = false;
                    t["exit_time"] = 1.f;
                    t["conditions"] = Entity::array();
                    trs.push_back(t);
                    st.undo.push_deep(st.entities);
                }
                _connecting_from.clear();
            }
        }

        // ── Any State pseudo-node ───────────────────────────────────────────────
        {
            bool hovered = ImGui::IsMouseHoveringRect(any_state_rect.first, any_state_rect.second);
            dl->AddRectFilled(any_state_rect.first, any_state_rect.second, AP_COL32(70, 55, 30, 255), 6.f);
            dl->AddRect(any_state_rect.first, any_state_rect.second, AP_COL32(200, 160, 60, 255), 6.f, 0, 1.5f);
            dl->AddText({any_state_rect.first.x + 10, any_state_rect.first.y + 10}, AP_COL32(230, 200, 140, 255), "Any State");
            ImVec2 out_handle{ any_state_rect.second.x - 6, (any_state_rect.first.y + any_state_rect.second.y) * 0.5f };
            dl->AddCircleFilled(out_handle, 5.f, AP_COL32(255, 210, 80, 255));
            if (canvas_hovered && hovered && std::hypot(ImGui::GetMousePos().x - out_handle.x, ImGui::GetMousePos().y - out_handle.y) < 10.f
                && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                _connecting_from = "*";
            }
        }

        // ── State nodes ──────────────────────────────────────────────────────────
        for (auto& name : states) {
            ImGui::PushID(name.c_str());
            auto [p0, p1] = node_rect(name);
            bool is_current = anim.value("current_animation", std::string()) == name;
            bool is_selected = (name == _selected_state);
            bool is_bt = anim_json::is_blend_tree(anim, name);

            bool in_multisel = std::find(_selected_states.begin(), _selected_states.end(), name) != _selected_states.end();
            ImU32 fill = is_bt ? AP_COL32(55, 45, 75, 255) : (in_multisel && !is_selected ? AP_COL32(55, 55, 75, 255) : AP_COL32(50, 50, 56, 255));
            ImU32 border = is_selected ? AP_COL32(255, 200, 60, 255) : (in_multisel ? AP_COL32(150, 160, 255, 255) : (is_current ? AP_COL32(90, 200, 110, 255) : AP_COL32(110, 110, 120, 255)));
            dl->AddRectFilled(p0, p1, fill, 6.f);
            dl->AddRect(p0, p1, border, 6.f, 0, is_selected || is_current ? 2.5f : 1.5f);
            dl->AddText({p0.x + 10, p0.y + 8}, AP_COL32(230, 230, 230, 255), name.c_str());
            dl->AddText({p0.x + 10, p0.y + 26}, AP_COL32(150, 150, 160, 255), is_bt ? "Blend Tree" : "Clip");
            if (is_current) dl->AddCircleFilled({p1.x - 12, p0.y + 12}, 4.f, AP_COL32(90, 220, 110, 255));

            // Output connection handle (right edge).
            ImVec2 out_handle{ p1.x - 6, (p0.y + p1.y) * 0.5f };
            dl->AddCircleFilled(out_handle, 5.f, AP_COL32(150, 180, 255, 255));

            bool node_hovered = canvas_hovered && ImGui::IsMouseHoveringRect(p0, p1);
            bool handle_hovered = canvas_hovered && std::hypot(ImGui::GetMousePos().x - out_handle.x, ImGui::GetMousePos().y - out_handle.y) < 10.f;

            if (handle_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                _connecting_from = name;
            } else if (node_hovered && _connecting_from.empty() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                _selected_transition = -1;
                if (ImGui::GetIO().KeyCtrl) {
                    // Ctrl+click: toggle membership in multi-select
                    auto it = std::find(_selected_states.begin(), _selected_states.end(), name);
                    if (it != _selected_states.end()) {
                        _selected_states.erase(it);
                        if (_selected_state == name && !_selected_states.empty())
                            _selected_state = _selected_states.back();
                        else if (_selected_states.empty())
                            _selected_state.clear();
                    } else {
                        _selected_states.push_back(name);
                        _selected_state = name;
                    }
                } else {
                    // Plain click: single select
                    _selected_state = name;
                    _selected_states = {name};
                }
                _dragging_node = name;
                // Offset must be in canvas-space (same space node positions are
                // stored in), not raw screen pixels — otherwise this teleports
                // the node the moment _pan/_zoom differ from their defaults,
                // since p0 here is already a zoomed/panned screen position.
                ImVec2 rel = anim_json::get_node_pos(anim, name, {0, 0});
                ImVec2 mouse_canvas = to_canvas(ImGui::GetMousePos());
                _drag_offset = { mouse_canvas.x - rel.x, mouse_canvas.y - rel.y };
            }
            if (node_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && is_bt) {
                _editing_blend_tree = name;
            }
            if (node_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                ImGui::OpenPopup("node_ctx");
                _context_node = name;
            }
            ImGui::PopID();
        }

        // Node dragging (applies regardless of which node ID is hovered now).
        if (!_dragging_node.empty()) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                ImVec2 mouse_canvas = to_canvas(ImGui::GetMousePos());
                ImVec2 new_rel{ mouse_canvas.x - _drag_offset.x, mouse_canvas.y - _drag_offset.y };
                new_rel.x = std::max(0.f, new_rel.x);
                new_rel.y = std::max(0.f, new_rel.y);
                anim_json::set_node_pos(anim, _dragging_node, new_rel);
            } else {
                _dragging_node.clear();
                st.undo.push_deep(st.entities);
            }
        }

        // ── Drag-select (rubber-band) ─────────────────────────────────────────────
        // Start: left-press on empty canvas (not over a node/handle)
        if (canvas_hovered && _connecting_from.empty() && _dragging_node.empty()
                && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            bool hit_any = ImGui::IsMouseHoveringRect(any_state_rect.first, any_state_rect.second);
            for (auto& n : states) { auto [p0,p1] = node_rect(n); if (ImGui::IsMouseHoveringRect(p0,p1)) hit_any = true; }
            if (!hit_any) {
                _drag_selecting = true;
                _drag_sel_start_canvas = to_canvas(ImGui::GetMousePos());
                if (!ImGui::GetIO().KeyCtrl) {
                    _selected_state.clear(); _selected_states.clear(); _selected_transition = -1;
                }
            }
        }

        if (_drag_selecting) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                // Draw rubber-band rectangle
                ImVec2 a = to_screen(_drag_sel_start_canvas);
                ImVec2 b = ImGui::GetMousePos();
                ImVec2 rmin{std::min(a.x,b.x), std::min(a.y,b.y)};
                ImVec2 rmax{std::max(a.x,b.x), std::max(a.y,b.y)};
                dl->AddRectFilled(rmin, rmax, AP_COL32(88,155,255,30));
                dl->AddRect(rmin, rmax, AP_COL32(88,155,255,180), 0.f, 0, 1.5f);

                // Live highlight nodes inside rect
                ImVec2 cmin = to_canvas(rmin);
                ImVec2 cmax = to_canvas(rmax);
                for (auto& n : states) {
                    ImVec2 npos = anim_json::get_node_pos(anim, n, {0,0});
                    ImVec2 ncen{npos.x + node_size.x * 0.5f, npos.y + node_size.y * 0.5f};
                    bool inside = (ncen.x >= cmin.x && ncen.x <= cmax.x &&
                                   ncen.y >= cmin.y && ncen.y <= cmax.y);
                    // Highlight border for nodes inside rect
                    if (inside) {
                        auto [hp0, hp1] = node_rect(n);
                        dl->AddRect(hp0, hp1, AP_COL32(88,155,255,220), 6.f, 0, 2.f);
                    }
                }
            } else {
                // Mouse released — commit selection
                ImVec2 a = _drag_sel_start_canvas;
                ImVec2 b = to_canvas(ImGui::GetMousePos());
                ImVec2 cmin{std::min(a.x,b.x), std::min(a.y,b.y)};
                ImVec2 cmax{std::max(a.x,b.x), std::max(a.y,b.y)};
                float dist = std::hypot(b.x-a.x, b.y-a.y);
                if (dist > 4.f) {
                    // Only commit if dragged a meaningful distance (not a plain click)
                    if (!ImGui::GetIO().KeyCtrl) _selected_states.clear();
                    for (auto& n : states) {
                        ImVec2 npos = anim_json::get_node_pos(anim, n, {0,0});
                        ImVec2 ncen{npos.x + node_size.x * 0.5f, npos.y + node_size.y * 0.5f};
                        bool inside = (ncen.x >= cmin.x && ncen.x <= cmax.x &&
                                       ncen.y >= cmin.y && ncen.y <= cmax.y);
                        if (inside && std::find(_selected_states.begin(), _selected_states.end(), n) == _selected_states.end())
                            _selected_states.push_back(n);
                    }
                    if (!_selected_states.empty()) _selected_state = _selected_states.back();
                }
                _drag_selecting = false;
            }
        }

        if (ImGui::BeginPopup("node_ctx")) {
            std::string name = _context_node;
            if (ImGui::MenuItem("Set as Default State")) { anim["current_animation"] = name; st.undo.push_deep(st.entities); }
            if (ImGui::MenuItem("Rename")) { _renaming = true; _selected_state = name; _selected_states = {name}; strncpy(_rename_buf, name.c_str(), sizeof(_rename_buf)-1); }
            if (anim_json::is_blend_tree(anim, name) && ImGui::MenuItem("Edit Blend Tree")) _editing_blend_tree = name;
            if (ImGui::MenuItem("Duplicate")) {
                st.undo.push_deep(st.entities);
                std::string base = name + "_copy", unique = base;
                int nn = 1;
                auto all2 = anim_json::all_state_names(anim);
                while (std::find(all2.begin(), all2.end(), unique) != all2.end()) unique = base + std::to_string(nn++);
                if (anim_json::is_blend_tree(anim, name)) anim["blend_trees"][unique] = anim["blend_trees"][name];
                else if (anim.contains("animations") && anim["animations"].contains(name)) anim["animations"][unique] = anim["animations"][name];
                ImVec2 pos2 = anim_json::get_node_pos(anim, name, {100,100});
                anim_json::set_node_pos(anim, unique, {pos2.x + 20.f, pos2.y + 20.f});
                _selected_state = unique; _selected_states = {unique};
            }
            if (ImGui::MenuItem("Delete")) {
                st.undo.push_deep(st.entities);
                // Delete all selected states if right-clicked node is among them
                bool in_sel = std::find(_selected_states.begin(), _selected_states.end(), name) != _selected_states.end();
                std::vector<std::string> to_del = in_sel ? _selected_states : std::vector<std::string>{name};
                for (auto& dn : to_del) {
                    anim_json::delete_state(anim, dn);
                    if (_editing_blend_tree == dn) _editing_blend_tree.clear();
                }
                _selected_state.clear(); _selected_states.clear();
            }
            ImGui::EndPopup();
        }

        // ── Del key: delete selected states (only when animator has focus) ────────
        if (_window_focused && !_selected_states.empty()
                && ImGui::IsKeyPressed(ImGuiKey_Delete) && !ImGui::GetIO().WantTextInput) {
            // (global handler in editor_main is already blocked when we have focus)
            // Nothing to do here — the toolbar "Delete Sel" button handles it.
            // But we DO want Del to work without needing the button, so duplicate it:
            for (auto& sname : _selected_states) {
                anim_json::delete_state(anim, sname);
                if (_editing_blend_tree == sname) _editing_blend_tree.clear();
            }
            _selected_states.clear();
            _selected_state.clear();
            st.undo.push_deep(st.entities);
        }

        dl->PopClipRect();
    }

    int _selected_transition = -1;
    std::string _dragging_node;
    ImVec2 _drag_offset{0,0};
    std::string _context_node;
    // Drag-select (rubber-band) state
    bool   _drag_selecting = false;
    ImVec2 _drag_sel_start_canvas{0,0};

    void _draw_grid(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImVec2 pan, float zoom) {
        const float step = 24.f * zoom;
        if (step < 4.f) return; // zoomed out far enough that lines would just be noise
        // Offset so the grid is anchored to canvas-space (0,0), not screen-space,
        // so it actually scrolls under the nodes when panning/zooming.
        float offset_x = std::fmod(pan.x * zoom, step);
        float offset_y = std::fmod(pan.y * zoom, step);
        for (float x = p0.x + offset_x; x < p1.x; x += step) dl->AddLine({x, p0.y}, {x, p1.y}, AP_COL32(255,255,255,8));
        for (float y = p0.y + offset_y; y < p1.y; y += step) dl->AddLine({p0.x, y}, {p1.x, y}, AP_COL32(255,255,255,8));
    }

    void _draw_arrow(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float thickness) {
        dl->AddLine(a, b, col, thickness);
        ImVec2 dir{ b.x - a.x, b.y - a.y };
        float len = std::hypot(dir.x, dir.y);
        if (len < 1.f) return;
        dir.x /= len; dir.y /= len;
        ImVec2 perp{ -dir.y, dir.x };
        ImVec2 tip{ a.x + dir.x * (len - 14.f), a.y + dir.y * (len - 14.f) };
        ImVec2 left{ tip.x - dir.x*12.f + perp.x*6.f, tip.y - dir.y*12.f + perp.y*6.f };
        ImVec2 right{ tip.x - dir.x*12.f - perp.x*6.f, tip.y - dir.y*12.f - perp.y*6.f };
        dl->AddTriangleFilled(b, left, right, col);
    }

    template <typename RectFn>
    std::string _hit_test_node(const std::vector<std::string>& states, RectFn rect_of, ImVec2 mp) {
        for (auto& n : states) {
            auto [p0, p1] = rect_of(n);
            if (mp.x >= p0.x && mp.x <= p1.x && mp.y >= p0.y && mp.y <= p1.y) return n;
        }
        return {};
    }

    void _create_state(EditorState& st, Entity& anim, std::string name, bool as_blend_tree) {
        if (name.empty()) name = "NewState";
        std::string unique = name;
        int n = 1;
        auto exists = [&](const std::string& nm){
            auto names = anim_json::all_state_names(anim);
            return std::find(names.begin(), names.end(), nm) != names.end();
        };
        while (exists(unique)) unique = name + std::to_string(n++);

        if (as_blend_tree) {
            auto& trees = anim_json::blend_trees(anim);
            Entity tree = Entity::object();
            tree["type"] = "1d";
            tree["param_x"] = std::string("Speed");
            tree["children"] = Entity::array();
            trees[unique] = tree;
        } else {
            auto& anims = anim_json::animations(anim);
            Entity clip = Entity::object();
            clip["frames"] = Entity::array();
            clip["fps"] = anim.value("default_fps", 12.0f);
            clip["loop"] = true;
            clip["ping_pong"] = false;
            clip["events"] = Entity::array();
            anims[unique] = clip;
        }
        if (anim.value("current_animation", std::string()).empty()) anim["current_animation"] = unique;
        _selected_state = unique;
        st.undo.push_deep(st.entities);
    }

    // ── State inspector (right pane) ─────────────────────────────────────────
    void _draw_state_inspector(EditorState& st, Entity& e, Entity& anim) {
        if (_selected_transition >= 0 && anim.contains("transitions") && anim["transitions"].is_array()
            && _selected_transition < (int)anim["transitions"].size()) {
            _draw_transition_inspector(st, anim, anim["transitions"][_selected_transition]);
            return;
        }
        if (_selected_state.empty()) {
            ImGui::TextDisabled("Select a state to edit it.");
            return;
        }
        if (!anim.contains("animations") || !anim["animations"].contains(_selected_state)) {
            if (anim_json::is_blend_tree(anim, _selected_state)) {
                ImGui::TextUnformatted(_selected_state.c_str());
                ImGui::TextDisabled("Blend Tree");
                if (ImGui::Button("Edit Blend Tree")) _editing_blend_tree = _selected_state;
                return;
            }
            ImGui::TextDisabled("State \"%s\" not found.", _selected_state.c_str());
            return;
        }

        Entity& clip = anim["animations"][_selected_state];

        // Reset the scrub/playback cursor whenever the selected clip changes,
        // so dragging the playhead on one clip never leaks into another.
        if (_selected_state != _preview_clip) {
            _preview_clip = _selected_state;
            _preview_frame = 0.f;
            _preview_playing = false;
        }

        if (_renaming) {
            ImGui::SetNextItemWidth(-1);
            bool enter = ImGui::InputText("##rename", _rename_buf, sizeof(_rename_buf), ImGuiInputTextFlags_EnterReturnsTrue);
            if (enter || ImGui::IsItemDeactivatedAfterEdit()) {
                std::string new_name = _rename_buf;
                if (!new_name.empty() && new_name != _selected_state) {
                    anim_json::rename_state(anim, _selected_state, new_name);
                    _selected_state = new_name;
                    _preview_clip = new_name;
                    st.undo.push_deep(st.entities);
                }
                _renaming = false;
            }
        } else {
            ImGui::Text("%s", _selected_state.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Rename")) { _renaming = true; strncpy(_rename_buf, _selected_state.c_str(), sizeof(_rename_buf)-1); }
            ImGui::SameLine();
            if (ImGui::SmallButton("Duplicate")) _duplicate_clip(st, anim, _selected_state);
        }
        ImGui::Separator();

        float fps = clip.value("fps", 12.0f);
        ImGui::SetNextItemWidth(120);
        if (ImGui::DragFloat("FPS", &fps, 0.5f, 1.f, 120.f, "%.0f")) clip["fps"] = fps;
        if (ImGui::IsItemDeactivatedAfterEdit()) st.undo.push_deep(st.entities);

        bool loop = clip.value("loop", true);
        if (ImGui::Checkbox("Loop", &loop)) { clip["loop"] = loop; st.undo.push_deep(st.entities); }
        ImGui::SameLine();
        bool pp = clip.value("ping_pong", false);
        if (ImGui::Checkbox("Ping-Pong", &pp)) { clip["ping_pong"] = pp; st.undo.push_deep(st.entities); }

        ImGui::Spacing();
        _draw_preview(st, anim, clip);

        ImGui::Spacing();
        ImGui::TextUnformatted("Frames (drag textures from Assets, drag to reorder):");
        _draw_frame_strip(st, anim, clip);

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Slice Sprite Sheet")) _draw_sheet_slicer(st, anim, clip);

        ImGui::Spacing();
        ImGui::Separator();
        _draw_timeline(st, clip);
    }

    // Resolves one frame list entry into a texture + UV sub-rect to draw.
    // A clip's "frames" array holds one of two shapes depending on how it was
    // authored:
    //  - a filename string, when frames were dragged in individually from
    //    the Assets panel — the whole image is the frame.
    //  - an integer sheet-cell index, when frames were picked via "Slice
    //    Sprite Sheet" — only that cell's sub-rect of anim["sprite_sheet"]
    //    is the frame. The frame strip used to only handle the string case,
    //    so sheet-sliced frames always fell through to a "?" placeholder.
    // Returns false (no texture) if the frame is empty/unresolvable.
    bool _resolve_frame_thumb(EditorState& st, Entity& anim, const Entity& frame_entry,
                               const thumbnail_cache::Entry*& out_th, ImVec2& out_uv0, ImVec2& out_uv1,
                               std::string& out_label) {
        out_uv0 = {0,0}; out_uv1 = {1,1};
        if (frame_entry.is_string()) {
            std::string fname = frame_entry.get<std::string>();
            out_label = fname;
            if (fname.empty()) { out_th = nullptr; return false; }
            out_th = _thumbs.get(st.asset_dir + "/" + fname);
            return out_th && out_th->tex.valid();
        }
        if (frame_entry.is_number_integer()) {
            int cell = frame_entry.get<int>();
            std::string sheet = anim.value("sprite_sheet", std::string());
            out_label = sheet.empty() ? std::string() : (sheet + " #" + std::to_string(cell));
            if (sheet.empty()) { out_th = nullptr; return false; }
            out_th = _thumbs.get(st.asset_dir + "/" + sheet);
            if (!out_th || !out_th->tex.valid() || out_th->w <= 0 || out_th->h <= 0) return false;

            int cols = std::max(1, anim.value("sheet_columns", 1));
            int rows = std::max(1, anim.value("sheet_rows", 1));
            int fw = anim.value("frame_width", out_th->w / cols);
            int fh = anim.value("frame_height", out_th->h / rows);
            int sp = anim.value("sheet_spacing", 0);
            int pad = anim.value("sheet_padding", 0);
            if (fw <= 0 || fh <= 0) return false;
            int total = cols * rows;
            if (cell < 0 || cell >= total) return false;

            int c = cell % cols, r = cell / cols;
            float px0 = (float)(pad + c * (fw + sp));
            float py0 = (float)(pad + r * (fh + sp));
            out_uv0 = { px0 / out_th->w, py0 / out_th->h };
            out_uv1 = { (px0 + fw) / out_th->w, (py0 + fh) / out_th->h };
            return true;
        }
        out_th = nullptr;
        return false;
    }

    // ── Live preview (scrubbable, with Play/Pause) ─────────────────────────
    // Unity's Animation window keeps a small preview of the selected clip
    // independent of whether the game is actually playing. This advances a
    // local _preview_frame using the same fps/loop/ping_pong rules
    // AnimatorSystem uses at runtime (see _frame_index above), purely for
    // editor display — it never writes into anim["frame"], so it can't
    // desync actual playback.
    std::string _preview_clip;
    float _preview_frame = 0.f;
    bool  _preview_playing = false;

    void _draw_preview(EditorState& st, Entity& anim, Entity& clip) {
        auto& frames = anim_json::ensure_array(clip, "frames");
        float fps = std::max(0.01f, clip.value("fps", 12.0f));
        bool loop = clip.value("loop", true);
        bool pp = clip.value("ping_pong", false);
        int n = (int)frames.size();

        if (_preview_playing && n > 0) {
            _preview_frame += fps * ImGui::GetIO().DeltaTime;
        }
        int idx = 0;
        if (n > 0) {
            int raw = (int)_preview_frame;
            if (pp && n > 1) {
                int cycle = n * 2 - 2;
                idx = raw % cycle;
                if (idx >= n) idx = cycle - idx;
                idx = std::clamp(idx, 0, n - 1);
            } else if (loop) {
                idx = raw % n;
            } else {
                idx = std::min(n - 1, raw);
                if (idx >= n - 1) _preview_playing = false;
            }
        }

        ImVec2 box_sz{96, 96};
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, {p0.x + box_sz.x, p0.y + box_sz.y}, AP_COL32(20, 20, 22, 255));
        dl->AddRect(p0, {p0.x + box_sz.x, p0.y + box_sz.y}, AP_COL32(90, 90, 100, 255));

        std::string fname;
        if (n > 0) {
            const thumbnail_cache::Entry* th = nullptr;
            ImVec2 uv0, uv1;
            if (_resolve_frame_thumb(st, anim, frames[idx], th, uv0, uv1, fname) && th && th->tex.valid()) {
                float src_w = (uv1.x - uv0.x) * th->w;
                float src_h = (uv1.y - uv0.y) * th->h;
                float scale = std::min(box_sz.x / std::max(1.f, src_w), box_sz.y / std::max(1.f, src_h));
                ImVec2 img_sz{ src_w * scale, src_h * scale };
                ImVec2 off{ (box_sz.x - img_sz.x) * 0.5f, (box_sz.y - img_sz.y) * 0.5f };
                dl->AddImage((ImTextureID)(intptr_t)th->imgui_ds, {p0.x + off.x, p0.y + off.y}, {p0.x + off.x + img_sz.x, p0.y + off.y + img_sz.y}, uv0, uv1);
            }
        }
        ImGui::Dummy(box_sz);
        ImGui::SameLine();

        ImGui::BeginGroup();
        if (ImGui::Button(_preview_playing ? "Pause##prev" : "Play##prev")) {
            _preview_playing = !_preview_playing;
            if (_preview_playing && n > 0 && idx >= n - 1 && !loop && !pp) _preview_frame = 0.f;
        }
        ImGui::SameLine();
        if (ImGui::Button("|<")) { _preview_frame = 0.f; _preview_playing = false; }
        ImGui::SameLine();
        ImGui::Text("Frame %d / %d", n > 0 ? idx + 1 : 0, n);
        ImGui::TextDisabled("%s", fname.empty() ? "(no frame)" : fname.c_str());
        ImGui::EndGroup();
    }

    // ── Timeline / dopesheet ─────────────────────────────────────────────────
    // A horizontal ruler of frame cells with a draggable playhead (drives the
    // preview above) and animation-event markers beneath it — the Unity
    // "Animation" window's dopesheet, condensed into one strip since this
    // engine only animates one track (the sprite frame) per clip.
    void _draw_timeline(EditorState& st, Entity& clip) {
        ImGui::TextUnformatted("Timeline");
        auto& frames = anim_json::ensure_array(clip, "frames");
        auto& events = anim_json::ensure_array(clip, "events");
        int n = (int)frames.size();
        if (n == 0) { ImGui::TextDisabled("Add frames to see the timeline."); return; }

        float cell_w = 22.f;
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::Dummy({1, 10}); // headroom so the playhead triangle isn't clipped above the ruler
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 ruler_sz{ std::max(avail, cell_w * n), 22.f };
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, {p0.x + ruler_sz.x, p0.y + ruler_sz.y}, AP_COL32(26, 26, 29, 255));

        for (int i = 0; i < n; ++i) {
            float x = p0.x + i * cell_w;
            bool current = (i == (int)_preview_frame % std::max(1, n));
            dl->AddRect({x, p0.y}, {x + cell_w, p0.y + ruler_sz.y}, AP_COL32(60, 60, 66, 255));
            if (current) dl->AddRectFilled({x + 1, p0.y + 1}, {x + cell_w - 1, p0.y + ruler_sz.y - 1}, AP_COL32(90, 140, 220, 120));
            char buf[8]; snprintf(buf, sizeof(buf), "%d", i);
            dl->AddText({x + 4, p0.y + 4}, AP_COL32(180, 180, 185, 255), buf);
        }
        // Playhead.
        float head_x = p0.x + ((int)_preview_frame % std::max(1, n)) * cell_w + cell_w * 0.5f;
        dl->AddTriangleFilled({head_x - 5, p0.y - 8}, {head_x + 5, p0.y - 8}, {head_x, p0.y - 1}, AP_COL32(255, 200, 60, 255));

        ImGui::InvisibleButton("##ruler", ruler_sz);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float mx = ImGui::GetMousePos().x - p0.x;
            int clicked = std::clamp((int)(mx / cell_w), 0, n - 1);
            _preview_frame = (float)clicked;
            _preview_playing = false;
        }

        // Event markers row, directly beneath the ruler — click to jump the
        // playhead there too, matching Unity's event-marker behavior.
        ImVec2 ep0{ p0.x, p0.y + ruler_sz.y + 2 };
        dl->AddLine(ep0, {ep0.x + ruler_sz.x, ep0.y}, AP_COL32(50, 50, 56, 255));
        for (auto& ev : events) {
            int f = ev.value("frame", 0);
            if (f < 0 || f >= n) continue;
            float x = ep0.x + f * cell_w + cell_w * 0.5f;
            dl->AddCircleFilled({x, ep0.y + 8}, 4.f, AP_COL32(220, 140, 60, 255));
        }
        ImGui::Dummy({ruler_sz.x, 14});

        ImGui::Spacing();
        _draw_events_editor(st, clip);
    }

    void _draw_frame_strip(EditorState& st, Entity& anim, Entity& clip) {
        auto& frames = anim_json::ensure_array(clip, "frames");
        ImVec2 thumb_sz{48, 48};
        float avail_w = ImGui::GetContentRegionAvail().x;
        int per_row = std::max(1, (int)(avail_w / (thumb_sz.x + 8)));

        int remove_idx = -1;
        int reorder_from = -1, reorder_to = -1;
        for (int i = 0; i < (int)frames.size(); ++i) {
            if (i % per_row != 0) ImGui::SameLine();
            ImGui::PushID(i);
            const thumbnail_cache::Entry* th = nullptr;
            ImVec2 uv0, uv1;
            std::string fname;
            bool has_tex = _resolve_frame_thumb(st, anim, frames[i], th, uv0, uv1, fname) && th && th->tex.valid();

            bool is_preview = (i == (int)_preview_frame % std::max(1, (int)frames.size()));
            if (is_preview) ImGui::PushStyleColor(ImGuiCol_Button, imgui_col::vec4(0.35f,0.55f,0.85f,0.6f));

            ImGui::BeginGroup();
            if (has_tex) {
                ImGui::ImageButton("##frame", (ImTextureID)(intptr_t)th->imgui_ds, thumb_sz, uv0, uv1);
            } else {
                ImGui::Button(fname.empty() ? "?" : fname.substr(0, 3).c_str(), thumb_sz);
            }
            if (is_preview) ImGui::PopStyleColor();

            // Click to scrub the preview to this frame.
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) { _preview_frame = (float)i; _preview_playing = false; }

            // Drag-to-reorder: this slot is both a drag source (frame index)
            // and a drop target, so frames can be reordered the way Unity's
            // dopesheet keyframes can be dragged left/right.
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                ImGui::SetDragDropPayload("ANIM_FRAME_IDX", &i, sizeof(int));
                if (has_tex) ImGui::Image((ImTextureID)(intptr_t)th->imgui_ds, {32,32}, uv0, uv1);
                ImGui::TextUnformatted(fname.empty() ? "(empty)" : fname.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                    frames[i] = std::string((const char*)p->Data);
                    st.undo.push_deep(st.entities);
                }
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ANIM_FRAME_IDX")) {
                    reorder_from = *(const int*)p->Data;
                    reorder_to = i;
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::IsItemHovered() && !fname.empty()) ImGui::SetTooltip("%s", fname.c_str());
            ImGui::EndGroup();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) remove_idx = i;
            ImGui::PopID();
        }

        // Trailing "add" slot.
        if (frames.size() % per_row != 0 || frames.empty()) ImGui::SameLine();
        ImGui::Button("+", thumb_sz);
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                frames.push_back(std::string((const char*)p->Data));
                st.undo.push_deep(st.entities);
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::TextDisabled("Click a frame to preview it, right-click to remove.");

        // Classic stable-list reorder: pull the dragged frame out, then
        // re-insert it at the drop target's position (adjusted for the
        // removal shifting later indices left by one). Verified against all
        // from/to pairs to always produce a same-size permutation with no
        // duplicated or dropped frames.
        if (reorder_from >= 0 && reorder_to >= 0 && reorder_from != reorder_to && reorder_from < (int)frames.size()) {
            Entity moved = frames[reorder_from];
            std::vector<Entity> tmp;
            for (int i = 0; i < (int)frames.size(); ++i) if (i != reorder_from) tmp.push_back(frames[i]);
            int insert_at = reorder_to;
            if (reorder_from < reorder_to) insert_at -= 1;
            insert_at = std::clamp(insert_at, 0, (int)tmp.size());
            tmp.insert(tmp.begin() + insert_at, moved);
            Entity result = Entity::array();
            for (auto& f : tmp) result.push_back(f);
            clip["frames"] = result;
            st.undo.push_deep(st.entities);
        }

        if (remove_idx >= 0) {
            Entity kept = Entity::array();
            for (int i = 0; i < (int)frames.size(); ++i) if (i != remove_idx) kept.push_back(frames[i]);
            clip["frames"] = kept;
            st.undo.push_deep(st.entities);
        }
    }

    void _duplicate_clip(EditorState& st, Entity& anim, const std::string& name) {
        if (!anim.contains("animations") || !anim["animations"].contains(name)) return;
        std::string base = name + "_copy";
        std::string unique = base;
        int n = 1;
        auto& anims = anim_json::animations(anim);
        while (anims.contains(unique)) unique = base + std::to_string(n++);
        anims[unique] = anims[name]; // deep value copy (Entity is value-typed JSON)
        _selected_state = unique;
        _preview_clip.clear();
        st.undo.push_deep(st.entities);
    }

    // ── Sprite-sheet slicer ──────────────────────────────────────────────────
    // The runtime's sheet-mode fields (use_sprite_sheet/sprite_sheet/
    // frame_width/frame_height/sheet_columns/sheet_rows/sheet_spacing/
    // sheet_padding — see AnimatorSystem::update in systems.hpp) live on the
    // Animator component itself, not per-clip, since the renderer only reads
    // one active sheet at a time. This editor mirrors that: slicing writes
    // those Animator-level fields and then fills the *currently selected
    // clip's* `frames` array with the resulting cell indices (0..rows*cols-1)
    // — same two-step Unity workflow (slice a sheet, then assign cells to a
    // clip) adapted to this engine's single-sheet-at-a-time model.
    void _draw_sheet_slicer(EditorState& st, Entity& anim, Entity& clip) {
        std::string sheet = anim.value("sprite_sheet", std::string());
        if (InspectorPanel::draw_project_asset_slot(
                st, "Sprite Sheet", sheet,
                {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif"},
                "Image files\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif\0All files\0*.*\0",
                "Choose sprite sheet")) {
            anim["sprite_sheet"] = sheet;
            st.undo.push_deep(st.entities);
        }
        ImGui::TextDisabled("Drag a compatible texture from Assets or choose Browse.");

        sheet = anim.value("sprite_sheet", std::string());
        const thumbnail_cache::Entry* th = sheet.empty() ? nullptr : _thumbs.get(st.asset_dir + "/" + sheet);

        int fw = anim.value("frame_width", th ? th->w : 0);
        int fh = anim.value("frame_height", th ? th->h : 0);
        int cols = std::max(1, anim.value("sheet_columns", 1));
        int rows = std::max(1, anim.value("sheet_rows", 1));
        int sp = anim.value("sheet_spacing", 0);
        int pad = anim.value("sheet_padding", 0);

        ImGui::SetNextItemWidth(80);
        if (ImGui::DragInt("Frame W", &fw, 1, 1, 4096)) anim["frame_width"] = fw;
        if (ImGui::IsItemDeactivatedAfterEdit()) st.undo.push_deep(st.entities);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        if (ImGui::DragInt("Frame H", &fh, 1, 1, 4096)) anim["frame_height"] = fh;
        if (ImGui::IsItemDeactivatedAfterEdit()) st.undo.push_deep(st.entities);

        ImGui::SetNextItemWidth(80);
        if (ImGui::DragInt("Columns", &cols, 1, 1, 256)) anim["sheet_columns"] = cols;
        if (ImGui::IsItemDeactivatedAfterEdit()) st.undo.push_deep(st.entities);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        if (ImGui::DragInt("Rows", &rows, 1, 1, 256)) anim["sheet_rows"] = rows;
        if (ImGui::IsItemDeactivatedAfterEdit()) st.undo.push_deep(st.entities);

        ImGui::SetNextItemWidth(80);
        if (ImGui::DragInt("Spacing", &sp, 1, 0, 256)) anim["sheet_spacing"] = sp;
        if (ImGui::IsItemDeactivatedAfterEdit()) st.undo.push_deep(st.entities);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        if (ImGui::DragInt("Padding", &pad, 1, 0, 256)) anim["sheet_padding"] = pad;
        if (ImGui::IsItemDeactivatedAfterEdit()) st.undo.push_deep(st.entities);

        if (th && th->w > 0 && fw > 0) {
            int c = std::max(1, (th->w - pad*2 + sp) / (fw + sp));
            if (ImGui::SmallButton("Auto-fit Columns")) { anim["sheet_columns"] = c; st.undo.push_deep(st.entities); }
            ImGui::SameLine();
        }
        if (th && th->h > 0 && fh > 0) {
            int r = std::max(1, (th->h - pad*2 + sp) / (fh + sp));
            if (ImGui::SmallButton("Auto-fit Rows")) { anim["sheet_rows"] = r; st.undo.push_deep(st.entities); }
        }

        // Grid preview with clickable cells: click toggles that cell's
        // membership in the current clip's frame list (Unity's "select
        // cells, click Slice" workflow, simplified to one click per cell).
        if (th && th->tex.valid()) {
            float max_w = ImGui::GetContentRegionAvail().x;
            float scale = std::min(1.f, max_w / std::max(1, th->w));
            ImVec2 img_sz{ th->w * scale, th->h * scale };
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddImage((ImTextureID)(intptr_t)th->imgui_ds, p0, {p0.x + img_sz.x, p0.y + img_sz.y});

            auto& frames = anim_json::ensure_array(clip, "frames");
            std::vector<int> used;
            for (auto& fr : frames) if (fr.is_number_integer()) used.push_back(fr.get<int>());

            ImGui::InvisibleButton("##sheetgrid", img_sz);
            bool grid_hovered = ImGui::IsItemHovered();
            ImVec2 mp = ImGui::GetMousePos();
            int total = cols * rows;
            int clicked_cell = -1;
            for (int r = 0; r < rows; ++r) {
                for (int c = 0; c < cols; ++c) {
                    int idx = r * cols + c;
                    if (idx >= total) continue;
                    float cx = pad + c * (fw + sp);
                    float cy = pad + r * (fh + sp);
                    ImVec2 cp0{ p0.x + cx * scale, p0.y + cy * scale };
                    ImVec2 cp1{ cp0.x + fw * scale, cp0.y + fh * scale };
                    bool is_used = std::find(used.begin(), used.end(), idx) != used.end();
                    dl->AddRect(cp0, cp1, is_used ? AP_COL32(90, 220, 110, 220) : AP_COL32(255, 255, 255, 60));
                    if (is_used) dl->AddRectFilled(cp0, cp1, AP_COL32(90, 220, 110, 50));
                    if (grid_hovered && mp.x >= cp0.x && mp.x < cp1.x && mp.y >= cp0.y && mp.y < cp1.y
                        && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        clicked_cell = idx;
                    }
                }
            }
            if (clicked_cell >= 0) {
                anim["use_sprite_sheet"] = true;
                auto it = std::find(used.begin(), used.end(), clicked_cell);
                if (it != used.end()) {
                    // Toggle off: remove that cell index from frames.
                    Entity kept = Entity::array();
                    for (auto& fr : frames) if (!(fr.is_number_integer() && fr.get<int>() == clicked_cell)) kept.push_back(fr);
                    clip["frames"] = kept;
                } else {
                    // This clip may still hold leftover filename-string
                    // frames from before it was switched to sheet mode
                    // (e.g. dragged-in textures). The runtime indexes this
                    // array positionally and only resolves *integer* entries
                    // to sheet cells (falling back to the array index for
                    // strings), so mixing shapes makes picked cells appear
                    // to have no effect. Purge non-integer entries the first
                    // time a sheet cell is picked so the array holds only
                    // cell indices from here on.
                    Entity kept = Entity::array();
                    for (auto& fr : frames) if (fr.is_number_integer()) kept.push_back(fr);
                    kept.push_back(clicked_cell);
                    clip["frames"] = kept;
                }
                st.undo.push_deep(st.entities);
            }
            ImGui::TextDisabled("Click cells to add/remove them from this clip's frame list.");

            bool use_sheet = anim.value("use_sprite_sheet", false);
            if (ImGui::Checkbox("Use Sprite Sheet Mode (Animator-wide)", &use_sheet)) { anim["use_sprite_sheet"] = use_sheet; st.undo.push_deep(st.entities); }
            ImGui::TextDisabled("Sheet mode applies to the whole Animator, matching the runtime (one active sheet at a time).");
        } else if (!sheet.empty()) {
            ImGui::TextDisabled("Could not load \"%s\" for preview.", sheet.c_str());
        }
    }

    void _draw_events_editor(EditorState& st, Entity& clip) {
        ImGui::TextUnformatted("Animation Events");
        auto& events = anim_json::ensure_array(clip, "events");
        int n_frames = clip.contains("frames") && clip["frames"].is_array() ? (int)clip["frames"].size() : 0;

        int remove_idx = -1;
        for (int i = 0; i < (int)events.size(); ++i) {
            ImGui::PushID(i);
            Entity& ev = events[i];
            int frame = ev.value("frame", 0);
            ImGui::SetNextItemWidth(70);
            if (ImGui::DragInt("##frame", &frame, 0.2f, 0, std::max(0, n_frames - 1))) ev["frame"] = frame;
            if (ImGui::IsItemDeactivatedAfterEdit()) st.undo.push_deep(st.entities);
            ImGui::SameLine();
            char name_buf[64];
            std::string nm = ev.value("name", std::string());
            strncpy(name_buf, nm.c_str(), sizeof(name_buf)-1); name_buf[sizeof(name_buf)-1]=0;
            ImGui::SetNextItemWidth(140);
            if (ImGui::InputText("##name", name_buf, sizeof(name_buf))) ev["name"] = std::string(name_buf);
            if (ImGui::IsItemDeactivatedAfterEdit()) st.undo.push_deep(st.entities);
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) remove_idx = i;
            ImGui::PopID();
        }
        if (ImGui::SmallButton("+ Add Event")) {
            Entity ev = Entity::object();
            ev["frame"] = 0;
            ev["name"] = std::string("Event");
            events.push_back(ev);
            st.undo.push_deep(st.entities);
        }
        if (remove_idx >= 0) {
            Entity kept = Entity::array();
            for (int i = 0; i < (int)events.size(); ++i) if (i != remove_idx) kept.push_back(events[i]);
            clip["events"] = kept;
            st.undo.push_deep(st.entities);
        }
    }

    // ── Transition inspector ─────────────────────────────────────────────────
    void _draw_transition_inspector(EditorState& st, Entity& anim, Entity& t) {
        std::string from = t.value("from", std::string());
        std::string to = t.value("to", std::string());
        ImGui::Text("%s -> %s", from == "*" ? "Any State" : from.c_str(), to.c_str());
        ImGui::Separator();

        float duration = t.value("duration", 0.f);
        ImGui::SetNextItemWidth(120);
        if (ImGui::DragFloat("Duration", &duration, 0.01f, 0.f, 10.f, "%.2fs")) t["duration"] = duration;
        if (ImGui::IsItemDeactivatedAfterEdit()) st.undo.push_deep(st.entities);

        bool has_exit = t.value("has_exit_time", false);
        if (ImGui::Checkbox("Has Exit Time", &has_exit)) { t["has_exit_time"] = has_exit; st.undo.push_deep(st.entities); }
        if (has_exit) {
            float exit_time = t.value("exit_time", 1.f);
            ImGui::SetNextItemWidth(120);
            if (ImGui::DragFloat("Exit Time", &exit_time, 0.01f, 0.f, 1.f, "%.2f")) t["exit_time"] = exit_time;
            if (ImGui::IsItemDeactivatedAfterEdit()) st.undo.push_deep(st.entities);
        }

        ImGui::Spacing();
        ImGui::TextUnformatted("Conditions");
        auto& conds = anim_json::ensure_array(t, "conditions");
        auto& params = anim_json::parameters(anim);
        std::vector<std::string> param_names;
        for (auto& kv : params.items()) {
            param_names.push_back(kv.first.rfind("__trig_",0)==0 ? kv.first.substr(7) : kv.first);
        }
        std::sort(param_names.begin(), param_names.end());
        param_names.erase(std::unique(param_names.begin(), param_names.end()), param_names.end());

        int remove_idx = -1;
        const char* ops[] = {"trigger","bool_true","bool_false","greater","less","equals","notequal"};
        for (int i = 0; i < (int)conds.size(); ++i) {
            ImGui::PushID(i);
            Entity& c = conds[i];
            std::string p = c.value("param", std::string());
            ImGui::SetNextItemWidth(100);
            if (ImGui::BeginCombo("##param", p.empty() ? "(param)" : p.c_str())) {
                for (auto& pn : param_names) if (ImGui::Selectable(pn.c_str(), pn==p)) { c["param"]=pn; st.undo.push_deep(st.entities); }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            std::string op = c.value("op", std::string("trigger"));
            ImGui::SetNextItemWidth(90);
            if (ImGui::BeginCombo("##op", op.c_str())) {
                for (auto* o : ops) if (ImGui::Selectable(o, op==o)) { c["op"]=std::string(o); st.undo.push_deep(st.entities); }
                ImGui::EndCombo();
            }
            if (op == "greater" || op == "less" || op == "equals" || op == "notequal") {
                ImGui::SameLine();
                float v = c.value("value", 0.f);
                ImGui::SetNextItemWidth(70);
                if (ImGui::DragFloat("##val", &v, 0.05f)) c["value"] = v;
                if (ImGui::IsItemDeactivatedAfterEdit()) st.undo.push_deep(st.entities);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) remove_idx = i;
            ImGui::PopID();
        }
        if (ImGui::SmallButton("+ Condition")) {
            Entity c = Entity::object();
            c["param"] = param_names.empty() ? std::string() : param_names.front();
            c["op"] = std::string("trigger");
            c["value"] = 0.f;
            conds.push_back(c);
            st.undo.push_deep(st.entities);
        }
        if (remove_idx >= 0) {
            Entity kept = Entity::array();
            for (int i = 0; i < (int)conds.size(); ++i) if (i != remove_idx) kept.push_back(conds[i]);
            t["conditions"] = kept;
            st.undo.push_deep(st.entities);
        }

        ImGui::Spacing();
        if (ImGui::Button("Delete Transition")) {
            Entity kept = Entity::array();
            for (auto& tr : anim["transitions"]) {
                bool same = tr.value("from",std::string())==from && tr.value("to",std::string())==to
                            && tr.value("duration",0.f)==t.value("duration",0.f);
                if (!same) kept.push_back(tr);
            }
            anim["transitions"] = kept;
            _selected_transition = -1;
            st.undo.push_deep(st.entities);
        }
    }

    // ── Blend tree editor (center pane, replaces graph while active) ──────────
    // Modelled after Unity 2D's Animator → Blend Tree inspector:
    //   • Header row: breadcrumb path, type combo, param pickers
    //   • 1D: horizontal track with colour-coded motion bars + draggable
    //     threshold diamonds, a preview-value red indicator line, and an
    //     auto-threshold button (Space Evenly).
    //   • 2D: dark canvas with grid lines, draggable motion dots, axis
    //     labels, a red crosshair for the live preview position, and a
    //     convex-hull influence overlay.
    //   • Motion list below the visualiser (same as Unity's "Motion" table):
    //     clip picker | threshold/X/Y spinners | up/down arrows | ✕ remove
    //   • "+ Add Motion Field" button at the bottom.

    void _draw_blend_tree_editor(EditorState& st, Entity& e, Entity& anim, const std::string& state_name) {
        Entity& tree = anim["blend_trees"][state_name];

        // ── Header / breadcrumb ────────────────────────────────────────────
        ImGui::PushStyleColor(ImGuiCol_Text, imgui_col::vec4(0.55f,0.55f,0.60f));
        ImGui::TextUnformatted("Base Layer >");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextUnformatted(state_name.c_str());
        ImGui::Separator();

        // ── Type selector ──────────────────────────────────────────────────
        std::string type = tree.value("type", std::string("1d"));
        const char* type_labels[] = { "1D Blend Tree", "2D Freeform Cartesian" };
        int type_idx = (type == "2d") ? 1 : 0;
        ImGui::SetNextItemWidth(200);
        if (ImGui::Combo("Blend Type", &type_idx, type_labels, 2)) {
            tree["type"] = std::string(type_idx == 1 ? "2d" : "1d");
            st.undo.push_deep(st.entities);
        }
        type = tree.value("type", std::string("1d"));

        // ── Param pickers ──────────────────────────────────────────────────
        auto& params_obj = anim_json::parameters(anim);
        std::vector<std::string> param_names;
        for (auto& kv : params_obj.items())
            param_names.push_back(kv.first.rfind("__trig_",0)==0 ? kv.first.substr(7) : kv.first);
        std::sort(param_names.begin(), param_names.end());
        param_names.erase(std::unique(param_names.begin(), param_names.end()), param_names.end());
        // Filter to only numeric params (float/int) — bools/triggers make no
        // sense as blend axes, matching Unity's parameter picker behaviour.
        std::vector<std::string> num_params;
        for (auto& pn : param_names) {
            if (params_obj.contains(pn)) {
                auto& v = params_obj[pn];
                if (v.is_number()) num_params.push_back(pn);
            }
        }
        if (num_params.empty()) num_params = param_names; // fallback: show all

        auto param_picker = [&](const char* label, const char* key) {
            std::string cur = tree.value(key, std::string());
            ImGui::SetNextItemWidth(130);
            if (ImGui::BeginCombo(label, cur.empty() ? "(none)" : cur.c_str())) {
                for (auto& pn : num_params)
                    if (ImGui::Selectable(pn.c_str(), pn==cur)) { tree[key]=pn; st.undo.push_deep(st.entities); }
                ImGui::EndCombo();
            }
        };

        param_picker("Parameter", "param_x");
        if (type == "2d") { ImGui::SameLine(); param_picker("Parameter Y", "param_y"); }

        ImGui::Spacing();
        auto& children = anim_json::ensure_array(tree, "children");
        std::vector<std::string> clip_names = anim_json::all_state_names(anim);

        // ── 1D Visualiser ──────────────────────────────────────────────────
        if (type == "1d") {
            // Sort children by threshold for the bar display.
            int  n = (int)children.size();
            // Gather min/max threshold.
            float tmin =  1e9f, tmax = -1e9f;
            for (auto& ch : children) {
                float t = ch.value("threshold", 0.f);
                tmin = std::min(tmin, t); tmax = std::max(tmax, t);
            }
            if (n == 0) { tmin = -1.f; tmax = 1.f; }
            if (tmax - tmin < 0.001f) { tmin -= 0.5f; tmax += 0.5f; }
            float range = tmax - tmin;

            // Preview scrubber value (shown as a red line).
            if (_bt_preview_x < tmin) _bt_preview_x = tmin;
            if (_bt_preview_x > tmax) _bt_preview_x = tmax;

            // Canvas
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            float  cw = std::max(200.f, ImGui::GetContentRegionAvail().x - 4.f);
            float  ch_height = 80.f;
            ImVec2 p1{ p0.x + cw, p0.y + ch_height };
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Background
            dl->AddRectFilled(p0, p1, AP_COL32(22, 22, 26, 255), 4.f);
            dl->AddRect(p0, p1, AP_COL32(60, 60, 70, 255), 4.f);

            // Grid lines every 0.25 of range
            {
                float step = range / 4.f;
                for (int gi = 1; gi < 4; ++gi) {
                    float gx = p0.x + (gi * step / range) * cw;
                    dl->AddLine({gx, p0.y}, {gx, p1.y}, AP_COL32(255,255,255,10));
                }
            }

            // Motion bars: each child occupies a region between its threshold
            // and the next child's threshold, Unity style.
            const ImU32 bar_colours[] = {
                AP_COL32( 70,130,190,200), AP_COL32( 80,170,100,200),
                AP_COL32(190,120, 60,200), AP_COL32(160, 80,160,200),
                AP_COL32(190,190, 60,200), AP_COL32( 60,170,170,200),
                AP_COL32(210, 80, 80,200), AP_COL32(130,100,200,200),
            };
            // Build sorted index list for display purposes only.
            std::vector<int> sorted_idx(n);
            std::iota(sorted_idx.begin(), sorted_idx.end(), 0);
            std::stable_sort(sorted_idx.begin(), sorted_idx.end(), [&](int a, int b){
                return children[a].value("threshold",0.f) < children[b].value("threshold",0.f);
            });

            for (int si = 0; si < n; ++si) {
                int    i   = sorted_idx[si];
                Entity& ch = children[i];
                float  thr = ch.value("threshold", 0.f);
                float  nxt = (si+1 < n) ? children[sorted_idx[si+1]].value("threshold",0.f) : tmax;
                float  x0  = p0.x + (thr - tmin) / range * cw;
                float  x1  = p0.x + (nxt - tmin) / range * cw;
                x1 = std::max(x0 + 2.f, x1);
                ImU32 col = bar_colours[i % 8];
                dl->AddRectFilled({x0, p0.y+4}, {x1, p1.y-4}, col, 2.f);
                // Clip name label, clipped to bar width.
                std::string clname = ch.value("clip", std::string("?"));
                float tw = ImGui::CalcTextSize(clname.c_str()).x;
                if (tw < x1 - x0 - 4)
                    dl->AddText({x0+4, p0.y + ch_height*0.5f - 7}, AP_COL32(230,230,230,230), clname.c_str());

                // Draggable threshold diamond.
                float  dx = p0.x + (thr - tmin) / range * cw;
                ImVec2 diam{ dx, p1.y - 6.f };
                bool  dhov = std::hypot(ImGui::GetMousePos().x-diam.x, ImGui::GetMousePos().y-diam.y) < 7.f;
                dl->AddTriangleFilled({diam.x,diam.y-8},{diam.x-5,diam.y+1},{diam.x+5,diam.y+1},
                    dhov ? AP_COL32(255,210,80,255) : AP_COL32(200,200,210,255));
            }

            // Preview position line (red, draggable).
            {
                float px = p0.x + (_bt_preview_x - tmin) / range * cw;
                dl->AddLine({px, p0.y}, {px, p1.y}, AP_COL32(220, 60, 60, 255), 2.f);
                dl->AddTriangleFilled({px,p0.y},{px-5,p0.y-8},{px+5,p0.y-8}, AP_COL32(220,60,60,255));
            }

            // Axis labels.
            {
                char buf[32];
                snprintf(buf,sizeof(buf),"%.2f",tmin);
                dl->AddText({p0.x+2, p1.y-16}, AP_COL32(130,130,140,200), buf);
                snprintf(buf,sizeof(buf),"%.2f",tmax);
                float tw2=ImGui::CalcTextSize(buf).x;
                dl->AddText({p1.x-tw2-2, p1.y-16}, AP_COL32(130,130,140,200), buf);
                std::string px_param = tree.value("param_x", std::string("Value"));
                dl->AddText({p0.x + cw*0.5f - ImGui::CalcTextSize(px_param.c_str()).x*0.5f, p0.y+4},
                    AP_COL32(150,150,160,180), px_param.c_str());
            }

            // Make the bar interactive: drag to move preview, click diamond to drag threshold.
            ImGui::InvisibleButton("##bt1d", {cw, ch_height});
            bool bar_hov = ImGui::IsItemHovered();
            if (bar_hov) {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    float mx = ImGui::GetMousePos().x - p0.x;
                    _bt_preview_x = tmin + (mx / cw) * range;
                    _bt_preview_x = std::clamp(_bt_preview_x, tmin, tmax);
                }
            }

            // Live preview value display.
            ImGui::SameLine(0,8);
            ImGui::BeginGroup();
            ImGui::TextDisabled("Preview");
            ImGui::SetNextItemWidth(100);
            if (ImGui::DragFloat("##prev1d", &_bt_preview_x, 0.01f, tmin, tmax, "%.3f")) {}
            // Show contributing motions + their weights.
            if (n >= 2) {
                std::string wstr;
                for (int si2 = 0; si2 < n; ++si2) {
                    int  i2  = sorted_idx[si2];
                    float ta = children[i2].value("threshold",0.f);
                    // Unity 1D: weight = piecewise linear blend between neighbours.
                    float w = 0.f;
                    if (n == 1) { w = 1.f; }
                    else if (si2 == 0)       { float tb=children[sorted_idx[1]].value("threshold",0.f); w = 1.f - std::clamp((_bt_preview_x-ta)/(tb-ta+0.0001f),0.f,1.f); }
                    else if (si2 == n-1)     { float tp=children[sorted_idx[n-2]].value("threshold",0.f); w = std::clamp((_bt_preview_x-tp)/(ta-tp+0.0001f),0.f,1.f); }
                    else {
                        float tp=children[sorted_idx[si2-1]].value("threshold",0.f);
                        float tn2=children[sorted_idx[si2+1]].value("threshold",0.f);
                        if (_bt_preview_x <= ta) w = std::clamp((_bt_preview_x-tp)/(ta-tp+0.0001f),0.f,1.f);
                        else                     w = 1.f - std::clamp((_bt_preview_x-ta)/(tn2-ta+0.0001f),0.f,1.f);
                    }
                    if (w > 0.001f) {
                        std::string cn2 = children[i2].value("clip",std::string("?"));
                        char wb[32]; snprintf(wb,sizeof(wb),"%.0f%%", w*100.f);
                        ImGui::TextColored(ImVec4((bar_colours[i2]&0xff)/255.f,((bar_colours[i2]>>8)&0xff)/255.f,((bar_colours[i2]>>16)&0xff)/255.f,1.f),
                            "%s %s", cn2.c_str(), wb);
                    }
                }
            }
            ImGui::EndGroup();

            ImGui::Spacing();
            // Space-Evenly helper button.
            if (n >= 2 && ImGui::SmallButton("Space Evenly")) {
                for (int i2 = 0; i2 < n; ++i2)
                    children[sorted_idx[i2]]["threshold"] = tmin + (float)i2 / (n-1) * range;
                st.undo.push_deep(st.entities);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(distributes thresholds evenly between %.2f - %.2f)", tmin, tmax);
        }

        // ── 2D Visualiser ──────────────────────────────────────────────────
        else {
            int   n  = (int)children.size();
            float sw = std::max(200.f, ImGui::GetContentRegionAvail().x - 4.f);
            float sh = sw * 0.55f;   // slightly wider-than-tall, like Unity's 2D view
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImVec2 p1{ p0.x + sw, p0.y + sh };
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Canvas bg + border
            dl->AddRectFilled(p0, p1, AP_COL32(22,22,26,255), 4.f);
            dl->AddRect(p0, p1, AP_COL32(60,60,70,255), 4.f);

            ImVec2 ctr{ (p0.x+p1.x)*0.5f, (p0.y+p1.y)*0.5f };
            float  scale = std::min(sw, sh) * 0.38f;

            // Grid: major axes + faint grid lines.
            dl->AddLine({ctr.x, p0.y+4}, {ctr.x, p1.y-4}, AP_COL32(255,255,255,25));
            dl->AddLine({p0.x+4, ctr.y}, {p1.x-4, ctr.y}, AP_COL32(255,255,255,25));
            for (int gi = 1; gi <= 4; ++gi) {
                float gx = ctr.x + gi*(scale*0.5f); dl->AddLine({gx,p0.y},{gx,p1.y},AP_COL32(255,255,255,8));
                float gx2= ctr.x - gi*(scale*0.5f); dl->AddLine({gx2,p0.y},{gx2,p1.y},AP_COL32(255,255,255,8));
                float gy = ctr.y + gi*(scale*0.5f); dl->AddLine({p0.x,gy},{p1.x,gy},AP_COL32(255,255,255,8));
                float gy2= ctr.y - gi*(scale*0.5f); dl->AddLine({p0.x,gy2},{p1.x,gy2},AP_COL32(255,255,255,8));
            }

            // Axis labels.
            {
                std::string px = tree.value("param_x", std::string("X"));
                std::string py = tree.value("param_y", std::string("Y"));
                dl->AddText({p1.x - ImGui::CalcTextSize(px.c_str()).x - 4, ctr.y + 4},
                    AP_COL32(160,160,170,200), px.c_str());
                dl->AddText({ctr.x + 4, p0.y + 4},
                    AP_COL32(160,160,170,200), py.c_str());
            }

            // Preview crosshair (red, draggable).
            {
                float rx = ctr.x + _bt_preview_x * scale;
                float ry = ctr.y - _bt_preview_y * scale;
                dl->AddLine({rx, p0.y}, {rx, p1.y}, AP_COL32(220,50,50,160), 1.f);
                dl->AddLine({p0.x, ry}, {p1.x, ry}, AP_COL32(220,50,50,160), 1.f);
                dl->AddCircleFilled({rx,ry}, 5.f, AP_COL32(220,50,50,255));
            }

            // Motion dots.
            const ImU32 dot_colours[] = {
                AP_COL32( 70,130,190,255), AP_COL32( 80,170,100,255),
                AP_COL32(190,120, 60,255), AP_COL32(160, 80,160,255),
                AP_COL32(190,190, 60,255), AP_COL32( 60,170,170,255),
                AP_COL32(210, 80, 80,255), AP_COL32(130,100,200,255),
            };
            ImGui::InvisibleButton("##bt2d", {sw, sh});
            bool canvas_hov = ImGui::IsItemHovered();
            ImVec2 mp = ImGui::GetMousePos();

            // If left-button just released, stop dragging.
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && _bt_dragging >= 0) {
                st.undo.push_deep(st.entities);
                _bt_dragging = -1;
            }

            for (int i = 0; i < n; ++i) {
                Entity& ch = children[i];
                float cx = ch.value("pos_x", 0.f), cy = ch.value("pos_y", 0.f);
                ImVec2 pt{ ctr.x + cx*scale, ctr.y - cy*scale };
                bool   dot_hov = std::hypot(mp.x-pt.x, mp.y-pt.y) < 8.f && canvas_hov;
                ImU32  col = dot_colours[i % 8];

                // Draw influence ripple around dot based on distance-to-preview.
                float dist = std::hypot(_bt_preview_x - cx, _bt_preview_y - cy);
                float rip  = std::max(0.f, 1.f - dist * 0.8f);
                if (rip > 0.01f)
                    dl->AddCircle(pt, 14.f + rip*6.f, (col & 0x00FFFFFFu) | ((ImU32)(rip*100) << 24), 20, 2.f);

                dl->AddCircleFilled(pt, dot_hov ? 9.f : 7.f, col);
                dl->AddCircle(pt, dot_hov ? 9.f : 7.f, AP_COL32(230,230,230,200), 16, 1.5f);
                std::string cn = ch.value("clip", std::string("?"));
                dl->AddText({pt.x + 10, pt.y - 7}, AP_COL32(230,230,230,230), cn.c_str());

                // Drag to reposition.
                if (dot_hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    _bt_dragging = i;
                if (_bt_dragging == i && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    ch["pos_x"] = (mp.x - ctr.x) / scale;
                    ch["pos_y"] = -(mp.y - ctr.y) / scale;
                }
            }

            // Drag the preview crosshair when clicking empty canvas.
            if (canvas_hov && _bt_dragging < 0 && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                _bt_preview_x = (mp.x - ctr.x) / scale;
                _bt_preview_y = -(mp.y - ctr.y) / scale;
            }

            ImGui::Spacing();
            ImGui::SetNextItemWidth(100);
            ImGui::DragFloat("Preview X", &_bt_preview_x, 0.01f, -2.f, 2.f, "%.3f");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            ImGui::DragFloat("Preview Y", &_bt_preview_y, 0.01f, -2.f, 2.f, "%.3f");
            ImGui::SameLine();
            ImGui::TextDisabled("(right-drag canvas to move)");
        }

        // ── Motion list (shared by 1D and 2D) ────────────────────────────
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("Motion List");
        ImGui::Spacing();

        // Column header row.
        float col0 = 180.f, col1 = type=="2d" ? 70.f : 110.f, col2 = 70.f;
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f,0.5f,0.55f,1.f));
        ImGui::Text("%-30s", "  Motion");
        ImGui::SameLine(col0);
        ImGui::TextUnformatted(type=="2d" ? "Pos X" : "Threshold");
        if (type=="2d") { ImGui::SameLine(col0+col1); ImGui::TextUnformatted("Pos Y"); }
        ImGui::PopStyleColor();
        ImGui::Separator();

        int remove_idx = -1;
        int  swap_with = -1;
        int  n = (int)children.size();
        for (int i = 0; i < n; ++i) {
            ImGui::PushID(i);
            Entity& ch = children[i];
            std::string clip = ch.value("clip", std::string());

            // Colour swatch matching the visualiser.
            const ImU32 swatch_cols[] = {
                AP_COL32( 70,130,190,255), AP_COL32( 80,170,100,255),
                AP_COL32(190,120, 60,255), AP_COL32(160, 80,160,255),
                AP_COL32(190,190, 60,255), AP_COL32( 60,170,170,255),
                AP_COL32(210, 80, 80,255), AP_COL32(130,100,200,255),
            };
            ImVec2 sw0 = ImGui::GetCursorScreenPos();
            ImGui::Dummy({8, ImGui::GetFrameHeight()});
            ImGui::GetWindowDrawList()->AddRectFilled(sw0, {sw0.x+6, sw0.y+ImGui::GetFrameHeight()-2},
                swatch_cols[i%8], 2.f);
            ImGui::SameLine(0, 4);

            ImGui::SetNextItemWidth(col0 - 14);
            if (ImGui::BeginCombo("##clip", clip.empty() ? "(none)" : clip.c_str())) {
                for (auto& cn : clip_names) {
                    if (cn == state_name) continue;
                    if (ImGui::Selectable(cn.c_str(), cn==clip)) { ch["clip"]=cn; st.undo.push_deep(st.entities); }
                }
                ImGui::EndCombo();
            }

            ImGui::SameLine(col0);
            if (type == "2d") {
                float px = ch.value("pos_x", 0.f), py = ch.value("pos_y", 0.f);
                ImGui::SetNextItemWidth(col1 - 4);
                if (ImGui::DragFloat("##px", &px, 0.01f, -10.f, 10.f, "%.2f")) ch["pos_x"] = px;
                if (ImGui::IsItemDeactivatedAfterEdit()) st.undo.push_deep(st.entities);
                ImGui::SameLine(col0 + col1);
                ImGui::SetNextItemWidth(col2 - 4);
                if (ImGui::DragFloat("##py", &py, 0.01f, -10.f, 10.f, "%.2f")) ch["pos_y"] = py;
                if (ImGui::IsItemDeactivatedAfterEdit()) st.undo.push_deep(st.entities);
            } else {
                float thr = ch.value("threshold", 0.f);
                ImGui::SetNextItemWidth(col1 - 4);
                if (ImGui::DragFloat("##thr", &thr, 0.01f, -100.f, 100.f, "%.3f")) ch["threshold"] = thr;
                if (ImGui::IsItemDeactivatedAfterEdit()) st.undo.push_deep(st.entities);
            }

            // Up/down reorder arrows + remove button.
            float right = ImGui::GetWindowWidth();
            ImGui::SameLine(right - 72);
            if (ImGui::SmallButton("^") && i > 0)   swap_with = i - 1;
            ImGui::SameLine();
            if (ImGui::SmallButton("v") && i < n-1) swap_with = i;
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) remove_idx = i;

            ImGui::PopID();
        }

        // Apply reorder.
        if (swap_with >= 0 && swap_with < n-1) {
            std::swap(children[swap_with], children[swap_with+1]);
            st.undo.push_deep(st.entities);
        }
        // Apply remove.
        if (remove_idx >= 0) {
            Entity kept = Entity::array();
            for (int i = 0; i < n; ++i) if (i != remove_idx) kept.push_back(children[i]);
            tree["children"] = kept;
            st.undo.push_deep(st.entities);
        }

        ImGui::Spacing();
        if (ImGui::Button("+ Add Motion Field")) {
            Entity ch = Entity::object();
            ch["clip"]      = clip_names.empty() ? std::string() : clip_names.front();
            ch["threshold"] = (float)children.size() * 0.5f; // incremental default
            ch["pos_x"]     = 0.f;
            ch["pos_y"]     = 0.f;
            children.push_back(ch);
            st.undo.push_deep(st.entities);
        }
    }

public:
    // ── Layers (separate dockable window, Unity keeps Layers as a small
    // top-left widget inside the Animator window; here it's its own window so
    // it can be docked next to the main Animator without crowding it) ──────────
    void draw_layers(EditorState& st) {
        if (!_layers_open) return;
        ImGui::SetNextWindowSize({320, 260}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Animator Layers##win", &_layers_open)) { ImGui::End(); return; }

        Entity* e = st.selected_entity();
        if (!e || !has_component(*e, "Animator")) {
            ImGui::TextDisabled("Select an entity with an Animator.");
            ImGui::End();
            return;
        }
        Entity& anim = (*e)["components"]["Animator"];
        auto& layers = anim_json::layers(anim);

        ImGui::TextUnformatted("Base Layer");
        ImGui::SameLine(ImGui::GetWindowWidth()-70);
        ImGui::TextDisabled("1.00");
        ImGui::Separator();

        int remove_idx = -1;
        for (int i = 0; i < (int)layers.size(); ++i) {
            ImGui::PushID(i);
            Entity& l = layers[i];
            char name_buf[64];
            std::string nm = l.value("name", std::string("Layer"));
            strncpy(name_buf, nm.c_str(), sizeof(name_buf)-1); name_buf[sizeof(name_buf)-1]=0;
            ImGui::SetNextItemWidth(110);
            if (ImGui::InputText("##name", name_buf, sizeof(name_buf))) l["name"] = std::string(name_buf);
            if (ImGui::IsItemDeactivatedAfterEdit()) st.undo.push_deep(st.entities);

            ImGui::SameLine();
            float w = l.value("weight", 1.f);
            ImGui::SetNextItemWidth(80);
            if (ImGui::SliderFloat("##weight", &w, 0.f, 1.f)) l["weight"] = w;
            if (ImGui::IsItemDeactivatedAfterEdit()) st.undo.push_deep(st.entities);

            ImGui::SameLine();
            if (ImGui::SmallButton("x")) remove_idx = i;

            std::string lcur = l.value("current_animation", std::string());
            auto state_names = anim_json::all_state_names(anim);
            ImGui::SetNextItemWidth(200);
            if (ImGui::BeginCombo("Clip", lcur.empty() ? "(none)" : lcur.c_str())) {
                for (auto& sn : state_names) {
                    if (anim_json::is_blend_tree(anim, sn)) continue; // layers play flat clips only
                    if (ImGui::Selectable(sn.c_str(), sn==lcur)) {
                        l["current_animation"] = sn; l["frame"] = 0.f; l["playing"] = true;
                        st.undo.push_deep(st.entities);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::Separator();
            ImGui::PopID();
        }
        if (remove_idx >= 0) {
            Entity kept = Entity::array();
            for (int i = 0; i < (int)layers.size(); ++i) if (i != remove_idx) kept.push_back(layers[i]);
            anim["layers"] = kept;
            st.undo.push_deep(st.entities);
        }
        if (ImGui::Button("+ Add Layer")) {
            Entity l = Entity::object();
            l["name"] = std::string("Layer ") + std::to_string(layers.size()+1);
            l["weight"] = 1.f;
            l["current_animation"] = std::string();
            l["frame"] = 0.f;
            l["playing"] = false;
            layers.push_back(l);
            st.undo.push_deep(st.entities);
        }
        ImGui::End();
    }
    bool* layers_open_flag() { return &_layers_open; }

private:
    bool _layers_open = false;
    // Blend tree editor preview state
    float _bt_preview_x = 0.f;
    float _bt_preview_y = 0.f;
    int   _bt_dragging  = -1;
};
