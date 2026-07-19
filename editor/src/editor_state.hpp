#pragma once
/*
 * editor_state.hpp — Shared state struct passed to every editor panel.
 *
 * Mirrors EditorApp in editor_main.py:
 *   entities, selected_entity_id, clipboard, scene_path,
 *   grid_snap, paused, undo stack, console log buffer.
 */

#include "../engine_cpp/entity.hpp"
#include "../engine_cpp/transform_system.hpp"
#include "component_defs.hpp"
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <functional>
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <mutex>

// ─── Undo ────────────────────────────────────────────────────────────────────
struct UndoManager {
    std::deque<EntityList> _undo, _redo;
    static constexpr int MAX = 50;

    void push(const EntityList& state) {
        _undo.push_front(state);
        if ((int)_undo.size() > MAX) _undo.pop_back();
        _redo.clear();
    }
    // Use this instead of push() whenever the snapshot needs to be truly
    // independent (property edits via gizmo drag, inspector field changes).
    // Plain push() shallow-copies the shared_ptr maps, so Ctrl+Z on a
    // move/rotate/scale would restore the already-mutated position.
    void push_deep(const EntityList& state) {
        EntityList deep;
        deep.reserve(state.size());
        for (const auto& e : state)
            deep.push_back(e.deep_clone());
        _undo.push_front(std::move(deep));
        if ((int)_undo.size() > MAX) _undo.pop_back();
        _redo.clear();
    }
    bool can_undo() const { return !_undo.empty(); }
    bool can_redo() const { return !_redo.empty(); }
    EntityList undo(const EntityList& cur) {
        if (_undo.empty()) return cur;
        _redo.push_front(cur);
        auto s = _undo.front(); _undo.pop_front(); return s;
    }
    EntityList redo(const EntityList& cur) {
        if (_redo.empty()) return cur;
        _undo.push_front(cur);
        auto s = _redo.front(); _redo.pop_front(); return s;
    }
};

// ─── Console log entry ───────────────────────────────────────────────────────
enum class LogLevel { Info, Warn, Error, Success, Engine };

struct LogEntry {
    std::string timestamp;
    LogLevel    level;
    std::string message;
};

// ─── EditorState ─────────────────────────────────────────────────────────────
struct EditorState {
    // Scene
    std::string  scene_path  = "games/abyss-of-hollows/scene.json";
    EntityList   entities;
    int          selected_id = -1;
    std::vector<int> selected_ids;
    EntityList   clipboard;

    // Viewport camera
    float cam_x = 0.f, cam_y = 0.f, cam_zoom = 1.f;

    // Tools
    std::string  tool = "select";   // select|move|rotate|scale|paint
    bool  show_grid   = true;
    bool  snap        = false;
    int   grid_snap   = 0;          // 0=off else px
    int   grid_size   = 32;
    int   paint_tile  = 0;

    // ── Tile painting extras ────────────────────────────────────────────────
    bool paint_erase      = false;  // true = paint -1 (erase)
    int  paint_brush_size = 1;      // NxN brush (1=single tile, 2=2x2, 3=3x3 …)
    bool paint_rect_mode  = false;  // true = drag to fill a rectangle
    bool paint_eyedropper = false;  // true = next click picks tile id from map
    // Rect-fill drag state (set on mouse-down, consumed on mouse-up)
    bool  _tile_rect_dragging = false;
    int   _tile_rect_col0 = 0, _tile_rect_row0 = 0; // drag start in tile coords

    // Unity-style palettes paint a named pattern, not merely an NxN repeat of
    // one atlas index.  `paint_tile` remains the compatibility/default single
    // tile while these cells describe the active saved brush around its origin.
    struct TileBrushCell { int x = 0, y = 0, tile_id = 0; };
    std::string active_tile_palette;
    std::string active_tile_brush;
    std::vector<TileBrushCell> paint_brush_cells;
    int paint_brush_rotation = 0;
    bool paint_brush_flip_x = false;
    bool paint_brush_flip_y = false;

    // Named tile presets: each preset is a named int (a single tile id) the
    // user saved so they can quickly switch between frequently-used tiles.
    // Presets capture the complete paint source, not only an atlas index. A
    // tile id has no meaning without its tileset, which was why selecting a
    // preset could appear to keep painting the old texture.
    struct TilePreset {
        std::string name;
        int tile_id = 0;
        std::string tileset;
        int tile_size = 32;
    };
    std::vector<TilePreset> tile_presets;

    // Requests are raised by the Tilemap Inspector/Assets panel and consumed
    // by TilePalettePanel in the main editor loop.  Keeping this in shared
    // state avoids a fragile cross-panel pointer and lets Asset double-clicks
    // open the correct palette too.
    bool request_tile_palette_open = false;
    std::string requested_tile_palette_asset;

    // Play state
    bool  playing  = false;
    bool  paused   = false;
    bool  step_once= false;
    EntityList scene_snapshot;   // saved before play
    // Canonical on-disk authoring state captured after a successful load/save.
    // SceneIO compares this only when the user closes the Editor, which makes
    // close protection complete without every panel maintaining a fragile
    // independent dirty flag.
    std::string saved_scene_fingerprint;

    // ── Real per-frame subsystem timings (ms), measured every frame in
    // ViewportPanel::draw() with std::chrono around each system's update()
    // call (scripts/coroutines, physics, animator/particles/audio, render).
    // ProfilerPanel (unity_gap_features.hpp) reads these directly instead of
    // showing made-up numbers — see frame_render_ms etc. below. All zero
    // while not playing, since most of these systems don't run in edit mode.
    float frame_script_ms  = 0.f;
    float frame_physics_ms = 0.f;
    float frame_render_ms  = 0.f;
    float frame_other_ms   = 0.f; // animator + particles + audio + transform + events

    // Menu-bar play/stop button flags (set by menu bar, consumed by viewport draw)
    bool _menubar_play_clicked = false;
    bool _menubar_stop_clicked = false;

    // Component clipboard (Copy/Paste Component Values in Inspector right-click)
    std::string _comp_clipboard_type;
    nlohmann::json _comp_clipboard_data;

    // Console
    //
    // THREAD SAFETY: `logs` is written from log()/log_warn()/log_error()/
    // log_engine(), which are reachable from background threads — most
    // notably Debug::set_log_callback() (see editor_main.cpp), which routes
    // AutoHotReload's FileWatcher thread and its build thread's status
    // messages straight into these calls. Meanwhile ConsolePanel::draw()
    // iterates `logs` on the main thread every single frame. A raw
    // std::deque has no built-in synchronization, so without a lock this
    // is a data race: push_back()/pop_front() from a background thread can
    // run concurrently with the main thread's range-for over the same
    // deque, which can corrupt the deque's internal block list mid-read.
    // This was the actual cause of the editor freezing/crashing during
    // script hot-reload — every "Rebuilding...", "Watching...", or build
    // failure message logged from AutoHotReload's threads raced against
    // the Console panel redrawing that same frame, and more UI activity
    // (e.g. clicking around) simply increased how often the race was hit.
    //
    // Fix: `_logs_mutex` guards every access to `logs`. Use log()/log_warn()/
    // etc. to write (thread-safe) and logs_snapshot() to read (also
    // thread-safe) instead of touching the deque directly from panel code.
    mutable std::mutex   _logs_mutex;
    std::deque<LogEntry> logs;
    bool  con_autoscroll = true;
    std::string con_filter;

    // Undo
    UndoManager undo;

    // Asset dir
    std::string asset_dir = "games/abyss-of-hollows/assets";

    // Inspector-to-graph-editor handoff. Keeping this in shared editor state
    // avoids a panel owning another panel and lets a VisualScript component
    // open the exact graph asset it references.
    bool request_visual_script_open = false;
    std::string requested_visual_script_asset;
    int requested_visual_script_entity_id = -1;

    // Asset-to-editor handoffs.  Keeping these in the shared state lets the
    // Inspector, Assets grid, and context menus open the same dedicated
    // editor without panels owning each other.
    bool request_sprite_editor_open = false;
    std::string requested_sprite_editor_asset;
    bool request_prefab_stage_open = false;
    std::string requested_prefab_stage_asset;
    bool request_shader_graph_open = false;
    std::string requested_shader_graph_asset;

    // Project Settings owns the user's build choices, while ViewportPanel
    // owns the export implementation. These one-shot requests bridge that
    // boundary so Build and Build & Run are real actions instead of asking
    // the user to find an unrelated toolbar button.
    bool request_standalone_build = false;
    bool request_standalone_build_and_run = false;

    // ── Selected asset (Unity Project-window style) ─────────────────────────
    // Absolute path of the asset file currently selected in the Assets
    // panel, or empty if none. Selecting an asset shows its properties in
    // the Inspector — same relationship as Unity's Project window driving
    // the Inspector when nothing in the Hierarchy is selected. Entity
    // selection (selected_id) and asset selection are independent; the
    // Inspector simply prefers whichever was clicked most recently (see
    // select()/select_asset()/clear_selection() below).
    std::string selected_asset_path;
    bool        asset_selection_is_newer = false;

    void select_asset(const std::string& path) {
        selected_asset_path = path;
        asset_selection_is_newer = true;
    }
    void clear_asset_selection() { selected_asset_path.clear(); }

    // ── Sorting Layers (Unity2D-style) ──────────────────────────────────────
    // Named, ordered render layers shared project-wide. Index 0 draws first
    // (furthest back); later layers draw on top. SpriteRenderer/Tilemap/
    // SortingGroup components reference one of these by name via their
    // "sorting_layer" field, with "order_in_layer" breaking ties within a
    // layer — exactly Unity's two-tier sorting model. Saved/loaded as part of
    // the scene file (see scene_io.hpp) so each project can define its own.
    std::vector<std::string> sorting_layers = {"Background","Default","Foreground","UI"};

    bool add_sorting_layer(const std::string& name) {
        if (name.empty()) return false;
        if (std::find(sorting_layers.begin(),sorting_layers.end(),name) != sorting_layers.end()) return false;
        sorting_layers.push_back(name);
        return true;
    }
    void remove_sorting_layer(const std::string& name) {
        // "Default" always stays — every legacy scene/component implicitly
        // relies on an empty sorting_layer field resolving to *something*.
        if (name == "Default") return;
        sorting_layers.erase(std::remove(sorting_layers.begin(), sorting_layers.end(), name), sorting_layers.end());
    }
    bool move_sorting_layer(int index, int new_index) {
        if (index < 0 || index >= (int)sorting_layers.size()) return false;
        new_index = std::max(0, std::min(new_index, (int)sorting_layers.size()-1));
        if (index == new_index) return true;
        std::string v = sorting_layers[index];
        sorting_layers.erase(sorting_layers.begin()+index);
        sorting_layers.insert(sorting_layers.begin()+new_index, v);
        return true;
    }

    // Recent scenes
    std::vector<std::string> recent_scenes;

    // Per-project default scene (Unity-style "the scene Build uses, no matter
    // what's open in the editor"). Keyed by project name (e.g. "game4"), since
    // each project under games/ has its own independent default. Empty/missing
    // entry means "fall back to whatever scene is currently open", preserving
    // old behavior for projects that haven't set one yet.
    std::unordered_map<std::string, std::string> default_scene_by_project;

    std::string default_scene_for(const std::string& project_name) const {
        auto it = default_scene_by_project.find(project_name);
        return it != default_scene_by_project.end() ? it->second : std::string();
    }
    void set_default_scene_for(const std::string& project_name, const std::string& scene_path) {
        default_scene_by_project[project_name] = scene_path;
    }

    // ── Helpers ───────────────────────────────────────────────────────────────
    Entity* find_entity(int id) {
        for (auto& e : entities) if (e.value("id",0)==id) return &e;
        return nullptr;
    }
    const Entity* find_entity(int id) const {
        for (auto& e : entities) if (e.value("id",0)==id) return &e;
        return nullptr;
    }
    Entity* selected_entity() { return find_entity(selected_id); }

    // ── Hierarchy helpers ────────────────────────────────────────────────────
    static int parent_of(const Entity& e) {
        if (!e.contains("components") || !e["components"].contains("Transform")) return -1;
        auto& tr = e["components"]["Transform"];
        if (!tr.contains("parent") || !tr["parent"].is_number_integer()) return -1;
        return tr["parent"].get<int>();
    }

    std::vector<int> children_of(int id) const {
        std::vector<int> out;
        for (auto& e : entities)
            if (parent_of(e) == id) out.push_back(e.value("id",0));
        return out;
    }

    // True if `candidate` is `ancestor`, or a descendant of it.
    bool is_descendant_of(int candidate, int ancestor) const {
        int cur = candidate;
        int guard = 0;
        while (cur >= 0 && guard++ < 10000) {
            if (cur == ancestor) return true;
            const Entity* p = find_entity(cur);
            if (!p) return false;
            cur = parent_of(*p);
        }
        return false;
    }

    // Refreshes every entity's legacy top-level "children" array (present in
    // older scene files but never actually read by any engine/editor code —
    // Transform.parent is the single source of truth for the hierarchy) so
    // saved scenes stay self-consistent for any external tooling that might
    // still look at it. Cheap (O(n)); safe to call after any reparent/
    // delete/duplicate/load.
    void resync_children_arrays() {
        std::unordered_map<int, nlohmann::json> kids;
        for (auto& e : entities) kids[e.value("id",0)] = nlohmann::json::array();
        for (auto& e : entities) {
            int pid = parent_of(e);
            if (pid >= 0 && kids.count(pid)) kids[pid].push_back(e.value("id",0));
        }
        for (auto& e : entities) e["children"] = kids[e.value("id",0)];
    }

    int entity_index(int id) const {
        for (int i = 0; i < (int)entities.size(); ++i)
            if (entities[i].value("id",0) == id) return i;
        return -1;
    }

    bool move_entity_to_index(int id, int new_index) {
        int old_index = entity_index(id);
        if (old_index < 0) return false;
        if (entities.empty()) return false;

        const int count_before = (int)entities.size();
        new_index = std::max(0, std::min(new_index, count_before));
        if (old_index == new_index || old_index + 1 == new_index) return true;

        Entity moving = std::move(entities[old_index]);
        entities.erase(entities.begin() + old_index);

        if (new_index > old_index) --new_index;
        if (new_index >= (int)entities.size()) entities.push_back(std::move(moving));
        else entities.insert(entities.begin() + new_index, std::move(moving));
        return true;
    }

    bool move_entity_before(int id, int target_id) {
        if (id == target_id) return false;
        int target_index = entity_index(target_id);
        if (target_index < 0) return false;
        return move_entity_to_index(id, target_index);
    }

    bool move_entity_after(int id, int target_id) {
        if (id == target_id) return false;
        int target_index = entity_index(target_id);
        if (target_index < 0) return false;
        return move_entity_to_index(id, target_index + 1);
    }

    enum class HierarchyDropMode { Before, After, Inside };

    // Reparents `id` under `new_parent_id` (-1 = make root), preserving
    // world-space position/rotation/scale by default (Unity Hierarchy drag
    // behavior). Refuses and returns false if this would create a cycle.
    bool reparent(int id, int new_parent_id, bool keep_world_position = true) {
        if (id == new_parent_id) return false;
        Entity* e = find_entity(id);
        if (!e) return false;
        if (new_parent_id >= 0 && is_descendant_of(new_parent_id, id)) return false; // cycle

        transform::WorldTRS world_before = transform::cached_world(*e);

        if (!has_component(*e, "Transform"))
            (*e)["components"]["Transform"] = component_defaults()["Transform"];
        auto& tr = (*e)["components"]["Transform"];

        if (new_parent_id < 0) {
            tr["parent"] = -1;
            if (keep_world_position) {
                tr["x"] = world_before.x; tr["y"] = world_before.y;
                tr["rotation"] = world_before.rotation;
                tr["scale_x"] = world_before.scale_x; tr["scale_y"] = world_before.scale_y;
            }
            resync_children_arrays();
            transform::mark_structure_dirty();
            return true;
        }

        Entity* np = find_entity(new_parent_id);
        if (!np) return false;
        tr["parent"] = new_parent_id;
        if (keep_world_position) {
            transform::WorldTRS parent_world = transform::cached_world(*np);
            transform::WorldTRS local = transform::world_to_local(parent_world, world_before);
            tr["x"] = local.x; tr["y"] = local.y;
            tr["rotation"] = local.rotation;
            tr["scale_x"] = local.scale_x; tr["scale_y"] = local.scale_y;
        }
        resync_children_arrays();
        transform::mark_structure_dirty();
        return true;
    }

    bool reparent_relative(int id, int target_id, HierarchyDropMode mode, bool keep_world_position = true) {
        if (id == target_id) return false;
        if (mode == HierarchyDropMode::Inside) {
            return reparent(id, target_id, keep_world_position);
        }

        Entity* target = find_entity(target_id);
        if (!target) return false;
        int new_parent_id = parent_of(*target);
        if (new_parent_id >= 0 && is_descendant_of(new_parent_id, id)) return false;

        if (!reparent(id, new_parent_id, keep_world_position)) return false;

        bool moved = true;
        if (mode == HierarchyDropMode::Before)
            moved = move_entity_before(id, target_id);
        else if (mode == HierarchyDropMode::After)
            moved = move_entity_after(id, target_id);

        resync_children_arrays();
        transform::mark_structure_dirty();
        return moved;
    }

    void select(int id) {
        selected_id = id;
        if (std::find(selected_ids.begin(),selected_ids.end(),id)==selected_ids.end())
            selected_ids = {id};
        asset_selection_is_newer = false;
    }
    void clear_selection() { selected_id=-1; selected_ids.clear(); }

    // Safe to call from ANY thread (main thread or a background thread like
    // AutoHotReload's watcher/build threads) — see the comment on `logs`
    // above for why this needs the lock.
    void log(const std::string& msg, LogLevel lv = LogLevel::Info) {
        time_t now = time(nullptr);
        // `localtime()` returns a shared static tm buffer. Hot-reload worker
        // logs can arrive while the main thread logs a UI action, so using it
        // here was still a small race even though the deque itself is locked.
        std::tm local_time{};
#if defined(_WIN32)
        localtime_s(&local_time, &now);
#else
        localtime_r(&now, &local_time);
#endif
        char buf[16]; strftime(buf, sizeof(buf), "%H:%M:%S", &local_time);
        std::lock_guard<std::mutex> lk(_logs_mutex);
        logs.push_back({buf, lv, msg});
        if ((int)logs.size() > 500) logs.pop_front();
    }
    void log_warn   (const std::string& m){ log(m, LogLevel::Warn); }
    void log_error  (const std::string& m){ log(m, LogLevel::Error); }
    void log_success(const std::string& m){ log(m, LogLevel::Success); }
    void log_engine (const std::string& m){ log(m, LogLevel::Engine); }

    // Thread-safe read access for panel code. Returns a copy rather than a
    // reference so ConsolePanel (or anything else) can iterate freely on
    // the main thread without holding the lock while a background thread
    // might be appending — copying a <=500-entry deque of small strings
    // once per frame is cheap and removes the race entirely.
    std::deque<LogEntry> logs_snapshot() const {
        std::lock_guard<std::mutex> lk(_logs_mutex);
        return logs;
    }
    void logs_clear() {
        std::lock_guard<std::mutex> lk(_logs_mutex);
        logs.clear();
    }

    // ─── Path helpers ─────────────────────────────────────────────────────────
    // All relative paths are resolved from the project root (the directory that
    // contains engine_cpp/, editor/, games/, scripts/, export/, etc.).
    // editor_main.cpp performs a chdir to that root before any path is used,
    // so bare relative strings resolve from the engine root.

    // Returns the top-level games/ directory (project_root/games).
    static std::filesystem::path games_root() {
        return std::filesystem::path("games");
    }

    // Derive asset_dir from scene_path
    void update_asset_dir() {
        namespace fs = std::filesystem;
        asset_dir = (fs::path(scene_path).parent_path() / "assets").string();
    }

    // Add to recent list
    void add_recent(const std::string& path) {
        auto it = std::find(recent_scenes.begin(),recent_scenes.end(),path);
        if (it != recent_scenes.end()) recent_scenes.erase(it);
        recent_scenes.insert(recent_scenes.begin(), path);
        if ((int)recent_scenes.size() > 8) recent_scenes.resize(8);
    }

    // New entity ID
    int next_id() {
        int max_id = 0;
        for (auto& e : entities) max_id = std::max(max_id, e.value("id",0));
        return max_id + 1;
    }
};

// Derive a stable per-project identifier from a scene path, e.g.
// "games/game4/scene.json" -> "game4". File-scope (not a class member) and
// included before both panels.hpp and editor_main.cpp use it, so it's
// visible everywhere consistently — MSVC's stricter lookup rejected the
// previous copy of this logic when it ended up nested inside ViewportPanel
// instead of at file scope (C3861: identifier not found), even though g++
// didn't complain. Keep this the single definition; anything else needing
// "the project name for this scene path" should call this instead of
// re-deriving it locally.
inline std::string project_name_from_scene_path(const std::filesystem::path& scene_path) {
    namespace fs = std::filesystem;
    std::string gen = fs::absolute(scene_path).generic_string();
    const std::string marker = "games/";
    auto pos = gen.find(marker);
    if (pos != std::string::npos) {
        std::string rest = gen.substr(pos + marker.size());
        auto slash = rest.find('/');
        if (slash != std::string::npos) return rest.substr(0, slash);
    }
    std::string stem = scene_path.stem().string();
    return stem.empty() ? "export" : stem;
}
