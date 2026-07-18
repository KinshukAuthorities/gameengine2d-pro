#pragma once
/*
 * feature_systems.hpp — TilemapColliderSystem, ParallaxSystem, EventSystem,
 *                        UILayoutSystem
 */

#include "entity.hpp"
#include "transform_system.hpp"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <vector>
#include <queue>
#include <algorithm>
#include <cmath>
#include <climits>
#include <filesystem>
#include <fstream>
#include <stdexcept>

inline constexpr float GAMEENGINE_PI = 3.14159265358979323846f;

// Advance delayed destruction in engine code so an effect cannot live
// forever merely because its optional gameplay script is reloading.
inline void advance_destroy_timers(EntityList& entities, float simulation_dt) {
    if (simulation_dt <= 0.0f) return;
    for (auto& entity : entities) {
        if (entity.value("_destroyed", false) || !entity.contains("_destroy_timer")) continue;
        if (!entity["_destroy_timer"].is_number()) {
            entity.erase("_destroy_timer");
            continue;
        }
        const float remaining = entity["_destroy_timer"].get<float>() - simulation_dt;
        if (remaining <= 0.0f) {
            entity.erase("_destroy_timer");
            entity["_destroyed"] = true;
        } else {
            entity["_destroy_timer"] = remaining;
        }
    }
}

// ─── EntityRef ───────────────────────────────────────────────────────────────────
// Transparent proxy for Entity* that kills the `*entity` / `entity->` pattern.
// All entity-level operations live on EntityRef directly.
struct EntityRef {
    Entity* _ptr = nullptr;

    EntityRef() = default;
    EntityRef(std::nullptr_t) {}
    EntityRef(Entity* p) : _ptr(p) {}
    EntityRef(const EntityRef&) = default;
    EntityRef& operator=(const EntityRef&) = default;
    EntityRef(Entity& e) : _ptr(&e) {}

    // Lowercase aliases for scripting convenience (match Entity API style).
    bool contains(const std::string& key) const { return Contains(key); }
    std::string value(const std::string& key, const char* def) const {
        return _ptr ? _ptr->value(key, std::string(def)) : def;
    }
    template <class T>
    T value(const std::string& key, T def) const { return Value(key, def); }

    Entity* operator->() const { return _ptr; }
    Entity& operator*() const { return *_ptr; }
    explicit operator bool() const { return _ptr != nullptr; }
    explicit operator Entity*() const { return _ptr; }
    bool operator==(const EntityRef& o) const { return _ptr == o._ptr; }
    bool operator!=(const EntityRef& o) const { return _ptr != o._ptr; }
    bool operator==(std::nullptr_t) const { return _ptr == nullptr; }
    bool operator!=(std::nullptr_t) const { return _ptr != nullptr; }

    // Direct entity access operators.
    Entity& operator[](const std::string& key) { return (*_ptr)[key]; }
    const Entity& operator[](const std::string& key) const { return (*_ptr)[key]; }

    // Safe existence check.
    bool Contains(const std::string& key) const { return _ptr && _ptr->contains(key); }

    // Safe typed read.
    template <class T>
    T Value(const std::string& key, T def) const {
        return _ptr ? _ptr->value(key, def) : def;
    }

    // Safe typed write.
    template <class T>
    void SetValue(const std::string& key, T val) {
        if (_ptr) (*_ptr)[key] = val;
    }

    // Component field write.
    template <class T>
    void SetComponent(const std::string& comp, const std::string& field, T val) {
        if (!_ptr || !_ptr->contains("components")) return;
        if (!(*_ptr)["components"].contains(comp)) return;
        (*_ptr)["components"][comp][field] = val;
    }

    // Component field read.
    template <class T>
    T ComponentValue(const std::string& comp, const std::string& field, T def) const {
        if (!_ptr || !_ptr->contains("components")) return def;
        if (!(*_ptr)["components"].contains(comp)) return def;
        return (*_ptr)["components"][comp].value(field, def);
    }

    // Destroy.
    void Destroy() { if (_ptr) (*_ptr)["_destroyed"] = true; }
    void DestroyWithDelay(float delay) {
        if (_ptr) (*_ptr)["_destroy_timer"] = _ptr->value("_destroy_timer", 0.f) + delay;
    }

    // Active state.
    void SetActive(bool v) { if (_ptr) (*_ptr)["active"] = v; }
    bool IsActive() const { return _ptr && _ptr->value("active", true); }
    void Show() { SetActive(true); }
    void Hide() { SetActive(false); }

    // Tag.
    void SetTag(const std::string& t) { if (_ptr) (*_ptr)["tag"] = t; }
    bool CompareTag(const std::string& t) const {
        if (!_ptr) return false;
        if (_ptr->value("tag", "") == t) return true;
        if (_ptr->contains("tags") && (*_ptr)["tags"].is_array()) {
            for (auto& tg : (*_ptr)["tags"])
                if (tg.is_string() && tg.get<std::string>() == t) return true;
        }
        return false;
    }
    std::string Tag() const {
        if (!_ptr) return "";
        if (_ptr->contains("tag") && (*_ptr)["tag"].is_string()) return (*_ptr)["tag"].get<std::string>();
        if (_ptr->contains("tags") && (*_ptr)["tags"].is_array()) {
            for (auto& tg : (*_ptr)["tags"]) if (tg.is_string()) return tg.get<std::string>();
        }
        return "";
    }

    // Erase a key from the entity.
    void Erase(const std::string& key) { if (_ptr) _ptr->erase(key); }
};

// ─── Simple EventBus ──────────────────────────────────────────────────────────
class EventBus {
public:
    using Handler = std::function<void(EntityRef, EntityRef)>;

    struct Registration { void* owner = nullptr; Handler handler; };
    // EventBus is included by both editor.exe and every hot-reload script
    // DLL. Keep the actual registrations in an explicitly bindable State so
    // every module talks to one process-wide event stream instead of each DLL
    // accidentally creating its own private EventBus.
    struct State {
        std::unordered_map<std::string, std::vector<Registration>> handlers;
        void* active_subscription_owner = nullptr;
    };

    static State& _local_state() { static State s; return s; }
    static State*& _state_ptr() { static State* p = nullptr; return p; }
    static State& _state() { return _state_ptr() ? *_state_ptr() : _local_state(); }
    static void bind_state(State* host) { _state_ptr() = host; }

    static EventBus& instance() { static EventBus inst; return inst; }

    void subscribe(const std::string& name, Handler h) {
        // ScriptSystem sets this while dispatching every script callback.
        // That makes existing EventBus::subscribe("name", [this] {...})
        // source automatically safe without requiring every game script to
        // remember a separate owner argument.
        _state().handlers[name].push_back({_state().active_subscription_owner, std::move(h)});
    }
    // Same as subscribe(), but tags the registration with `owner` (typically
    // `this` from inside a script). Pairs with unsubscribe_all(owner) below —
    // call that from on_destroy() so a script that subscribed in Awake()
    // never has its destroyed `this` dangling-referenced by a lambda the
    // next time this event fires. See MonoBehaviour's bridging in
    // unity2d_script_api.hpp, which calls unsubscribe_all(this)
    // automatically from on_destroy() so callers don't have to remember to.
    void subscribe(const std::string& name, void* owner, Handler h) {
        _state().handlers[name].push_back({owner, std::move(h)});
    }

    static void set_subscription_owner(void* owner) { _state().active_subscription_owner = owner; }
    static void* subscription_owner() { return _state().active_subscription_owner; }

    void emit(const std::string& name, const Entity& data, Entity* source=nullptr) {
        auto& state = _state();
        auto it = state.handlers.find(name);
        if (it==state.handlers.end()) return;
        // A callback may destroy an entity, unsubscribe itself, or subscribe
        // another handler. Iterate a snapshot so those valid mutations never
        // invalidate the vector currently being dispatched.
        const auto registrations = it->second;
        EntityRef data_ref(const_cast<Entity*>(&data));
        EntityRef src_ref(source);
        struct OwnerRestore {
            State& state;
            void* previous;
            ~OwnerRestore() { state.active_subscription_owner = previous; }
        } restore{state, state.active_subscription_owner};
        for (const auto& reg : registrations) {
            if (!reg.handler) continue;
            state.active_subscription_owner = reg.owner;
            reg.handler(data_ref, src_ref);
        }
    }

    // Removes every handler registered with the given owner token, across
    // ALL event names — not just one. O(handlers), fine for the scale this
    // is used at (called once per destroyed script instance, not per frame).
    void unsubscribe_all(void* owner) {
        if (!owner) return;
        for (auto& [name, regs] : _state().handlers) {
            regs.erase(std::remove_if(regs.begin(), regs.end(),
                [owner](const Registration& r){ return r.owner == owner; }),
                regs.end());
        }
    }

    void clear(const std::string& name) { _state().handlers.erase(name); }
    void clear_all() {
        _state().handlers.clear();
        _state().active_subscription_owner = nullptr;
    }

    // ── Typed events ─────────────────────────────────────────────────────────
    // Unity-style typed pub/sub on top of the same raw string+Entity bus
    // above — fully backwards compatible with existing emit("name", data)
    // call sites, since typed events are just sugar that serializes T
    // to/from an Entity using the same field names a raw subscriber would
    // see. Requires T to provide:
    //   static const char* EventName();      // bus event name, e.g. "PlayerHit"
    //   Entity ToEntity() const;              // pack fields into an Entity
    //   static T FromEntity(const Entity& e); // unpack
    // (Most structs satisfy this with a few lines — see the roadmap example
    // PlayerHitEvent for the pattern.) This keeps the typed layer additive
    // and zero-magic: no reflection, no codegen, just one small struct per
    // event type, same as Unity's [Serializable] event classes.
    template <class T>
    using TypedHandler = std::function<void(const T&)>;

    template <class T>
    void Subscribe(TypedHandler<T> handler) {
        subscribe(T::EventName(), [handler](EntityRef e, EntityRef /*source*/) {
            if (handler && e) handler(T::FromEntity(*e));
        });
    }
    // Owner-tagged overload — see subscribe(name, owner, handler) above.
    template <class T>
    void Subscribe(void* owner, TypedHandler<T> handler) {
        subscribe(T::EventName(), owner, [handler](EntityRef e, EntityRef /*source*/) {
            if (handler && e) handler(T::FromEntity(*e));
        });
    }
    template <class T>
    void Emit(const T& ev, Entity* source = nullptr) {
        emit(T::EventName(), ev.ToEntity(), source);
    }

};

// ─── EventSystem ──────────────────────────────────────────────────────────────
class EventSystem {
public:
    void update(EntityList& entities, float dt) {
        for (auto& e : entities) {
            if (!entity_active(e)) continue;
            if (!has_component(e,"EventEmitter")) continue;
            auto& comp = e["components"]["EventEmitter"];
            if (!comp.value("enabled",true)) continue;

            int eid = e.value("id",0);
            std::string name = comp.value("event_name","");
            if (name.empty()) continue;

            bool emit_on_start = comp.value("emit_on_start",false);
            float emit_every   = comp.value("emit_every",0.f);
            bool  once         = comp.value("once",false);
            bool  should_emit  = false;

            if (emit_on_start && !_started.count(eid)) {
                should_emit = true; _started.insert(eid);
            } else if (emit_every > 0) {
                _accum[eid] = _accum.count(eid) ? _accum[eid]+dt : dt;
                while (_accum[eid] >= emit_every) { _accum[eid]-=emit_every; should_emit=true; }
            }

            if (should_emit) {
                Entity payload = comp.value("payload", Entity::object());
                EventBus::instance().emit(name, payload, &e);
                if (once) comp["enabled"]=false;
            }
        }
    }

private:
    std::unordered_set<int> _started;
    std::unordered_map<int,float> _accum;
};

// ─── Grid2DSystem ────────────────────────────────────────────────────────────
// A Grid2D is an authoring parent for one or more Tilemaps. The inspector
// exposes width, height, and gap, so the runtime resolves those values into
// transient Tilemap geometry before renderer, physics, and navigation use it.
class Grid2DSystem {
public:
    void update(EntityList& entities) {
        std::unordered_map<int, Entity*> by_id;
        by_id.reserve(entities.size());
        for (auto& e : entities) by_id[e.value("id", -1)] = &e;

        for (auto& tilemap_entity : entities) {
            if (!has_component(tilemap_entity, "Tilemap")) continue;
            auto& tilemap = tilemap_entity["components"]["Tilemap"];
            Entity* owner = nullptr;
            int parent_id = transform::parent_id_of(tilemap_entity);
            std::unordered_set<int> visited;
            while (parent_id >= 0 && visited.insert(parent_id).second) {
                const auto it = by_id.find(parent_id);
                if (it == by_id.end()) break;
                if (has_component(*it->second, "Grid2D")) { owner = it->second; break; }
                parent_id = transform::parent_id_of(*it->second);
            }
            if (!owner) {
                tilemap.erase("_grid_cell_width"); tilemap.erase("_grid_cell_height");
                tilemap.erase("_grid_cell_gap_x"); tilemap.erase("_grid_cell_gap_y");
                continue;
            }
            const auto& grid = (*owner)["components"]["Grid2D"];
            const float width = std::max(1.f, grid.value("cell_width", 32.f));
            const float height = std::max(1.f, grid.value("cell_height", 32.f));
            // A non-positive stride would make culling and collision invalid.
            const float gap_x = std::max(1.f - width, grid.value("cell_gap_x", 0.f));
            const float gap_y = std::max(1.f - height, grid.value("cell_gap_y", 0.f));
            tilemap["_grid_cell_width"] = width;
            tilemap["_grid_cell_height"] = height;
            tilemap["_grid_cell_gap_x"] = gap_x;
            tilemap["_grid_cell_gap_y"] = gap_y;
        }
    }
};

// ─── TilemapColliderSystem (optimised row-merging) ────────────────────────────
// Instead of one box per solid tile, we use greedy row-merging to produce
// fewer, tighter rectangles — typically 5-10× fewer colliders for large
// tilemaps, reducing JSON allocation overhead and physics broadphase cost.
class TilemapColliderSystem {
public:
    void update(EntityList& entities) {
        for (auto& e : entities) {
            if (!entity_active(e)) continue;
            if (!has_component(e,"Tilemap")) continue;
            auto& tm = e["components"]["Tilemap"];

            if (!tm.value("generate_colliders",false)) {
                e.erase("_tilemap_colliders");
                e.erase("_tilemap_debug_lines");
                continue;
            }

            float ox = has_component(e,"Transform") ? transform::world_x(e) : 0.f;
            float oy = has_component(e,"Transform") ? transform::world_y(e) : 0.f;
            const float cell_width = std::max(1.f, tm.value("_grid_cell_width", (float)tm.value("tile_size",32)));
            const float cell_height = std::max(1.f, tm.value("_grid_cell_height", (float)tm.value("tile_size",32)));
            const float gap_x = tm.value("_grid_cell_gap_x", 0.f);
            const float gap_y = tm.value("_grid_cell_gap_y", 0.f);
            const float stride_x = std::max(1.f, cell_width + gap_x);
            const float stride_y = std::max(1.f, cell_height + gap_y);
            const bool merge_cells = std::abs(gap_x) < 0.0001f && std::abs(gap_y) < 0.0001f;
            int origin_x = tm.value("origin_x", 0);
            int origin_y = tm.value("origin_y", 0);
            auto& grid = tm["grid"];

            std::unordered_set<int> col_set;
            if (tm.contains("collider_tile_ids"))
                for (auto& id : tm["collider_tile_ids"])
                    if (id.is_number()) col_set.insert(id.get<int>());

            int rows = (int)grid.size();
            if (rows == 0) { e.erase("_tilemap_colliders"); e.erase("_tilemap_debug_lines"); continue; }
            int cols = (int)grid[0].size();
            if (cols == 0) { e.erase("_tilemap_colliders"); e.erase("_tilemap_debug_lines"); continue; }

            auto is_solid = [&](int r, int c) -> bool {
                if (r < 0 || r >= rows || c < 0 || c >= cols) return false;
                if (grid[r][c].is_null() || !grid[r][c].is_number()) return false;
                int tid = grid[r][c].get<int>();
                if (tid < 0) return false;
                if (!col_set.empty() && !col_set.count(tid)) return false;
                // Tile Palette collision data is copied into the Tilemap when
                // assigned, keeping physics independent from editor asset paths.
                if (tm.contains("tile_collision") && tm["tile_collision"].is_object()) {
                    const std::string key = std::to_string(tid);
                    if (tm["tile_collision"].contains(key)) {
                        const Entity& collision = tm["tile_collision"][key];
                        if (collision.is_string() && collision.get<std::string>() == "none") return false;
                    }
                }
                return true;
            };

            Entity col_list = Entity::array();
            Entity dbg_list = Entity::array();
            auto add_line = [&](float x1, float y1, float x2, float y2) {
                Entity color = Entity::array();
                color.push_back(80); color.push_back(255); color.push_back(140); color.push_back(200);
                dbg_list.push_back({{"x1", x1}, {"y1", y1}, {"x2", x2}, {"y2", y2}, {"color", color}});
            };

            std::vector<std::vector<bool>> visited(rows, std::vector<bool>(cols, false));

            auto add_box = [&](int r, int c, int w, int h) {
                float x1 = ox + (c + origin_x) * stride_x;
                float y1 = oy + (r + origin_y) * stride_y;
                float x2 = ox + (c + w - 1 + origin_x) * stride_x + cell_width;
                float y2 = oy + (r + h - 1 + origin_y) * stride_y + cell_height;
                float cx = (x1 + x2) * 0.5f;
                float cy = (y1 + y2) * 0.5f;
                col_list.push_back({{"x", cx}, {"y", cy}, {"width", x2 - x1}, {"height", y2 - y1}});
                add_line(x1, y1, x2, y1);
                add_line(x2, y1, x2, y2);
                add_line(x2, y2, x1, y2);
                add_line(x1, y2, x1, y1);
            };

            for (int r = 0; r < rows; ++r) {
                for (int c = 0; c < cols; ++c) {
                    if (visited[r][c] || !is_solid(r, c)) continue;
                    int c2 = c;
                    while (merge_cells && c2+1<cols && !visited[r][c2+1] && is_solid(r, c2+1)) ++c2;
                    int r2 = r;
                    for (int rw = r+1; merge_cells && rw < rows; ++rw) {
                        bool full = true;
                        for (int cx = c; cx <= c2; ++cx) { if (visited[rw][cx]||!is_solid(rw,cx)){full=false;break;} }
                        if (full) r2 = rw; else break;
                    }
                    for (int vr=r;vr<=r2;++vr) for (int vc=c;vc<=c2;++vc) visited[vr][vc]=true;
                    add_box(r, c, c2-c+1, r2-r+1);
                }
            }

            e["_tilemap_colliders"] = col_list;
            e["_tilemap_debug_lines"] = dbg_list;
        }
    }
};

// ─── UILayoutSystem ───────────────────────────────────────────────────────────
// Mirrors Unity's HorizontalLayoutGroup / VerticalLayoutGroup / GridLayoutGroup:
// a UILayoutGroup component on an entity automatically arranges every UI child
// entity (any entity whose Transform.parent points at the layout group, same
// parenting mechanism Transform::SetParent uses everywhere else in this
// engine) into a row, column, or grid, instead of every child needing its own
// hand-placed anchor_x/anchor_y/pos_x/pos_y.
//
// This is a real, previously-missing gap: every UI*Panel/Button/Text/etc in
// this engine positions itself independently via anchor+pivot+pos against the
// SCREEN (see RenderSystem::draw_ui's resolve() lambda in
// engine/render_system_vk.cpp) with no concept of "lay these out relative to
// each other". Without a layout group, anything like a scrollable inventory
// list, a settings menu, or a row of toolbar buttons has to be re-positioned
// by hand every time an item is added/removed/resized — exactly the kind of
// busywork Unity's layout groups exist to eliminate.
//
// Design: rather than teach RenderSystem's resolve() about parent-relative
// coordinates (a much bigger change touching the render path), this system
// runs once per frame BEFORE draw_ui and computes each child's anchor_x/
// anchor_y/pivot_x/pivot_y/pos_x/pos_y/width/height directly — i.e. it
// pre-solves the layout into the exact same fields every UI component already
// reads, the same "mutate component data so existing systems pick it up for
// free" pattern TilemapColliderSystem above uses for _tilemap_colliders.
// Child UI components keep their own width/height as their "preferred size"
// for sizing-along-the-main-axis groups (Grid uses a fixed cell_width/
// cell_height instead — see below).
class UILayoutSystem {
public:
    void update(EntityList& entities) {
        for (auto& group_e : entities) {
            if (!entity_active(group_e)) continue;
            if (!has_component(group_e,"UILayoutGroup")) continue;
            auto& lg = group_e["components"]["UILayoutGroup"];

            int group_id = group_e.value("id",-1);
            if (group_id < 0) continue;

            // Children = any active entity parented (Transform.parent) to
            // this layout group entity, in scene order (stable — Unity's
            // layout groups use Hierarchy sibling order, and entity vector
            // order is this engine's closest equivalent since there's no
            // separate explicit sibling-index field).
            std::vector<Entity*> children;
            for (auto& e : entities) {
                if (!entity_active(e)) continue;
                if (&e == &group_e) continue;
                if (transform::parent_id_of(e) == group_id) children.push_back(&e);
            }
            if (children.empty()) continue;

            std::string type = lg.value("type", std::string("vertical")); // horizontal|vertical|grid
            float spacing   = lg.value("spacing", 4.f);
            float pad_l     = lg.value("padding_left",   8.f);
            float pad_r     = lg.value("padding_right",  8.f);
            float pad_t     = lg.value("padding_top",    8.f);
            float pad_b     = lg.value("padding_bottom", 8.f);
            // child_alignment mirrors Unity's TextAnchor enum but only the
            // cross-axis component matters here (the main axis is fully
            // determined by stacking order) — "start"|"center"|"end".
            std::string cross_align = lg.value("child_alignment", std::string("start"));

            // The group's own resolved screen rect — UI*'s "anchor against
            // the group" instead of "anchor against the screen" is achieved
            // by writing each child's anchor_x/anchor_y to match the GROUP's
            // anchor and offsetting pos_x/pos_y in screen pixels from there,
            // since resolve() in render_system_vk.cpp only knows how to
            // anchor against the screen, not against an arbitrary parent
            // rect. This keeps every existing UI component's render code
            // completely untouched.
            float group_ax = lg.value("anchor_x", 0.5f), group_ay = lg.value("anchor_y", 0.5f);
            float group_px = lg.value("pivot_x",   0.5f), group_py = lg.value("pivot_y",   0.5f);
            float group_w  = lg.value("width",  300.f),  group_h  = lg.value("height", 300.f);
            float group_pos_x = lg.value("pos_x", 0.f),  group_pos_y = lg.value("pos_y", 0.f);
            // Top-left of the group's own box, in the same offset-from-anchor
            // space pos_x/pos_y already live in (see resolve()'s `ax*sw +
            // pos_x - px*w` math) — children's pos_x/pos_y get added on top
            // of this so they end up inside the group's box once resolve()
            // adds the group's own anchor term back in.
            float box_left = group_pos_x - group_px * group_w;
            float box_top  = group_pos_y - group_py * group_h;

            auto ui_comp_of = [](Entity& e) -> Entity* {
                if (!e.contains("components")) return nullptr;
                for (auto& [k, v] : e["components"].items())
                    if (k.rfind("UI", 0) == 0 && k != "UILayoutGroup" && k != "UICanvas") return &v;
                return nullptr;
            };

            auto apply_anchor_pos = [&](Entity* c, float left, float top, float w, float h) {
                // Every UI* component anchors/pivots independently, so force
                // them all to top-left pivot anchored at the screen's
                // top-left for the duration of layout — this makes `pos_x/
                // pos_y` behave as plain top-left screen pixels, which is
                // what the math above computes. Anything that read a
                // different pivot/anchor before being parented into a layout
                // group is intentionally overridden — Unity's layout groups
                // do the same (a child's RectTransform anchors are driven by
                // the parent layout group, not the child's own settings).
                (*c)["anchor_x"] = 0.f; (*c)["anchor_y"] = 0.f;
                (*c)["pivot_x"]  = 0.f; (*c)["pivot_y"]  = 0.f;
                (*c)["pos_x"] = left; (*c)["pos_y"] = top;
                (*c)["width"] = w;    (*c)["height"] = h;
            };

            if (type == "grid") {
                float cell_w = lg.value("cell_width",  64.f);
                float cell_h = lg.value("cell_height", 64.f);
                int   columns = std::max(1, lg.value("columns", 4));
                int   i = 0;
                for (auto* child : children) {
                    Entity* c = ui_comp_of(*child);
                    if (!c) { ++i; continue; }
                    int col = i % columns, row = i / columns;
                    float left = box_left + pad_l + col * (cell_w + spacing);
                    float top  = box_top  + pad_t + row * (cell_h + spacing);
                    apply_anchor_pos(c, left, top, cell_w, cell_h);
                    ++i;
                }
                continue;
            }

            bool horizontal = (type == "horizontal");
            float cursor = horizontal ? (box_left + pad_l) : (box_top + pad_t);
            float cross_extent = horizontal ? (group_h - pad_t - pad_b) : (group_w - pad_l - pad_r);

            for (auto* child : children) {
                Entity* c = ui_comp_of(*child);
                if (!c) continue;
                float cw = c->value("width", 100.f);
                float ch = c->value("height", 30.f);

                float cross_pos; // position along the cross axis
                if (horizontal) {
                    float free_space = cross_extent - ch;
                    cross_pos = box_top + pad_t + (cross_align=="center" ? free_space*0.5f :
                                                    cross_align=="end"    ? free_space : 0.f);
                    apply_anchor_pos(c, cursor, cross_pos, cw, ch);
                    cursor += cw + spacing;
                } else {
                    float free_space = cross_extent - cw;
                    cross_pos = box_left + pad_l + (cross_align=="center" ? free_space*0.5f :
                                                     cross_align=="end"    ? free_space : 0.f);
                    apply_anchor_pos(c, cross_pos, cursor, cw, ch);
                    cursor += ch + spacing;
                }
            }
        }
    }
};
// ─── SortingGroupSystem ───────────────────────────────────────────────────────
// Mirrors Unity's SortingGroup component. When a SortingGroup is attached to
// a parent entity, all SpriteRenderer children are rendered as a unit: the
// group's sorting_layer + order_in_layer determine where the entire subtree
// appears in the draw order, and individual children's order_in_layer values
// only sort *within* the group (not against the rest of the scene).
//
// Implementation: this system runs once per frame before RenderSystem::draw().
// It stamps every child SpriteRenderer with a composite key:
//   group_layer_index * (1<<20) + group_order_in_layer * (1<<10) + child_order
// written to a "_sorting_key" field that RenderSystem reads for draw ordering.
// Children without a SortingGroup ancestor get their own plain key.
// This is the "mutate component data → existing render path reads it" pattern.
class SortingGroupSystem {
public:
    // sorting_layer_index: maps sorting layer name → its global sort index.
    // If empty, all layers are treated as index 0 (same as Unity's Default layer).
    void update(EntityList& entities,
                const std::unordered_map<std::string,int>& sorting_layer_index = {}) {
        // Build id→entity pointer map (needed for parent lookups)
        std::unordered_map<int,Entity*> eid_map;
        for (auto& e : entities)
            eid_map[e.value("id",-1)] = &e;

        for (auto& e : entities) {
            if (!entity_active(e)) continue;
            if (!has_component(e,"SpriteRenderer")) continue;

            // Walk ancestors to find the nearest SortingGroup
            int parent_id = transform::parent_id_of(e);
            Entity* group_ent = nullptr;
            while (parent_id >= 0) {
                auto it = eid_map.find(parent_id);
                if (it == eid_map.end()) break;
                if (has_component(*it->second, "SortingGroup")) {
                    group_ent = it->second;
                    break;
                }
                parent_id = transform::parent_id_of(*it->second);
            }

            auto& sr = e["components"]["SpriteRenderer"];
            if (!group_ent) {
                // No group: plain key from own layer + order
                std::string layer_name = sr.value("sorting_layer", std::string(""));
                int li = 0;
                auto lit = sorting_layer_index.find(layer_name);
                if (lit != sorting_layer_index.end()) li = lit->second;
                int oi = sr.value("order_in_layer", 0);
                sr["_sorting_key"] = li * (1 << 20) + oi;
                continue;
            }

            auto& grp = (*group_ent)["components"]["SortingGroup"];
            std::string grp_layer = grp.value("sorting_layer", std::string(""));
            int gli = 0;
            auto lit = sorting_layer_index.find(grp_layer);
            if (lit != sorting_layer_index.end()) gli = lit->second;
            int goi  = grp.value("order_in_layer", 0);
            int coi  = sr.value("order_in_layer", 0);
            // Pack: group occupies bits 20-30 (layer) + 10-20 (order); child in 0-10
            sr["_sorting_key"] = gli * (1 << 20) + goi * (1 << 10) + (coi & 0x3FF);
        }
    }
};

// ─── SpriteMaskSystem ─────────────────────────────────────────────────────────
// Mirrors Unity's SpriteMask component. An entity with SpriteMask defines a
// world-space rectangular (or circular) alpha-cut region. SpriteRenderer
// entities whose mask_interaction field is "visible_inside" or "visible_outside"
// get a scissor/clip rect written to "_mask_rect" which RenderSystem can use to
// restrict drawing. This is a CPU-side bounding-rect mask (not per-pixel GPU
// stencil — that would require a separate stencil pass); sufficient for the
// common Unity use cases: UI scroll views, sprite cropping, 2D vision cones.
//
// The mask rect is written in screen-space pixels via a Camera that caller
// provides (same Camera the render pass uses). RenderSystem reads _mask_rect and
// emits QuadCommand::scissor = that rect for masked sprites.
class SpriteMaskSystem {
public:
    // camera_x/y: world-space camera centre. ppu: pixels per world unit. sw/sh: screen size.
    void update(EntityList& entities, float camera_x, float camera_y,
                float ppu, int sw, int sh) {
        // Collect mask regions
        struct MaskRect { float wx,wy,ww,wh; int range_start,range_end; };
        std::vector<MaskRect> masks;
        for (auto& e : entities) {
            if (!entity_active(e)) continue;
            if (!has_component(e,"SpriteMask")) continue;
            auto& m = e["components"]["SpriteMask"];
            if (!m.value("enabled",true)) continue;
            float wx = has_component(e,"Transform") ? transform::world_x(e) : 0.f;
            float wy = has_component(e,"Transform") ? transform::world_y(e) : 0.f;
            float hw = m.value("width", 64.f) * 0.5f;
            float hh = m.value("height",64.f) * 0.5f;
            masks.push_back({wx-hw, wy-hh, m.value("width",64.f), m.value("height",64.f),
                             m.value("sorting_layer_range_start",-32768),
                             m.value("sorting_layer_range_end", 32767)});
        }

        float half_w = sw * 0.5f, half_h = sh * 0.5f;
        auto world_to_screen = [&](float wx, float wy, float& sx, float& sy) {
            sx = half_w + (wx - camera_x) * ppu;
            sy = half_h - (wy - camera_y) * ppu; // Y-flip
        };

        for (auto& e : entities) {
            if (!entity_active(e)) continue;
            if (!has_component(e,"SpriteRenderer")) continue;
            auto& sr = e["components"]["SpriteRenderer"];
            std::string mi = sr.value("mask_interaction", std::string("none"));
            if (mi == "none") { sr.erase("_mask_rect"); continue; }

            int layer_order = sr.value("_sorting_key", sr.value("order_in_layer",0));
            bool found = false;
            for (auto& mr : masks) {
                if (layer_order < mr.range_start || layer_order > mr.range_end) continue;
                float sx1,sy1,sx2,sy2;
                world_to_screen(mr.wx,        mr.wy,        sx1,sy1);
                world_to_screen(mr.wx+mr.ww,  mr.wy+mr.wh, sx2,sy2);
                if (sx2 < sx1) std::swap(sx1,sx2);
                if (sy2 < sy1) std::swap(sy1,sy2);
                Entity rect = Entity::object();
                rect["x"] = sx1; rect["y"] = sy1;
                rect["w"] = sx2-sx1; rect["h"] = sy2-sy1;
                rect["visible_inside"] = (mi == "visible_inside");
                sr["_mask_rect"] = rect;
                found = true;
                break;
            }
            if (!found) sr.erase("_mask_rect");
        }
    }
};

// ─── PhysicsEffectorSystem ────────────────────────────────────────────────────
// Implements Unity's physics effector components:
//
//   AreaEffector2D     — applies a world-direction or polar force/lift to all
//                        Rigidbody2D bodies overlapping its collider region.
//   ConstantForce2D    — continuously adds a fixed world-space or local-space
//                        force/torque to a single Rigidbody2D each physics tick.
//   PointEffector2D    — attract/repel from a world point; distance-falloff.
//   PlatformEffector2D — one-way collision: blocks bodies from below but lets
//                        them fall through from above (and optionally sides).
//   BuoyancyEffector2D — applies upward buoyancy + linear drag to bodies whose
//                        centre is below the water surface (wy field).
//
// All effectors run once per physics tick (called from fixed_update path).
// They modify velocity_x/velocity_y/angular_velocity on Rigidbody2D directly
// (the same fields phys::apply_physics integrates), so they compose naturally
// with the existing physics integrator without needing a separate force buffer.
class PhysicsEffectorSystem {
public:
    void update(EntityList& entities, float fixed_dt) {
        _update_area_effectors   (entities, fixed_dt);
        _update_constant_forces  (entities, fixed_dt);
        _update_point_effectors  (entities, fixed_dt);
        _update_buoyancy         (entities, fixed_dt);
        _update_platform_effectors(entities);
    }

private:
    // Helper: world-space AABB of the entity's collider (BoxCollider2D or CircleCollider2D)
    struct AABB { float x,y,w,h; };
    AABB _collider_aabb(const Entity& e) {
        float wx = has_component(e,"Transform") ? transform::world_x(e) : 0.f;
        float wy = has_component(e,"Transform") ? transform::world_y(e) : 0.f;
        if (has_component(e,"BoxCollider2D")) {
            auto& c = e["components"]["BoxCollider2D"];
            float w = c.value("width",32.f), h = c.value("height",32.f);
            float ox = c.value("offset_x",0.f), oy = c.value("offset_y",0.f);
            return {wx+ox-w*0.5f, wy+oy-h*0.5f, w, h};
        }
        if (has_component(e,"CircleCollider2D")) {
            auto& c = e["components"]["CircleCollider2D"];
            float r = c.value("radius",16.f);
            float ox = c.value("offset_x",0.f), oy = c.value("offset_y",0.f);
            return {wx+ox-r, wy+oy-r, r*2.f, r*2.f};
        }
        return {wx-16.f,wy-16.f,32.f,32.f};
    }
    bool _aabb_overlaps(const AABB& a, const AABB& b) {
        return a.x < b.x+b.w && a.x+a.w > b.x &&
               a.y < b.y+b.h && a.y+a.h > b.y;
    }

    void _update_area_effectors(EntityList& entities, float dt) {
        for (auto& ee : entities) {
            if (!entity_active(ee)) continue;
            if (!has_component(ee,"AreaEffector2D")) continue;
            auto& aef = ee["components"]["AreaEffector2D"];
            if (!aef.value("enabled",true)) continue;
            AABB eaabb = _collider_aabb(ee);
            float angle_deg = aef.value("force_angle", -90.f); // -90 = upward
            float mag   = aef.value("force_magnitude", 0.f);
            float lift  = aef.value("lift_magnitude",  0.f);   // perpendicular
            float drag  = aef.value("drag",  0.f);
            float ang_d = aef.value("angular_drag", 0.f);
            float rad   = angle_deg * GAMEENGINE_PI / 180.f;
            float fx = std::cos(rad) * mag;
            float fy = std::sin(rad) * mag;
            // lift is perpendicular to force direction
            float lx = -std::sin(rad) * lift;
            float ly =  std::cos(rad) * lift;

            for (auto& body : entities) {
                if (!entity_active(body)) continue;
                if (!has_component(body,"Rigidbody2D")) continue;
                if (&body == &ee) continue;
                auto& rb = body["components"]["Rigidbody2D"];
                if (rb.value("is_kinematic",false)) continue;
                AABB baabb = _collider_aabb(body);
                if (!_aabb_overlaps(eaabb, baabb)) continue;
                float mass = std::max(0.001f, rb.value("mass",1.f));
                float ax = (fx + lx) / mass * dt;
                float ay = (fy + ly) / mass * dt;
                rb["velocity_x"] = rb.value("velocity_x",0.f) + ax - rb.value("velocity_x",0.f)*drag*dt;
                rb["velocity_y"] = rb.value("velocity_y",0.f) + ay - rb.value("velocity_y",0.f)*drag*dt;
                rb["angular_velocity"] = rb.value("angular_velocity",0.f) * (1.f - ang_d*dt);
            }
        }
    }

    void _update_constant_forces(EntityList& entities, float dt) {
        for (auto& e : entities) {
            if (!entity_active(e)) continue;
            if (!has_component(e,"ConstantForce2D")) continue;
            if (!has_component(e,"Rigidbody2D")) continue;
            auto& cf = e["components"]["ConstantForce2D"];
            auto& rb = e["components"]["Rigidbody2D"];
            if (!cf.value("enabled",true)) continue;
            if (rb.value("is_kinematic",false)) continue;
            float mass = std::max(0.001f, rb.value("mass",1.f));
            // World-space force applied directly
            float fx = cf.value("force_x",0.f);
            float fy = cf.value("force_y",0.f);
            // Local-space (relative) force: rotated by the entity's current world angle
            float rfx = cf.value("relative_force_x",0.f);
            float rfy = cf.value("relative_force_y",0.f);
            if (rfx != 0.f || rfy != 0.f) {
                float ang_deg = has_component(e,"Transform")
                    ? e["components"]["Transform"].value("rotation",0.f) : 0.f;
                float ang = ang_deg * GAMEENGINE_PI / 180.f;
                float ca = std::cos(ang), sa = std::sin(ang);
                fx += rfx * ca - rfy * sa;
                fy += rfx * sa + rfy * ca;
            }
            float torque = cf.value("torque",0.f);
            rb["velocity_x"] = rb.value("velocity_x",0.f) + fx/mass*dt;
            rb["velocity_y"] = rb.value("velocity_y",0.f) + fy/mass*dt;
            rb["angular_velocity"] = rb.value("angular_velocity",0.f) + torque/mass*dt;
        }
    }

    void _update_point_effectors(EntityList& entities, float dt) {
        for (auto& pe : entities) {
            if (!entity_active(pe)) continue;
            if (!has_component(pe,"PointEffector2D")) continue;
            auto& pef = pe["components"]["PointEffector2D"];
            if (!pef.value("enabled",true)) continue;
            float px = has_component(pe,"Transform") ? transform::world_x(pe) : 0.f;
            float py = has_component(pe,"Transform") ? transform::world_y(pe) : 0.f;
            float force_mag  = pef.value("force_magnitude", 0.f);
            float force_var  = pef.value("force_variation",  0.f);
            float dist_scale = pef.value("distance_scale",  1.f);
            float max_dist   = pef.value("max_radius",      200.f);
            float drag       = pef.value("drag",            0.f);
            float ang_drag   = pef.value("angular_drag",    0.f);

            for (auto& body : entities) {
                if (!entity_active(body)) continue;
                if (!has_component(body,"Rigidbody2D")) continue;
                if (&body == &pe) continue;
                auto& rb = body["components"]["Rigidbody2D"];
                if (rb.value("is_kinematic",false)) continue;
                float bx = has_component(body,"Transform") ? transform::world_x(body) : 0.f;
                float by = has_component(body,"Transform") ? transform::world_y(body) : 0.f;
                float dx = px - bx, dy = py - by;
                float dist = std::sqrt(dx*dx+dy*dy);
                if (dist < 0.001f || dist > max_dist) continue;
                float falloff = 1.f / std::max(0.001f, dist * dist_scale);
                float nx = dx/dist, ny = dy/dist;
                float mass = std::max(0.001f, rb.value("mass",1.f));
                // Per-body deterministic variation so each body gets a consistent nudge
                float var = (force_var > 0.f) ? force_var * (((body.value("id",0) * 2654435761u) & 0xFFFF) / 65535.f * 2.f - 1.f) : 0.f;
                float acc = (force_mag + var) * falloff / mass * dt;
                rb["velocity_x"] = rb.value("velocity_x",0.f) + nx * acc;
                rb["velocity_y"] = rb.value("velocity_y",0.f) + ny * acc;
                if (drag > 0.f) {
                    rb["velocity_x"] = rb.value("velocity_x",0.f) * (1.f - drag * dt);
                    rb["velocity_y"] = rb.value("velocity_y",0.f) * (1.f - drag * dt);
                }
                if (ang_drag > 0.f)
                    rb["angular_velocity"] = rb.value("angular_velocity",0.f) * (1.f - ang_drag * dt);
            }
        }
    }

    void _update_buoyancy(EntityList& entities, float dt) {
        for (auto& water : entities) {
            if (!entity_active(water)) continue;
            if (!has_component(water,"BuoyancyEffector2D")) continue;
            auto& bef = water["components"]["BuoyancyEffector2D"];
            if (!bef.value("enabled",true)) continue;
            float surface_y = has_component(water,"Transform") ? transform::world_y(water) : 0.f;
            surface_y += bef.value("surface_level",0.f);
            float density    = bef.value("density",     2.f);
            float lin_drag   = bef.value("linear_drag", 3.f);
            float ang_drag   = bef.value("angular_drag",1.f);
            float flow_angle = bef.value("flow_angle",  0.f) * GAMEENGINE_PI / 180.f;
            float flow_mag   = bef.value("flow_magnitude",0.f);
            AABB waabb = _collider_aabb(water);

            for (auto& body : entities) {
                if (!entity_active(body)) continue;
                if (!has_component(body,"Rigidbody2D")) continue;
                if (&body == &water) continue;
                auto& rb = body["components"]["Rigidbody2D"];
                if (rb.value("is_kinematic",false)) continue;
                float bx = has_component(body,"Transform") ? transform::world_x(body) : 0.f;
                float by = has_component(body,"Transform") ? transform::world_y(body) : 0.f;
                // Screen-space: Y increases downward. Bodies *in* the water have
                // by > surface_y (they are visually below the waterline). The old
                // check was inverted — it rejected bodies that were actually submerged.
                if (by < surface_y) continue; // truly above surface: skip
                AABB baabb = _collider_aabb(body);
                if (!_aabb_overlaps(waabb, baabb)) continue;
                float mass = std::max(0.001f, rb.value("mass",1.f));
                // Submersion depth: how far below the surface (positive = submerged)
                float depth = by - surface_y;
                // Buoyancy pushes upward (negative Y in screen-space)
                float buoy = density * depth * dt;
                buoy = std::min(buoy, 9.8f * mass * dt); // cap so bodies don't rocket out
                rb["velocity_y"] = rb.value("velocity_y",0.f) - buoy;
                // linear drag in water
                float vx = rb.value("velocity_x",0.f), vy = rb.value("velocity_y",0.f);
                rb["velocity_x"] = vx - vx * lin_drag * dt;
                rb["velocity_y"] = vy - vy * lin_drag * dt;
                rb["angular_velocity"] = rb.value("angular_velocity",0.f) * (1.f - ang_drag*dt);
                // flow current
                rb["velocity_x"] = rb.value("velocity_x",0.f) + std::cos(flow_angle)*flow_mag*dt;
                rb["velocity_y"] = rb.value("velocity_y",0.f) + std::sin(flow_angle)*flow_mag*dt;
            }
        }
    }

    // PlatformEffector2D: one-way collision. Rather than modifying the existing
    // box-collider intersection test in physics.cpp (a much larger change), this
    // system sets a "_one_way_up" flag on colliders that physics.cpp already reads
    // (matching the convention already used by the existing one_way_platform path).
    void _update_platform_effectors(EntityList& entities) {
        for (auto& e : entities) {
            if (!entity_active(e)) continue;
            if (!has_component(e,"PlatformEffector2D")) continue;
            auto& pef = e["components"]["PlatformEffector2D"];
            if (!pef.value("enabled",true)) continue;
            // Tag the collider so the physics system knows to only block from above
            for (auto& key : {"BoxCollider2D","EdgeCollider2D"}) {
                if (has_component(e, key)) {
                    e["components"][key]["_one_way_up"]       = true;
                    e["components"][key]["one_way_sides"]     = pef.value("use_side_friction",false);
                    e["components"][key]["surface_arc"]       = pef.value("surface_arc",180.f);
                    e["components"][key]["side_arc"]          = pef.value("side_arc",50.f);
                    e["components"][key]["use_one_way_grouping"] = pef.value("use_one_way_grouping",true);
                }
            }
        }
    }
};

// ─── Pathfinding (NavMesh2D / A*) ─────────────────────────────────────────────
// Lightweight Unity NavMesh2D equivalent for 2D grid-based worlds.
//
// Navigation graph: built from a Tilemap's grid (impassable tiles = obstacles)
// or from a hand-authored list of walkable world-space cells. The graph is
// rebuilt lazily whenever the scene's Tilemap "grid" changes (checked via a
// dirty flag). Once built, NavMeshAgent2D components request paths via RequestPath()
// and follow waypoints each frame.
//
// NavMeshAgent2D component fields:
//   speed           float   world-units per second
//   stopping_distance float how close to destination counts as "arrived"
//   destination_x/y float   destination in world space
//   auto_move       bool    if true, agent moves itself toward next waypoint
//                           (set false to read waypoints manually from "_path")
//
// NavMesh2D component (on the same entity as a Tilemap):
//   tile_size       int     cell size in world units (matches Tilemap's tile_size)
//   obstacle_ids    array   tile ids that block movement (empty = all tiles block)
//   agent_radius    float   cell clearance required around obstacles (in tiles)
//
// A* implementation: standard 4-neighbour (or 8-neighbour if allow_diagonals=true)
// grid A* with Manhattan/octile heuristic. Path is stored as world-space waypoints
// in entity["_path"] (array of {x,y} objects) for scripts to read.
class NavMesh2DSystem {
public:
    struct Cell { int col, row; };

    void update(EntityList& entities, float dt) {
        // Step 1: rebuild nav graph if any Tilemap+NavMesh2D is dirty
        for (auto& e : entities) {
            if (!entity_active(e)) continue;
            if (!has_component(e,"Tilemap") || !has_component(e,"NavMesh2D")) continue;
            if (!e.value("_nav_dirty", true)) continue;
            _build_graph(e);
            e["_nav_dirty"] = false;
        }

        // Dynamic rectangular obstacles are reapplied every frame so moving
        // obstacles immediately affect the next path request without forcing a
        // full tilemap rebuild.
        for (auto& e : entities) {
            if (entity_active(e) && has_component(e, "NavMesh2D"))
                _apply_dynamic_obstacles(e, entities);
        }

        // Step 2: resolve _dest_entity → destination_x/y (set from the inspector picker)
        for (auto& agent : entities) {
            if (!entity_active(agent)) continue;
            const char* nav_key = _nav_component_key(agent);
            if (!nav_key) continue;
            auto& nav = agent["components"][nav_key];
            int dest_eid = nav.value("_dest_entity", -1);
            if (dest_eid < 0) continue;
            for (auto& tgt : entities) {
                if (tgt.value("id",-1) != dest_eid) continue;
                if (!entity_active(tgt)) break;
                float tx = has_component(tgt,"Transform") ? transform::world_x(tgt) : 0.f;
                float ty = has_component(tgt,"Transform") ? transform::world_y(tgt) : 0.f;
                nav["destination_x"] = tx;
                nav["destination_y"] = ty;
                break;
            }
        }

        // Step 3: for every NavMeshAgent2D that has a pending path request, run A*
        for (auto& agent : entities) {
            if (!entity_active(agent)) continue;
            const char* nav_key = _nav_component_key(agent);
            if (!nav_key) continue;
            auto& nav = agent["components"][nav_key];

            float tx = nav.value("destination_x", nav.value("path_target_x", 0.f));
            float ty = nav.value("destination_y", nav.value("path_target_y", 0.f));
            const bool has_explicit_target = nav.value("_dest_entity", -1) >= 0 ||
                std::abs(tx) > 0.0001f || std::abs(ty) > 0.0001f;
            const bool destination_changed = !nav.contains("_last_path_destination_x")
                ? has_explicit_target
                : std::abs(tx - nav.value("_last_path_destination_x", tx)) > 0.25f ||
                  std::abs(ty - nav.value("_last_path_destination_y", ty)) > 0.25f;
            const bool requested = nav.value("_path_requested", false) ||
                (nav.value("auto_repath", true) && destination_changed);
            if (!requested) continue;
            float ax = has_component(agent,"Transform") ? transform::world_x(agent) : 0.f;
            float ay = has_component(agent,"Transform") ? transform::world_y(agent) : 0.f;

            // Find the nearest NavMesh2D/Tilemap
            for (auto& map_e : entities) {
                if (!entity_active(map_e)) continue;
                if (!has_component(map_e,"NavMesh2D")) continue;
                auto path = _find_path(map_e, ax, ay, tx, ty);
                nav["_path_waypoints"] = _waypoints_to_entity(path);
                nav["_wp_index"] = 0;
                break;
            }
            nav["_path_requested"] = false;
            nav["_last_path_destination_x"] = tx;
            nav["_last_path_destination_y"] = ty;
            agent["_path"] = nav["_path_waypoints"];
        }

        // Step 3: auto-move agents along their path
        for (auto& agent : entities) {
            if (!entity_active(agent)) continue;
            const char* nav_key = _nav_component_key(agent);
            if (!nav_key) continue;
            auto& nav = agent["components"][nav_key];
            if (!nav.value("auto_move",true)) continue;
            if (!nav.contains("_path_waypoints") || !nav["_path_waypoints"].is_array()) continue;

            int wp_idx = nav.value("_wp_index", 0);
            int wp_count = (int)nav["_path_waypoints"].size();
            if (wp_idx >= wp_count) {
                nav["_reached_destination"] = true;
                continue;
            }
            nav["_reached_destination"] = false;

            float speed = nav.value("speed", 100.f);
            float stop  = nav.value("stopping_distance", nav.value("stopping_dist", 4.f));
            auto& wp = nav["_path_waypoints"][wp_idx];
            float wx = wp.value("x",0.f), wy = wp.value("y",0.f);
            float ax = has_component(agent,"Transform") ? transform::world_x(agent) : 0.f;
            float ay = has_component(agent,"Transform") ? transform::world_y(agent) : 0.f;
            float dx = wx-ax, dy = wy-ay;
            float dist = std::sqrt(dx*dx+dy*dy);
            if (dist <= stop) {
                nav["_wp_index"] = wp_idx + 1;
                continue;
            }
            float nx = dx/dist, ny = dy/dist;
            // If entity has Rigidbody2D, move via velocity; otherwise move Transform directly
            if (has_component(agent,"Rigidbody2D")) {
                auto& rb = agent["components"]["Rigidbody2D"];
                rb["velocity_x"] = nx * speed;
                rb["velocity_y"] = ny * speed;
            } else if (has_component(agent,"Transform")) {
                auto& tr = agent["components"]["Transform"];
                tr["x"] = tr.value("x",0.f) + nx*speed*dt;
                tr["y"] = tr.value("y",0.f) + ny*speed*dt;
            }
        }
    }

    // Request a path for a NavMeshAgent2D entity. Call from script:
    //   NavMesh2DSystem::request_path(entity, target_x, target_y);
    static void request_path(Entity& agent, float tx, float ty) {
        const char* nav_key = _nav_component_key(agent);
        if (!nav_key) return;
        auto& nav = agent["components"][nav_key];
        nav["destination_x"] = tx;
        nav["destination_y"] = ty;
        // Preserve fields used by scenes created before NavMeshAgent2D was the
        // public component name, without exposing that legacy name in the UI.
        if (std::string(nav_key) == "NavAgent2D") {
            nav["path_target_x"] = tx;
            nav["path_target_y"] = ty;
        }
        nav["_path_requested"] = true;
    }

private:
    // Per-navmesh graph: 2D boolean grid (true = passable)
    struct Graph {
        std::vector<std::vector<bool>> base_passable;
        std::vector<std::vector<bool>> passable;
        int cols = 0, rows = 0;
        float ox = 0, oy = 0, cell_width = 32, cell_height = 32, stride_x = 32, stride_y = 32;
        int origin_col = 0, origin_row = 0;
    };
    std::unordered_map<int, Graph> _graphs; // keyed by entity id

    static const char* _nav_component_key(const Entity& entity) {
        if (has_component(entity, "NavMeshAgent2D")) return "NavMeshAgent2D";
        // Compatibility with scenes authored before the component was renamed.
        if (has_component(entity, "NavAgent2D")) return "NavAgent2D";
        return nullptr;
    }

    void _build_graph(Entity& e) {
        int eid = e.value("id",-1);
        auto& tm = e["components"]["Tilemap"];
        auto& nm = e["components"]["NavMesh2D"];
        Graph g;
        g.cell_width  = std::max(1.f, tm.value("_grid_cell_width", (float)tm.value("tile_size",32)));
        g.cell_height = std::max(1.f, tm.value("_grid_cell_height", (float)tm.value("tile_size",32)));
        g.stride_x    = std::max(1.f, g.cell_width + tm.value("_grid_cell_gap_x", 0.f));
        g.stride_y    = std::max(1.f, g.cell_height + tm.value("_grid_cell_gap_y", 0.f));
        g.origin_col  = tm.value("origin_x",0);
        g.origin_row  = tm.value("origin_y",0);
        g.ox = has_component(e,"Transform") ? transform::world_x(e) : 0.f;
        g.oy = has_component(e,"Transform") ? transform::world_y(e) : 0.f;

        std::unordered_set<int> obstacle_ids;
        if (nm.contains("obstacle_ids") && nm["obstacle_ids"].is_array())
            for (auto& id : nm["obstacle_ids"])
                if (id.is_number()) obstacle_ids.insert(id.get<int>());
        bool all_block = obstacle_ids.empty();

        auto& grid = tm["grid"];
        g.rows = (int)grid.size();
        g.cols = g.rows > 0 ? (int)grid[0].size() : 0;
        g.passable.assign(g.rows, std::vector<bool>(g.cols, true));
        for (int r = 0; r < g.rows; ++r) {
            for (int c = 0; c < g.cols; ++c) {
                if (!grid[r][c].is_number()) continue;
                int tid = grid[r][c].get<int>();
                bool is_obstacle = (all_block && tid >= 0) ||
                                   (!all_block && obstacle_ids.count(tid));
                g.passable[r][c] = !is_obstacle;
            }
        }
        g.base_passable = g.passable;
        _graphs[eid] = std::move(g);
    }

    void _apply_dynamic_obstacles(Entity& map_entity, const EntityList& entities) {
        auto graph_it = _graphs.find(map_entity.value("id", -1));
        if (graph_it == _graphs.end()) return;
        Graph& graph = graph_it->second;
        graph.passable = graph.base_passable;
        if (graph.cols <= 0 || graph.rows <= 0) return;

        for (const auto& obstacle_entity : entities) {
            if (!entity_active(obstacle_entity) ||
                !has_component(obstacle_entity, "NavMeshObstacle2D") ||
                !has_component(obstacle_entity, "Transform")) continue;
            const auto& obstacle = obstacle_entity["components"]["NavMeshObstacle2D"];
            if (!obstacle.value("carve", true)) continue;
            const float width = std::max(0.f, obstacle.value("width", 32.f));
            const float height = std::max(0.f, obstacle.value("height", 32.f));
            if (width <= 0.f || height <= 0.f) continue;
            const float center_x = transform::world_x(obstacle_entity) + obstacle.value("offset_x", 0.f);
            const float center_y = transform::world_y(obstacle_entity) + obstacle.value("offset_y", 0.f);
            const float min_x = center_x - width * 0.5f, max_x = center_x + width * 0.5f;
            const float min_y = center_y - height * 0.5f, max_y = center_y + height * 0.5f;
            for (int row = 0; row < graph.rows; ++row) {
                for (int col = 0; col < graph.cols; ++col) {
                    const float x = graph.ox + (col + graph.origin_col) * graph.stride_x + graph.cell_width * 0.5f;
                    const float y = graph.oy + (row + graph.origin_row) * graph.stride_y + graph.cell_height * 0.5f;
                    if (x >= min_x && x <= max_x && y >= min_y && y <= max_y)
                        graph.passable[row][col] = false;
                }
            }
        }
    }

    // A* on the graph, returns world-space waypoints
    std::vector<std::pair<float,float>> _find_path(Entity& map_e,
        float sx, float sy, float ex, float ey) {

        int eid = map_e.value("id",-1);
        auto git = _graphs.find(eid);
        if (git == _graphs.end()) return {};
        const Graph& g = git->second;
        if (g.cols == 0 || g.rows == 0) return {};
        bool diag = map_e["components"]["NavMesh2D"].value("allow_diagonals",false);

        auto world_to_cell = [&](float wx, float wy, int& col, int& row) {
            col = (int)std::floor((wx - g.ox) / g.stride_x) - g.origin_col;
            row = (int)std::floor((wy - g.oy) / g.stride_y) - g.origin_row;
        };
        auto cell_to_world = [&](int col, int row, float& wx, float& wy) {
            wx = g.ox + (col + g.origin_col) * g.stride_x + g.cell_width * 0.5f;
            wy = g.oy + (row + g.origin_row) * g.stride_y + g.cell_height * 0.5f;
        };
        auto in_bounds = [&](int c, int r) {
            return c>=0&&r>=0&&c<g.cols&&r<g.rows;
        };
        auto passable = [&](int c, int r) {
            return in_bounds(c,r) && g.passable[r][c];
        };

        int sc,sr,ec,er;
        world_to_cell(sx,sy,sc,sr);
        world_to_cell(ex,ey,ec,er);
        sc=std::max(0,std::min(g.cols-1,sc)); sr=std::max(0,std::min(g.rows-1,sr));
        ec=std::max(0,std::min(g.cols-1,ec)); er=std::max(0,std::min(g.rows-1,er));
        if (!passable(ec,er)) return {};
        if (sc==ec && sr==er) return {{ex,ey}};

        // Keep A*'s queue priority (g + h) separate from its real travelled
        // cost (g). Comparing a priority to cost_so_far incorrectly marks
        // every heuristic-bearing neighbour as stale.
        using OpenCell = std::tuple<float,float,int,int>; // priority, g, col, row
        std::priority_queue<OpenCell, std::vector<OpenCell>, std::greater<OpenCell>> open;
        std::unordered_map<int, float> cost_so_far;
        std::unordered_map<int, int>   came_from;
        auto encode = [&](int c, int r){ return r*g.cols+c; };

        open.push({0.f,0.f,sc,sr});
        cost_so_far[encode(sc,sr)] = 0.f;
        came_from[encode(sc,sr)] = -1;

        static const int dx4[] = {0,0,1,-1};
        static const int dy4[] = {1,-1,0,0};
        static const int dx8[] = {0,0,1,-1,1,1,-1,-1};
        static const int dy8[] = {1,-1,0,0,1,-1,1,-1};
        const float costs8[] = {1,1,1,1,1.414f,1.414f,1.414f,1.414f};
        int nn = diag ? 8 : 4;
        const int* dxs = diag ? dx8 : dx4;
        const int* dys = diag ? dy8 : dy4;

        while (!open.empty()) {
            auto [priority,cost,cc,cr] = open.top(); open.pop();
            (void)priority;
            int cur_key = encode(cc,cr);
            if (cc==ec && cr==er) {
                // Reconstruct
                std::vector<std::pair<float,float>> path;
                int key = cur_key;
                while (key >= 0 && came_from.count(key) && came_from[key]>=0) {
                    int nc2=key%g.cols, nr2=key/g.cols;
                    float wx,wy; cell_to_world(nc2,nr2,wx,wy);
                    path.push_back({wx,wy});
                    key=came_from[key];
                }
                path.push_back({sx,sy}); // start
                std::reverse(path.begin(),path.end());
                if (!path.empty()) path.back() = {ex,ey}; // exact destination
                return path;
            }
            // Parenthesize the fallback lookup. Without this, C++ parses the
            // ternary after `cost > count(...)`, causing even the initial A*
            // node to be treated as stale and every path request to return
            // empty.
            if (cost > (cost_so_far.count(cur_key) ? cost_so_far[cur_key] : 1e9f)) continue;
            for (int i=0;i<nn;++i) {
                int nc=cc+dxs[i], nr=cr+dys[i];
                if (!passable(nc,nr)) continue;
                float step_cost = diag ? costs8[i] : 1.f;
                float new_cost = cost_so_far[cur_key] + step_cost;
                int nk = encode(nc,nr);
                auto cit = cost_so_far.find(nk);
                if (cit == cost_so_far.end() || new_cost < cit->second) {
                    cost_so_far[nk] = new_cost;
                    came_from[nk] = cur_key;
                    float h = diag ? std::max(std::abs(nc-ec),std::abs(nr-er))
                                   : (float)(std::abs(nc-ec)+std::abs(nr-er));
                    open.push({new_cost + h, new_cost, nc, nr});
                }
            }
        }
        return {}; // no path found
    }

    Entity _waypoints_to_entity(const std::vector<std::pair<float,float>>& wps) {
        Entity arr = Entity::array();
        for (auto& [wx,wy] : wps) {
            Entity pt = Entity::object();
            pt["x"] = wx; pt["y"] = wy;
            arr.push_back(pt);
        }
        return arr;
    }
};

// ─── VirtualCameraSystem (Cinemachine-style) ──────────────────────────────────
// Mirrors Unity's Cinemachine CinemachineVirtualCamera. Provides:
//   - Multi-priority virtual cameras; highest-priority active cam drives the
//     real Camera2D component (on the Brain entity).
//   - Dead zones + soft zones (Cinemachine Framing Transposer).
//   - Look-ahead: predicts target position from velocity to reduce perceived lag.
//   - Camera shake / impulse via CinemachineImpulseSource (trauma-decay model).
//   - Camera confiner: clamps the view inside a world-space rectangle.
//   - Orthographic size damping (cinematic zoom transitions).
//
// Component: VirtualCamera
//   follow_target    int     entity id to follow (or -1 for static)
//   look_at_target   int     entity id to look at (same as follow usually)
//   priority         int     higher = more important (Brain uses highest active)
//   dead_zone_w/h    float   world-units: inner region where camera doesn't move
//   soft_zone_w/h    float   world-units: damped region outside dead zone
//   look_ahead_time  float   seconds of velocity prediction
//   ortho_size       float   target orthographic half-height in world units
//   ortho_damp       float   smoothing factor for ortho-size changes (0=instant)
//   x_damp/y_damp   float   positional damping (0=instant, higher=more lag)
//   confine          bool    true to clamp view inside confiner_rect
//   confiner_x/y/w/h float  world-space confiner bounds (if confine=true)
//
// Brain entity: has Camera2D component — VirtualCameraSystem writes the final
//   blended position + zoom to Camera2D.offset_x/offset_y/orthographic_size.
//
// Impulse: call VirtualCameraSystem::add_impulse(entity_id, strength) to
//   trigger a camera shake. Multiple sources accumulate (trauma model).
class VirtualCameraSystem {
public:
    // Add a camera impulse (shake). strength is 0-1; trauma decays over time.
    void add_impulse(int camera_entity_id, float strength) {
        _trauma[camera_entity_id] += std::min(1.f, std::max(0.f, strength));
        if (_trauma[camera_entity_id] > 1.f) _trauma[camera_entity_id] = 1.f;
    }

    void update(EntityList& entities, float dt) {
        // Step 1: find the Brain (entity with Camera2D that has is_brain=true,
        // or the first Camera2D if none is explicitly a brain)
        Entity* brain = nullptr;
        for (auto& e : entities) {
            if (!entity_active(e)) continue;
            if (!has_component(e,"Camera2D")) continue;
            auto& cam = e["components"]["Camera2D"];
            if (cam.value("is_brain",false) || brain == nullptr) { brain = &e; }
            if (cam.value("is_brain",false)) break;
        }
        if (!brain) return;
        auto& brain_cam = (*brain)["components"]["Camera2D"];

        // Step 2: find the highest-priority active camera.  Cinemachine2D is
        // the user-facing component; VirtualCamera remains a compatible
        // legacy alias so old scenes keep their behavior.
        Entity* best_vc = nullptr;
        std::string best_component;
        int best_prio = INT_MIN;
        for (auto& e : entities) {
            if (!entity_active(e)) continue;
            const std::string component = has_component(e,"Cinemachine2D") ? "Cinemachine2D" :
                                          (has_component(e,"VirtualCamera") ? "VirtualCamera" : "");
            if (component.empty()) continue;
            auto& vc = e["components"][component];
            if (!vc.value("enabled",true)) continue;
            int prio = vc.value("priority",0);
            if (prio > best_prio || best_vc == nullptr) {
                best_prio = prio;
                best_vc = &e;
                best_component = component;
            }
        }
        if (!best_vc) {
            brain_cam["_virtual_camera_active"] = false;
            return;
        }

        auto& vc = (*best_vc)["components"][best_component];
        int vc_id = best_vc->value("id",-1);

        // Step 3: get follow target position
        int follow_id = vc.value("follow_target",-1);
        float target_x = vc.value("target_x", brain_cam.value("offset_x",0.f));
        float target_y = vc.value("target_y", brain_cam.value("offset_y",0.f));
        for (auto& e : entities) {
            if (e.value("id",-1) != follow_id) continue;
            float ex = has_component(e,"Transform") ? transform::world_x(e) : 0.f;
            float ey = has_component(e,"Transform") ? transform::world_y(e) : 0.f;

            // Look-ahead: predict position from velocity, smoothed to avoid jitter
            float look_ahead = vc.value("look_ahead_time",0.f);
            if (look_ahead > 0.f && has_component(e,"Rigidbody2D")) {
                auto& rb = e["components"]["Rigidbody2D"];
                float raw_vx = rb.value("velocity_x",0.f);
                float raw_vy = rb.value("velocity_y",0.f);
                // Smooth the lookahead target with its own damping factor
                float la_smooth = vc.value("look_ahead_smoothing", 10.f);
                float la_t = (la_smooth > 0.f) ? (1.f - std::exp(-la_smooth * dt)) : 1.f;
                float prev_lx = vc.value("_la_vx", raw_vx);
                float prev_ly = vc.value("_la_vy", raw_vy);
                float smooth_vx = prev_lx + (raw_vx - prev_lx) * la_t;
                float smooth_vy = prev_ly + (raw_vy - prev_ly) * la_t;
                vc["_la_vx"] = smooth_vx;
                vc["_la_vy"] = smooth_vy;
                ex += smooth_vx * look_ahead;
                ey += smooth_vy * look_ahead;
            }
            target_x = ex; target_y = ey;
            break;
        }

        // Step 4: dead zone + soft zone clamping
        // Seed _cam_x/_cam_y to target on very first update so camera
        // doesn't lerp in from world-origin (0,0) on scene load.
        if (!vc.contains("_cam_x")) { vc["_cam_x"] = target_x; vc["_cam_y"] = target_y; }
        float cur_x = vc.value("_cam_x", target_x);
        float cur_y = vc.value("_cam_y", target_y);

        float dz_w = vc.value("dead_zone_w",0.f) * 0.5f;
        float dz_h = vc.value("dead_zone_h",0.f) * 0.5f;
        float sz_w = vc.value("soft_zone_w",0.8f) * 0.5f; // outside soft zone: hard-clamp
        float sz_h = vc.value("soft_zone_h",0.8f) * 0.5f;

        float dx = target_x - cur_x, dy = target_y - cur_y;

        // Dead zone: suppress movement entirely within inner box
        if (std::abs(dx) < dz_w) dx = 0.f;
        else dx = (dx > 0.f ? dx - dz_w : dx + dz_w);
        if (std::abs(dy) < dz_h) dy = 0.f;
        else dy = (dy > 0.f ? dy - dz_h : dy + dz_h);

        // Soft zone: damp within soft zone, hard-snap if target escapes it
        float outer_dx = std::abs(target_x - cur_x) - sz_w;
        float outer_dy = std::abs(target_y - cur_y) - sz_h;
        if (outer_dx > 0.f) {
            // Target outside soft zone on X — hard follow with no damping
            cur_x = target_x - (target_x > cur_x ? sz_w : -sz_w);
            dx = 0.f; // positional damping already applied via hard clamp
        }
        if (outer_dy > 0.f) {
            cur_y = target_y - (target_y > cur_y ? sz_h : -sz_h);
            dy = 0.f;
        }

        // Positional damping
        float xd = vc.value("x_damp", vc.value("damping_x", 2.f));
        float yd = vc.value("y_damp", vc.value("damping_y", 2.f));
        float t_x = (xd > 0.f) ? (1.f - std::exp(-xd*dt)) : 1.f;
        float t_y = (yd > 0.f) ? (1.f - std::exp(-yd*dt)) : 1.f;
        cur_x += dx * t_x;
        cur_y += dy * t_y;

        // Step 5: confiner clamp
        if (vc.value("confine",false)) {
            float cx = vc.value("confiner_x", vc.value("confine_min_x", -1000.f));
            float cy = vc.value("confiner_y", vc.value("confine_min_y", -1000.f));
            float cw = vc.value("confiner_w", vc.value("confine_max_x", 1000.f) - cx);
            float ch = vc.value("confiner_h", vc.value("confine_max_y", 1000.f) - cy);
            float ortho = brain_cam.value("orthographic_size",5.f);
            float aspect = vc.value("aspect_ratio",1.777f);
            float half_view_w = ortho * aspect;
            float half_view_h = ortho;
            cur_x = std::max(cx + half_view_w, std::min(cx + cw - half_view_w, cur_x));
            cur_y = std::max(cy + half_view_h, std::min(cy + ch - half_view_h, cur_y));
        }

        vc["_cam_x"] = cur_x;
        vc["_cam_y"] = cur_y;

        // Step 6: orthographic size damping
        float target_ortho = vc.value("ortho_size", vc.value("orthographic_size", brain_cam.value("orthographic_size",5.f)));
        float ortho_damp   = vc.value("ortho_damp", 0.f);
        float cur_ortho    = vc.value("_cur_ortho", target_ortho);
        if (ortho_damp > 0.f)
            cur_ortho += (target_ortho - cur_ortho) * (1.f - std::exp(-ortho_damp*dt));
        else
            cur_ortho = target_ortho;
        vc["_cur_ortho"] = cur_ortho;

        // Step 7: trauma-based shake
        float trauma = _trauma.count(vc_id) ? _trauma[vc_id] : 0.f;
        float shake_amt = trauma * trauma; // square for more natural falloff
        float shake_mag_pos = vc.value("shake_magnitude_position", 0.3f);
        float shake_mag_rot = vc.value("shake_magnitude_rotation", 2.f);
        _shake_time[vc_id] = _shake_time.count(vc_id) ? _shake_time[vc_id] + dt : dt;
        float st = _shake_time[vc_id];
        // Simple Lissajous-style noise (no dependency on external noise lib)
        float sx = shake_amt * shake_mag_pos * (std::sin(st*23.7f)*0.6f + std::sin(st*11.3f)*0.4f);
        float sy = shake_amt * shake_mag_pos * (std::sin(st*17.1f)*0.6f + std::sin(st*29.3f)*0.4f);
        float sr_ang = shake_amt * shake_mag_rot * std::sin(st*13.7f);
        // Decay trauma
        float decay = vc.value("shake_decay",1.5f);
        if (trauma > 0.f) {
            _trauma[vc_id] = std::max(0.f, trauma - decay * dt);
        }

        // Step 8: write back to brain Camera2D
        brain_cam["offset_x"] = cur_x + sx;
        brain_cam["offset_y"] = cur_y + sy;
        brain_cam["orthographic_size"] = cur_ortho;
        brain_cam["_virtual_camera_x"] = cur_x + sx;
        brain_cam["_virtual_camera_y"] = cur_y + sy;
        brain_cam["_virtual_camera_active"] = true;
        // Camera::zoom is the renderer's live scale.  Convert the authored
        // orthographic size into that scale around the legacy 5-unit baseline,
        // then multiply the Cinemachine framing zoom.  This keeps old Camera2D
        // scenes stable while making both Cinemachine controls visibly live.
        const float ortho_reference = std::max(0.001f, brain_cam.value("orthographic_reference_size", 5.f));
        const float framing_zoom = best_component == "Cinemachine2D"
            ? vc.value("zoom", 1.f) : 1.f;
        brain_cam["zoom"] = framing_zoom * ortho_reference / std::max(0.001f, cur_ortho);
        brain_cam["projection_size_mode"] = "zoom";
        // Camera rotation shake (stored separately; CameraSystem reads it)
        brain_cam["_shake_angle"] = sr_ang;
    }

private:
    std::unordered_map<int,float> _trauma;
    std::unordered_map<int,float> _shake_time;
};


// ─── ObjectPool2DSystem ───────────────────────────────────────────────────────
// Motion trail data is updated independently of the renderer so it has the
// exact same behaviour in Play mode and an exported standalone game.  Points
// are kept newest-first in the component and expire by their authored time.
// The Vulkan renderer turns those points into tapered quads.
class TrailRenderer2DSystem {
public:
    void update(EntityList& entities, float dt) {
        for (auto& entity : entities) {
            if (!entity_active(entity) || !has_component(entity, "TrailRenderer2D") ||
                !has_component(entity, "Transform")) continue;

            auto& trail = entity["components"]["TrailRenderer2D"];
            if (!trail.value("enabled", true)) continue;
            auto& points = trail["_trail_points"];
            if (!points.is_array()) points = Entity::array();

            const float lifetime = std::max(0.001f, trail.value("time", 0.5f));
            const auto world = transform::cached_world(entity);
            const float minimum_distance = std::max(0.f, trail.value("min_vertex_distance", 4.f));

            if (trail.value("emitting", true)) {
                bool append = points.empty();
                if (!append) {
                    const auto& newest = points[0];
                    const float dx = world.x - newest.value("x", world.x);
                    const float dy = world.y - newest.value("y", world.y);
                    append = dx * dx + dy * dy >= minimum_distance * minimum_distance;
                }
                if (append) {
                    Entity point = Entity::object();
                    point["x"] = world.x;
                    point["y"] = world.y;
                    point["age"] = 0.f;
                    points.insert(0, std::move(point));
                }
            }

            const auto start_color = trail.value("color_start", std::vector<int>{255, 200, 80, 255});
            const auto end_color = trail.value("color_end", std::vector<int>{255, 80, 80, 0});
            const float start_width = std::max(0.f, trail.value("width_start", 6.f));
            const float end_width = std::max(0.f, trail.value("width_end", 0.f));
            for (size_t index = 0; index < points.size();) {
                auto& point = points[index];
                const float age = point.value("age", 0.f) + std::max(0.f, dt);
                if (age >= lifetime) {
                    points.erase_at(index);
                    continue;
                }
                point["age"] = age;
                const float t = std::clamp(age / lifetime, 0.f, 1.f);
                point["width"] = start_width + (end_width - start_width) * t;
                const auto channel = [&](int i, int fallback) {
                    const int first = start_color.size() > (size_t)i ? start_color[(size_t)i] : fallback;
                    const int last = end_color.size() > (size_t)i ? end_color[(size_t)i] : fallback;
                    return first + (int)((last - first) * t);
                };
                point["r"] = channel(0, 255);
                point["g"] = channel(1, 255);
                point["b"] = channel(2, 255);
                point["a"] = channel(3, 255);
                ++index;
            }
        }
    }
};

// ─── CustomRenderTexture2DSystem ────────────────────────────────────────────
// Owns lifecycle/update state for procedural runtime textures. Pixel upload is
// performed by RenderSystem because it owns Vulkan resources; this system only
// advances a revision counter, which makes on-demand updates truly on-demand.
class CustomRenderTexture2DSystem {
public:
    void update(EntityList& entities, float dt) {
        for (auto& entity : entities) {
            if (!entity.contains("components") || !entity["components"].contains("CustomRenderTexture2D")) continue;
            auto& crt = entity["components"]["CustomRenderTexture2D"];
            if (!crt.value("enabled", true)) continue;

            if (!crt.value("_runtime_initialized", false)) {
                crt["_runtime_initialized"] = true;
                crt["_runtime_revision"] = std::max(1, crt.value("_runtime_revision", 1));
                crt["_runtime_phase"] = 0.f;
                crt["_runtime_accumulator"] = 0.f;
            }
            if (crt.value("request_update", false)) {
                crt["request_update"] = false;
                crt["_runtime_revision"] = crt.value("_runtime_revision", 1) + 1;
            }
            if (crt.value("update_mode", std::string("on_demand")) != "realtime") continue;

            const float interval = std::clamp(crt.value("update_interval", 1.f / 15.f), 1.f / 120.f, 5.f);
            const float accumulator = crt.value("_runtime_accumulator", 0.f) + std::max(0.f, dt);
            if (accumulator < interval) {
                crt["_runtime_accumulator"] = accumulator;
                continue;
            }
            // A frame hitch cannot cause a burst of backlogged GPU uploads.
            crt["_runtime_accumulator"] = std::fmod(accumulator, interval);
            crt["_runtime_phase"] = crt.value("_runtime_phase", 0.f)
                                    + interval * crt.value("animation_speed", 1.f);
            crt["_runtime_revision"] = crt.value("_runtime_revision", 1) + 1;
        }
    }
};

// ─── VideoPlayer2DSystem ────────────────────────────────────────────────────
// VideoPlayer2D uses animated GIF clips in v1 so playback is self-contained in
// every Windows export.  The frame decoder/upload lives in TextureCache; this
// system owns play/pause/restart state and the component's monotonic clock.
class VideoPlayer2DSystem {
public:
    void update(EntityList& entities, float dt) {
        for (auto& entity : entities) {
            if (!entity_active(entity) || !has_component(entity, "VideoPlayer2D")) continue;
            auto& video = entity["components"]["VideoPlayer2D"];
            if (!video.value("_runtime_initialized", false)) {
                video["_runtime_initialized"] = true;
                video["playing"] = video.value("play_on_awake", false);
                video["playback_time"] = std::max(0.f, video.value("playback_time", 0.f));
            }
            const std::string clip = video.value("clip", std::string());
            if (clip.empty()) {
                video["_runtime_error"] = "Choose an animated GIF clip.";
                continue;
            }
            std::string extension = std::filesystem::path(clip).extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (extension != ".gif") {
                video["_runtime_error"] = "VideoPlayer2D currently accepts animated GIF clips.";
                video["playing"] = false;
                continue;
            }
            video["_runtime_error"] = "";
            if (video.value("restart", false)) {
                video["restart"] = false;
                video["playback_time"] = 0.f;
                video["playing"] = true;
            }
            if (video.value("playing", false)) {
                const float speed = std::clamp(video.value("playback_speed", 1.f), 0.f, 8.f);
                video["playback_time"] = std::max(0.f, video.value("playback_time", 0.f) + std::max(0.f, dt) * speed);
            }
        }
    }
};

// Health is deliberately a runtime system rather than an Inspector-only bag
// of fields.  Native scripts, visual scripts, collision damage and network
// replication can all change current_health; this system normalizes those
// writes, enforces i-frames, emits the authored events, and handles optional
// destruction exactly once.
class HealthSystem {
public:
    void update(EntityList& entities, float dt) {
        for (auto& entity : entities) {
            if (!entity_active(entity) || !has_component(entity, "HealthComponent")) continue;
            auto& health = entity["components"]["HealthComponent"];
            const float maximum = std::max(0.001f, health.value("max_health", 100.f));
            health["max_health"] = maximum;

            float current = std::clamp(health.value("current_health", maximum), 0.f, maximum);
            float remaining = std::max(0.f, health.value("_invincibility_remaining", 0.f) - std::max(0.f, dt));
            health["_invincibility_remaining"] = remaining;
            const bool primed = health.value("_runtime_primed", false);
            const float previous = std::clamp(health.value("_last_health", current), 0.f, maximum);

            // A direct assignment while invulnerable should behave like a
            // scripted TakeDamage call: retain the old value, rather than
            // silently allowing a second hit through the component field.
            if (primed && current < previous && (remaining > 0.f || health.value("invincible", false))) {
                current = previous;
            }
            health["current_health"] = current;

            if (!primed) {
                health["_runtime_primed"] = true;
                health["_last_health"] = current;
                health["_runtime_dead"] = current <= 0.f;
                continue;
            }

            if (current < previous) {
                Entity payload = Entity::object();
                payload["entity_id"] = entity.value("id", -1);
                payload["amount"] = previous - current;
                payload["current_health"] = current;
                payload["max_health"] = maximum;
                EventBus::instance().emit("HealthDamaged", payload, &entity);
                const std::string custom = health.value("on_damage_event", std::string());
                if (!custom.empty()) EventBus::instance().emit(custom, payload, &entity);
                const float iframe = std::max(0.f, health.value("invincibility_time", 0.f));
                if (iframe > 0.f) health["_invincibility_remaining"] = iframe;
            }

            const bool was_dead = health.value("_runtime_dead", false);
            const bool dead = current <= 0.f;
            if (dead && !was_dead) {
                Entity payload = Entity::object();
                payload["entity_id"] = entity.value("id", -1);
                payload["current_health"] = current;
                payload["max_health"] = maximum;
                EventBus::instance().emit("HealthDied", payload, &entity);
                const std::string custom = health.value("on_death_event", std::string());
                if (!custom.empty()) EventBus::instance().emit(custom, payload, &entity);
                if (health.value("auto_destroy_on_death", false)) entity["active"] = false;
            }
            health["_runtime_dead"] = dead;
            health["_last_health"] = current;
        }
    }
};

// Runtime ScriptableObject resolver.  The editor owns authoring of .sobj
// assets; this system gives every reference a safe project-relative loaded
// data object in Play/standalone mode and refreshes it when its source file
// changes.  The loaded data is a deep copy per entity so gameplay state does
// not leak between two references to the same authored asset.
class ScriptableObjectSystem {
public:
    void set_asset_dir(std::string asset_dir) { _asset_dir = std::move(asset_dir); _cache.clear(); }

    void update(EntityList& entities) {
        for (auto& entity : entities) {
            if (!entity_active(entity) || !has_component(entity, "ScriptableObjectRef")) continue;
            auto& reference = entity["components"]["ScriptableObjectRef"];
            const std::string authored = reference.value("asset_path", std::string());
            if (authored.empty()) {
                reference.erase("data");
                reference["_load_error"] = "Select a .sobj asset.";
                continue;
            }
            const std::filesystem::path path = _resolve(authored);
            if (path.empty()) {
                reference.erase("data");
                reference["_load_error"] = "Scriptable object must be inside the active project.";
                continue;
            }
            std::error_code ec;
            const auto modified = std::filesystem::last_write_time(path, ec);
            if (ec) {
                reference.erase("data");
                reference["_load_error"] = "Could not read .sobj asset.";
                continue;
            }
            const std::string key = path.lexically_normal().generic_string();
            auto found = _cache.find(key);
            if (found == _cache.end() || found->second.modified != modified) {
                std::ifstream stream(path, std::ios::binary);
                nlohmann::json json;
                try {
                    stream >> json;
                    if (!json.is_object()) throw std::runtime_error("root is not an object");
                    Entity loaded;
                    loaded = json;
                    _cache[key] = {modified, std::move(loaded)};
                } catch (...) {
                    reference.erase("data");
                    reference["_load_error"] = "Invalid .sobj JSON.";
                    continue;
                }
                found = _cache.find(key);
            }
            reference["data"] = found->second.data.deep_clone();
            reference["type_name"] = found->second.data.value("type", reference.value("type_name", std::string()));
            reference["_load_error"] = "";
            reference["_resolved_asset"] = key;
        }
    }

private:
    struct Cached { std::filesystem::file_time_type modified{}; Entity data; };

    std::filesystem::path _resolve(const std::string& authored) const {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path root = fs::weakly_canonical(fs::path(_asset_dir), ec);
        if (ec || root.empty()) return {};
        fs::path candidate(authored);
        if (candidate.is_relative()) candidate = root / candidate;
        candidate = fs::weakly_canonical(candidate, ec);
        if (ec) return {};
        const std::string root_text = root.generic_string();
        const std::string candidate_text = candidate.generic_string();
        if (candidate_text.size() < root_text.size() || candidate_text.compare(0, root_text.size(), root_text) != 0 ||
            (candidate_text.size() > root_text.size() && candidate_text[root_text.size()] != '/')) return {};
        return candidate;
    }

    std::string _asset_dir;
    std::unordered_map<std::string, Cached> _cache;
};

// Timed prefab spawner.  It keeps spawned roots associated with their owner,
// honours max_count, reuses inactive previous roots before allocating more,
// and loads only from the active project asset directory when a relative
// prefab reference is used.  This makes the Inspector fields operational in
// a standalone game rather than merely serializing a path.
class Spawner2DSystem {
public:
    void set_asset_dir(std::string asset_dir) { _asset_dir = std::move(asset_dir); }

    void update(EntityList& entities, float dt) {
        std::vector<Entity> staged;
        for (auto& owner : entities) {
            if (!entity_active(owner) || !has_component(owner, "Spawner2D")) continue;
            auto& spawner = owner["components"]["Spawner2D"];
            if (!spawner.value("enabled", true)) continue;
            int owner_id = owner.value("id", -1);
            if (owner_id < 0) {
                owner_id = next_entity_id();
                owner["id"] = owner_id;
            }
            const int maximum = std::max(0, spawner.value("max_count", 10));
            int active_count = 0;
            for (const auto& candidate : entities) {
                if (entity_active(candidate) && candidate.value("_spawner_owner", -1) == owner_id &&
                    candidate.value("_spawner_root", false)) ++active_count;
            }
            for (const auto& candidate : staged) {
                if (candidate.value("_spawner_owner", -1) == owner_id && candidate.value("_spawner_root", false))
                    ++active_count;
            }

            auto spawn_once = [&]() -> bool {
                if (maximum > 0 && active_count >= maximum) return false;
                const auto owner_world = has_component(owner, "Transform")
                    ? transform::cached_world(owner) : transform::WorldTRS{};
                const int ordinal = spawner.value("_spawn_ordinal", 0);
                const float radius = std::max(0.f, spawner.value("spawn_radius", 0.f));
                const float theta = (float)ordinal * 2.39996323f;
                const float x = owner_world.x + spawner.value("offset_x", 0.f) + std::cos(theta) * radius;
                const float y = owner_world.y + spawner.value("offset_y", 0.f) + std::sin(theta) * radius;

                // Recycle a returned root before allocating a new prefab tree.
                for (auto& candidate : entities) {
                    if (entity_active(candidate) || candidate.value("_spawner_owner", -1) != owner_id ||
                        !candidate.value("_spawner_root", false)) continue;
                    candidate["active"] = true;
                    if (has_component(candidate, "Transform")) {
                        auto& transform_component = candidate["components"]["Transform"];
                        transform_component["x"] = x;
                        transform_component["y"] = y;
                    }
                    if (spawner.value("inherit_velocity", false) && has_component(owner, "Rigidbody2D") &&
                        has_component(candidate, "Rigidbody2D")) {
                        const auto& source = owner["components"]["Rigidbody2D"];
                        auto& destination = candidate["components"]["Rigidbody2D"];
                        destination["velocity_x"] = source.value("velocity_x", 0.f);
                        destination["velocity_y"] = source.value("velocity_y", 0.f);
                    }
                    spawner["_spawn_ordinal"] = ordinal + 1;
                    ++active_count;
                    return true;
                }

                Entity prefab_root = _load_prefab(spawner.value("prefab", std::string()));
                if (!prefab_root.is_object()) {
                    spawner["_last_error"] = "Select a valid project prefab before spawning.";
                    return false;
                }
                _stage_tree(prefab_root, -1, owner_id, x, y, staged, spawner, owner);
                spawner["_last_error"] = "";
                spawner["_spawn_ordinal"] = ordinal + 1;
                ++active_count;
                return true;
            };

            const bool started = spawner.value("_runtime_started", false);
            if (!started) {
                spawner["_runtime_started"] = true;
                spawner["_timer"] = 0.f;
                if (spawner.value("spawn_on_start", false)) spawn_once();
            }
            float timer = std::max(0.f, spawner.value("_timer", 0.f)) + std::max(0.f, dt);
            const float interval = std::max(0.001f, spawner.value("interval", 1.f));
            while (timer >= interval) {
                timer -= interval;
                if (!spawn_once()) { timer = std::min(timer, interval); break; }
            }
            spawner["_timer"] = timer;
        }
        if (!staged.empty()) {
            entities.reserve(std::max(entities.capacity(), entities.size() + staged.size() + 16));
            for (auto& entity : staged) entities.push_back(std::move(entity));
            transform::invalidate_all();
        }
    }

private:
    Entity _load_prefab(const std::string& raw_path) const {
        if (raw_path.empty()) return Entity{};
        namespace fs = std::filesystem;
        fs::path path(raw_path);
        if (path.is_relative()) path = fs::path(_asset_dir) / path;
        std::ifstream stream(path, std::ios::binary);
        if (!stream) return Entity{};
        try {
            nlohmann::json document;
            stream >> document;
            const nlohmann::json& root = document.contains("root") ? document["root"] : document;
            Entity result;
            result = root; // public runtime-value conversion; keeps this system independent of editor JSON helpers
            return result;
        } catch (...) {
            return Entity{};
        }
    }

    static void _stage_tree(Entity node, int parent_id, int owner_id, float root_x, float root_y,
                            std::vector<Entity>& staged, Entity& spawner, const Entity& owner) {
        Entity children = Entity::array();
        if (node.contains("_prefab_children")) children = node["_prefab_children"];
        else if (node.contains("children")) children = node["children"];
        node.erase("_prefab_children");
        node.erase("children");
        node["id"] = next_entity_id();
        const int node_id = node.value("id", -1);
        node["active"] = true;
        node["_spawner_owner"] = owner_id;
        node["_spawner_root"] = parent_id < 0;
        if (!has_component(node, "Transform")) node["components"]["Transform"] = Entity::object();
        auto& transform_component = node["components"]["Transform"];
        if (parent_id < 0) {
            transform_component["x"] = root_x;
            transform_component["y"] = root_y;
            transform_component.erase("parent");
            if (spawner.value("inherit_velocity", false) && has_component(owner, "Rigidbody2D") &&
                has_component(node, "Rigidbody2D")) {
                const auto& source = owner["components"]["Rigidbody2D"];
                auto& destination = node["components"]["Rigidbody2D"];
                destination["velocity_x"] = source.value("velocity_x", 0.f);
                destination["velocity_y"] = source.value("velocity_y", 0.f);
            }
        } else {
            transform_component["parent"] = parent_id;
        }
        staged.push_back(node);
        if (!children.is_array()) return;
        for (const auto& child : children)
            if (child.is_object()) _stage_tree(child, node_id, owner_id, root_x, root_y, staged, spawner, owner);
    }

    std::string _asset_dir;
};

// Two-bone 2D inverse kinematics.  The component can name the three bones
// explicitly or be placed on the root of a simple parented chain, in which
// case its first two Transform children are used.  Rotations are written as
// ordinary local Transform rotations, so animation, the editor viewport and
// exported builds all consume the same result.
class LimbIK2DSystem {
public:
    void update(EntityList& entities) {
        bool changed = false;
        for (auto& host : entities) {
            if (!entity_active(host) || !has_component(host, "LimbIK2D")) continue;
            auto& ik = host["components"]["LimbIK2D"];
            if (!ik.value("enabled", true)) continue;

            Entity* root = _find(entities, ik.value("root_entity", host.value("id", -1)));
            Entity* middle = _find(entities, ik.value("mid_entity", -1));
            Entity* end = _find(entities, ik.value("end_entity", -1));
            if (!root) root = &host;
            if (!middle) middle = _first_child(entities, root->value("id", -1));
            if (!end && middle) end = _first_child(entities, middle->value("id", -1));
            if (!root || !middle || !end || !has_component(*root, "Transform") ||
                !has_component(*middle, "Transform") || !has_component(*end, "Transform")) continue;

            float target_x = ik.value("target_x", 0.f);
            float target_y = ik.value("target_y", 0.f);
            if (Entity* target = _find(entities, ik.value("target_entity", -1)); target && has_component(*target, "Transform")) {
                const auto target_world = transform::cached_world(*target);
                target_x = target_world.x;
                target_y = target_world.y;
            }

            const auto root_world = transform::cached_world(*root);
            const auto middle_world = transform::cached_world(*middle);
            const auto end_world = transform::cached_world(*end);
            const float l1 = std::max(.001f, ik.value("length1", std::hypot(middle_world.x - root_world.x, middle_world.y - root_world.y)));
            const float l2 = std::max(.001f, ik.value("length2", std::hypot(end_world.x - middle_world.x, end_world.y - middle_world.y)));
            float dx = target_x - root_world.x, dy = target_y - root_world.y;
            float distance = std::hypot(dx, dy);
            if (distance < .0001f) continue;
            distance = std::clamp(distance, std::abs(l1 - l2) + .0001f, l1 + l2 - .0001f);
            const float base = std::atan2(dy, dx);
            const float root_cos = std::clamp((l1*l1 + distance*distance - l2*l2) / (2.f*l1*distance), -1.f, 1.f);
            const float middle_cos = std::clamp((l1*l1 + l2*l2 - distance*distance) / (2.f*l1*l2), -1.f, 1.f);
            int bend = ik.value("bend_direction", 1) < 0 ? -1 : 1;
            if (Entity* pole = _find(entities, ik.value("pole_entity", -1)); pole && has_component(*pole, "Transform")) {
                const auto pole_world = transform::cached_world(*pole);
                const float cross = dx * (pole_world.y - root_world.y) - dy * (pole_world.x - root_world.x);
                if (std::abs(cross) > .0001f) bend = cross > 0.f ? 1 : -1;
            }
            const float root_world_angle = (base - bend * std::acos(root_cos)) * ENGINE_RAD2DEG;
            const float middle_world_angle = root_world_angle + bend * (GAMEENGINE_PI - std::acos(middle_cos)) * ENGINE_RAD2DEG;
            const float weight = std::clamp(ik.value("weight", 1.f), 0.f, 1.f);
            _set_world_rotation(*root, root_world_angle, weight);
            _set_world_rotation(*middle, middle_world_angle, weight);
            ik["_solved"] = true;
            changed = changed || weight > 0.f;
        }
        if (changed) transform::invalidate_all();
    }

private:
    static Entity* _find(EntityList& entities, int id) {
        if (id < 0) return nullptr;
        for (auto& entity : entities) if (entity.value("id", -1) == id) return &entity;
        return nullptr;
    }

    static Entity* _first_child(EntityList& entities, int parent_id) {
        if (parent_id < 0) return nullptr;
        for (auto& entity : entities)
            if (entity_active(entity) && transform::parent_id_of(entity) == parent_id) return &entity;
        return nullptr;
    }

    static float _wrap_degrees(float value) {
        while (value > 180.f) value -= 360.f;
        while (value < -180.f) value += 360.f;
        return value;
    }

    static void _set_world_rotation(Entity& entity, float desired_world_rotation, float weight) {
        auto& local = entity["components"]["Transform"];
        float parent_rotation = 0.f;
        const int parent_id = transform::parent_id_of(entity);
        if (parent_id >= 0) {
            const auto index = transform::node_index(parent_id);
            if (index != transform::npos()) {
                const Entity* parent = transform::registry().nodes[index].entity;
                if (parent) parent_rotation = transform::cached_world(*parent).rotation;
            }
        }
        const float target_local = desired_world_rotation - parent_rotation;
        const float current_local = local.value("rotation", 0.f);
        local["rotation"] = current_local + _wrap_degrees(target_local - current_local) * weight;
    }
};

// Unity-style object pooling: a named pool pre-instantiates entities from a
// prefab key and recycles them instead of destroying/creating every time.
// Entities carry an "ObjectPool" component with pool_key, max_size, and
// pre_warm fields. Pooled entities are deactivated (active=false) rather than
// removed from the entity list; Get() reactivates the oldest idle one.
// Mirrors Unity's ObjectPool<T> / PoolManager workflow.
class ObjectPool2DSystem {
public:
    struct PoolEntry { int entity_id; bool in_use; };
    using Pool = std::vector<PoolEntry>;

    // Pre-warm: find all entities with ObjectPool component and cache them.
    void init(EntityList& entities) {
        _pools.clear();
        for (auto& e : entities) {
            if (!has_component(e, "ObjectPool")) continue;
            auto& pc = e["components"]["ObjectPool"];
            std::string key = pc.value("pool_key", "");
            if (key.empty()) continue;
            PoolEntry entry;
            entry.entity_id = e.value("id", -1);
            entry.in_use    = entity_active(e);
            _pools[key].push_back(entry);
        }
    }

    // Get: return the id of a free entity from the named pool, or -1 if full.
    // Reactivates the entity and marks it in_use.
    int get(const std::string& key, EntityList& entities) {
        auto it = _pools.find(key);
        if (it == _pools.end()) return -1;
        for (auto& pe : it->second) {
            if (pe.in_use) continue;
            for (auto& e : entities) {
                if (e.value("id",-1) != pe.entity_id) continue;
                e["active"] = true;
                pe.in_use = true;
                // Fire OnSpawnFromPool callback via EventBus
                Entity payload; payload["pool_key"] = key;
                EventBus::instance().emit("OnSpawnFromPool", payload, &e);
                return pe.entity_id;
            }
        }
        return -1; // pool exhausted
    }

    // Return: deactivate an entity and mark it idle.
    void ret(int entity_id, EntityList& entities) {
        for (auto& e : entities) {
            if (e.value("id",-1) != entity_id) continue;
            e["active"] = false;
            // Reset common transient state
            if (has_component(e, "Rigidbody2D")) {
                e["components"]["Rigidbody2D"]["velocity_x"] = 0.f;
                e["components"]["Rigidbody2D"]["velocity_y"] = 0.f;
            }
            Entity payload; payload["entity_id"] = entity_id;
            EventBus::instance().emit("OnReturnToPool", payload, &e);
            break;
        }
        for (auto& [key, pool] : _pools) {
            for (auto& pe : pool) {
                if (pe.entity_id == entity_id) { pe.in_use = false; return; }
            }
        }
    }

    // Auto-return entities whose "pool_lifetime" timer expires.
    void update(EntityList& entities, float dt) {
        for (auto& e : entities) {
            if (!entity_active(e)) continue;
            if (!has_component(e, "ObjectPool")) continue;
            auto& pc = e["components"]["ObjectPool"];
            float lifetime = pc.value("pool_lifetime", 0.f);
            if (lifetime <= 0.f) continue;
            float elapsed = pc.value("_pool_elapsed", 0.f) + dt;
            pc["_pool_elapsed"] = elapsed;
            if (elapsed >= lifetime) {
                pc["_pool_elapsed"] = 0.f;
                ret(e.value("id",-1), entities);
            }
        }
    }

    ObjectPool2DSystem& instance() { static ObjectPool2DSystem inst; return inst; }
private:
    std::unordered_map<std::string, Pool> _pools;
};

// ─── TriggerSystem ────────────────────────────────────────────────────────────
// Fires OnTriggerEnter2D / OnTriggerStay2D / OnTriggerExit2D events via
// EventBus, exactly mirroring Unity's MonoBehaviour callbacks. Works against
// the engine's existing Collider2D AABB data stored on each entity by the
// physics system (_col_aabb). Only entities with isTrigger=true are checked.
class TriggerSystem {
    using IDPair = std::pair<int,int>;
    std::unordered_set<uint64_t> _active;  // currently overlapping pairs

    static uint64_t pair_key(int a, int b) {
        if (a > b) std::swap(a,b);
        return (uint64_t)(uint32_t)a << 32 | (uint32_t)b;
    }

    struct AABB { float x,y,w,h; };
    static bool overlaps(const AABB& a, const AABB& b) {
        return std::abs(a.x - b.x)*2.f < (a.w + b.w) &&
               std::abs(a.y - b.y)*2.f < (a.h + b.h);
    }

    // Pull AABB from physics-cached data or from BoxCollider2D component.
    static bool get_aabb(const Entity& e, AABB& out) {
        if (e.contains("_col_aabb")) {
            auto& bb = e["_col_aabb"];
            out = { bb.value("x",0.f), bb.value("y",0.f),
                    bb.value("w",0.f), bb.value("h",0.f) };
            return out.w > 0.f;
        }
        if (has_component(e,"BoxCollider2D")) {
            auto& c = e["components"]["BoxCollider2D"];
            float wx = has_component(e,"Transform") ? transform::world_x(e) : 0.f;
            float wy = has_component(e,"Transform") ? transform::world_y(e) : 0.f;
            out = { wx + c.value("offset_x",0.f), wy + c.value("offset_y",0.f),
                    c.value("width",0.f), c.value("height",0.f) };
            return out.w > 0.f;
        }
        return false;
    }

public:
    void update(EntityList& entities, float /*dt*/) {
        // Collect trigger entities
        std::vector<Entity*> triggers;
        for (auto& e : entities) {
            if (!entity_active(e)) continue;
            bool is_trigger = false;
            if (has_component(e,"BoxCollider2D") && e["components"]["BoxCollider2D"].value("isTrigger",false)) is_trigger=true;
            if (has_component(e,"CircleCollider2D") && e["components"]["CircleCollider2D"].value("isTrigger",false)) is_trigger=true;
            if (is_trigger) triggers.push_back(&e);
        }

        std::unordered_set<uint64_t> current_frame;
        for (size_t i=0; i<triggers.size(); ++i) {
            for (size_t j=i+1; j<triggers.size(); ++j) {
                Entity* ea = triggers[i]; Entity* eb = triggers[j];
                int ia = ea->value("id",-1), ib = eb->value("id",-1);
                AABB aa{}, ab{};
                if (!get_aabb(*ea,aa) || !get_aabb(*eb,ab)) continue;
                bool over = overlaps(aa,ab);
                uint64_t key = pair_key(ia,ib);
                bool was_over = _active.count(key)>0;
                if (over) {
                    current_frame.insert(key);
                    Entity payload; payload["other_id"] = ib;
                    if (!was_over) {
                        EventBus::instance().emit("OnTriggerEnter2D", payload, ea);
                        payload["other_id"] = ia;
                        EventBus::instance().emit("OnTriggerEnter2D", payload, eb);
                    } else {
                        EventBus::instance().emit("OnTriggerStay2D", payload, ea);
                        payload["other_id"] = ia;
                        EventBus::instance().emit("OnTriggerStay2D", payload, eb);
                    }
                } else if (was_over) {
                    Entity payload; payload["other_id"] = ib;
                    EventBus::instance().emit("OnTriggerExit2D", payload, ea);
                    payload["other_id"] = ia;
                    EventBus::instance().emit("OnTriggerExit2D", payload, eb);
                }
            }
        }
        _active = std::move(current_frame);
    }
};

// ─── FlockingSystem (2D Steering / Boids) ────────────────────────────────────
// Unity has built-in NavMeshAgent and steering; this adds Craig Reynolds boids
// (separation, alignment, cohesion) plus seek/flee/arrive/wander behaviours —
// all absent in the existing NavMesh2DSystem which only does A* pathfinding.
// Entities carry a "Flock2D" component with tunable weights; velocity is
// written into Rigidbody2D so physics still drives movement.
class FlockingSystem {
public:
    void update(EntityList& entities, float dt) {
        // Collect all flocking entities grouped by flock_id
        struct FlockAgent { Entity* e; float x,y,vx,vy; };
        std::unordered_map<std::string, std::vector<FlockAgent>> groups;

        for (auto& e : entities) {
            if (!entity_active(e)) continue;
            if (!has_component(e,"Flock2D")) continue;
            if (!has_component(e,"Rigidbody2D")) continue;
            std::string fid = e["components"]["Flock2D"].value("flock_id","default");
            FlockAgent ag;
            ag.e  = &e;
            ag.x  = transform::world_x(e);
            ag.y  = transform::world_y(e);
            ag.vx = e["components"]["Rigidbody2D"].value("velocity_x",0.f);
            ag.vy = e["components"]["Rigidbody2D"].value("velocity_y",0.f);
            groups[fid].push_back(ag);
        }

        for (auto& [fid, agents] : groups) {
            for (size_t i=0; i<agents.size(); ++i) {
                auto& ag  = agents[i];
                auto& fc  = (*ag.e)["components"]["Flock2D"];
                float radius    = fc.value("perception_radius", 80.f);
                float sep_w     = fc.value("separation_weight", 1.5f);
                float ali_w     = fc.value("alignment_weight",  1.0f);
                float coh_w     = fc.value("cohesion_weight",   1.0f);
                float max_speed = fc.value("max_speed", 120.f);
                float max_force = fc.value("max_force",  60.f);

                float sep_x=0,sep_y=0, ali_x=0,ali_y=0, coh_x=0,coh_y=0;
                int neighbours = 0;

                for (size_t j=0; j<agents.size(); ++j) {
                    if (i==j) continue;
                    float dx = ag.x - agents[j].x;
                    float dy = ag.y - agents[j].y;
                    float d  = std::sqrt(dx*dx+dy*dy);
                    if (d > radius || d < 0.001f) continue;
                    ++neighbours;
                    // Separation: push away, weighted by 1/d
                    sep_x += dx/d; sep_y += dy/d;
                    // Alignment: match average velocity
                    ali_x += agents[j].vx; ali_y += agents[j].vy;
                    // Cohesion: steer toward average position
                    coh_x += agents[j].x; coh_y += agents[j].y;
                }

                float steer_x=0, steer_y=0;
                if (neighbours > 0) {
                    float n = (float)neighbours;
                    // Alignment — normalise average vel then scale
                    ali_x/=n; ali_y/=n;
                    float al = std::sqrt(ali_x*ali_x+ali_y*ali_y);
                    if (al>0.001f){ ali_x=ali_x/al*max_speed; ali_y=ali_y/al*max_speed; }
                    // Cohesion — steer toward centroid
                    coh_x=coh_x/n-ag.x; coh_y=coh_y/n-ag.y;
                    float cl = std::sqrt(coh_x*coh_x+coh_y*coh_y);
                    if (cl>0.001f){ coh_x=coh_x/cl*max_speed; coh_y=coh_y/cl*max_speed; }

                    steer_x = sep_x*sep_w + ali_x*ali_w + coh_x*coh_w;
                    steer_y = sep_y*sep_w + ali_y*ali_w + coh_y*coh_w;
                }

                // Optional seek target
                if (fc.value("seek_enabled",false)) {
                    float tx=fc.value("seek_x",0.f), ty=fc.value("seek_y",0.f);
                    float dx=tx-ag.x, dy=ty-ag.y;
                    float d=std::sqrt(dx*dx+dy*dy);
                    if (d>0.001f){
                        float seek_w=fc.value("seek_weight",1.f);
                        steer_x+=dx/d*max_speed*seek_w;
                        steer_y+=dy/d*max_speed*seek_w;
                    }
                }

                // Clamp force and integrate
                float fl=std::sqrt(steer_x*steer_x+steer_y*steer_y);
                if (fl>max_force&&fl>0.001f){ steer_x=steer_x/fl*max_force; steer_y=steer_y/fl*max_force; }
                auto& rb = (*ag.e)["components"]["Rigidbody2D"];
                float nvx = rb.value("velocity_x",0.f) + steer_x*dt;
                float nvy = rb.value("velocity_y",0.f) + steer_y*dt;
                float spd = std::sqrt(nvx*nvx+nvy*nvy);
                if (spd>max_speed&&spd>0.001f){ nvx=nvx/spd*max_speed; nvy=nvy/spd*max_speed; }
                rb["velocity_x"] = nvx;
                rb["velocity_y"] = nvy;
            }
        }
    }
};

// ─── LODGroupSystem (Level of Detail for 2D sprites) ─────────────────────────
// Unity's LODGroup swaps meshes by camera distance; in 2D this swaps the
// SpriteRenderer's texture (or disables the entity entirely) based on the
// orthographic-camera pixel size of the entity. Entities carry an "LODGroup2D"
// component with an array of LOD levels, each specifying a screen_threshold
// (fraction of screen height 0–1) and a texture path.
// This fills a genuine Unity gap: there is no equivalent of LODGroup in the
// existing engine, so distant sprite-heavy scenes have no automatic detail
// reduction mechanism.
class LODGroupSystem {
public:
    void update(EntityList& entities, float dt) {
        // Find active Camera2D
        float ortho = 5.f, cam_y_pixels = 600.f;
        for (auto& e : entities) {
            if (!entity_active(e)) continue;
            if (!has_component(e,"Camera2D")) continue;
            auto& cam = e["components"]["Camera2D"];
            if (!cam.value("is_main",false)) continue;
            ortho          = cam.value("orthographic_size", 5.f);
            cam_y_pixels   = cam.value("screen_height", 600.f);
            break;
        }
        float units_per_pixel = (ortho * 2.f) / cam_y_pixels;

        for (auto& e : entities) {
            if (!entity_active(e)) continue;
            if (!has_component(e,"LODGroup2D")) continue;
            auto& lg = e["components"]["LODGroup2D"];
            if (!lg.contains("levels") || !lg["levels"].is_array()) continue;

            // Compute entity screen-height fraction
            float entity_size = lg.value("reference_height_units", 1.f);
            float screen_frac = entity_size / (units_per_pixel * cam_y_pixels);

            int chosen = -1;
            float best_thresh = 1e9f;
            for (int i=0; i<(int)lg["levels"].size(); ++i) {
                auto& lv = lg["levels"][i];
                float thresh = lv.value("screen_threshold", 0.f);
                if (screen_frac <= thresh && thresh < best_thresh) {
                    best_thresh = thresh;
                    chosen = i;
                }
            }

            if (chosen < 0) {
                // Below all LODs — cull (deactivate renderer)
                if (has_component(e,"SpriteRenderer"))
                    e["components"]["SpriteRenderer"]["enabled"] = false;
            } else {
                auto& lv = lg["levels"][chosen];
                std::string tex = lv.value("texture","");
                if (!tex.empty() && has_component(e,"SpriteRenderer")) {
                    e["components"]["SpriteRenderer"]["texture"]  = tex;
                    e["components"]["SpriteRenderer"]["enabled"]  = true;
                }
                // Optionally hide/show child entities by tag
                std::string show_tag = lv.value("show_tag","");
                if (!show_tag.empty()) {
                    // Mark so RenderSystem / ScriptSystem can filter by tag
                    lg["_active_lod_tag"] = show_tag;
                }
            }
            lg["_active_lod"] = chosen;
        }
    }
};

// ─── TilemapAnimationSystem ───────────────────────────────────────────────────
// Unity's Tilemap supports animated tiles via TileAnimationData; this system
// advances per-tile animation frames stored in the Tilemap component's
// "animated_tiles" map: { "col,row": { frames:[id,...], fps:8, _t:0.0 } }.
// The tile id written to grid[row][col] is replaced each frame, so the
// existing TilemapColliderSystem and render pipeline see the correct tile id
// without any other changes — same "mutate data so existing systems see it"
// pattern used throughout this engine.
class TilemapAnimationSystem {
public:
    void update(EntityList& entities, float dt) {
        for (auto& e : entities) {
            if (!entity_active(e)) continue;
            if (!has_component(e,"Tilemap")) continue;
            auto& tm = e["components"]["Tilemap"];
            if (!tm.contains("animated_tiles")) continue;
            auto& anim_tiles = tm["animated_tiles"];
            if (!anim_tiles.is_object()) continue;

            auto& grid = tm["grid"];
            for (auto& [cell_key, at] : anim_tiles.items()) {
                if (!at.is_object()) continue;
                float fps = at.value("fps", 8.f);
                if (fps <= 0.f) continue;
                float t = at.value("_t", 0.f) + dt * fps;
                auto& frames = at["frames"];
                if (!frames.is_array() || frames.empty()) continue;
                int frame_idx = (int)t % (int)frames.size();
                at["_t"] = t - std::floor(t / frames.size()) * frames.size(); // fmod
                int tile_id = frames[frame_idx].is_number()
                              ? frames[frame_idx].get<int>() : 0;

                // Parse "col,row" key
                size_t comma = cell_key.find(',');
                if (comma == std::string::npos) continue;
                int col = std::stoi(cell_key.substr(0, comma));
                int row = std::stoi(cell_key.substr(comma+1));
                if (row >= 0 && row < (int)grid.size() &&
                    col >= 0 && col < (int)grid[row].size()) {
                    grid[row][col] = tile_id;
                }
            }
        }
    }
};

// ─── SceneTransitionSystem ────────────────────────────────────────────────────
// Unity's SceneManager.LoadScene triggers a fade/transition animation before
// swapping scenes. This runtime system manages a screen-cover fade effect:
// when a "SceneTransition" component is added to an entity with a target_scene
// and transition_type, the system drives an _alpha value 0→1 (fade out) then
// emits "OnTransitionMidpoint" (where ScriptSystem loads the new scene) then
// fades 1→0. The alpha is exposed as a render overlay; RenderSystem checks
// _scene_transition_alpha on the engine state entity.
class SceneTransitionSystem {
public:
    enum class Phase { Idle, FadeOut, FadeIn };

    void update(EntityList& entities, float dt) {
        for (auto& e : entities) {
            if (!entity_active(e)) continue;
            if (!has_component(e,"SceneTransition")) continue;
            auto& sc = e["components"]["SceneTransition"];
            if (!sc.value("active",false)) continue;

            float dur = sc.value("duration", 0.5f);
            if (dur <= 0.f) dur = 0.001f;
            float alpha = sc.value("_alpha", 0.f);
            std::string phase_str = sc.value("_phase","fadeout");
            Phase phase = phase_str=="fadein" ? Phase::FadeIn : Phase::FadeOut;

            if (phase == Phase::FadeOut) {
                alpha = std::min(1.f, alpha + dt/dur);
                sc["_alpha"] = alpha;
                if (alpha >= 1.f) {
                    sc["_phase"] = "fadein";
                    // Signal scene swap
                    Entity payload;
                    payload["target_scene"] = sc.value("target_scene","");
                    payload["transition_type"] = sc.value("transition_type","fade");
                    EventBus::instance().emit("OnTransitionMidpoint", payload, &e);
                }
            } else {
                alpha = std::max(0.f, alpha - dt/dur);
                sc["_alpha"] = alpha;
                if (alpha <= 0.f) {
                    sc["active"] = false;
                    sc["_phase"] = "fadeout";
                    EventBus::instance().emit("OnTransitionComplete", Entity::object(), &e);
                }
            }

            // Write global overlay alpha so RenderSystem can composite it
            e["_scene_transition_alpha"] = alpha;
            std::string col_type = sc.value("transition_type","fade");
            e["_scene_transition_type"]  = col_type;
        }
    }

    // Call from game script to start a transition
    void begin(EntityList& entities, const std::string& target_scene,
               const std::string& type="fade", float duration=0.5f) {
        for (auto& e : entities) {
            if (!has_component(e,"SceneTransition")) continue;
            auto& sc = e["components"]["SceneTransition"];
            sc["active"]          = true;
            sc["target_scene"]    = target_scene;
            sc["transition_type"] = type;
            sc["duration"]        = duration;
            sc["_alpha"]          = 0.f;
            sc["_phase"]          = "fadeout";
            return;
        }
        // No SceneTransition entity — fire immediately
        Entity payload;
        payload["target_scene"] = target_scene;
        EventBus::instance().emit("OnTransitionMidpoint", payload, nullptr);
    }
};

// ─── FollowerSystem (Path Follow / Waypoint system) ──────────────────────────
// Unity's iTween / PathCreator / built-in path follow moves an entity along
// a sequence of waypoints with configurable speed and looping. Entities carry
// a "PathFollower2D" component with a "waypoints" array [{x,y},...],
// speed, loop (bool), ping_pong (bool), and look_at_direction (bool).
// Position is written to Transform every frame. Authors can use it for
// camera rails, moving platforms, and UI/world choreography without needing
// a Rigidbody2D.
class FollowerSystem {
public:
    void update(EntityList& entities, float dt) {
        for (auto& e : entities) {
            if (!entity_active(e)) continue;
            if (!has_component(e,"PathFollower2D")) continue;
            auto& pf = e["components"]["PathFollower2D"];
            auto& wps = pf["waypoints"];
            if (!wps.is_array() || wps.size() < 2) continue;

            float speed      = pf.value("speed", 80.f);
            bool  loop       = pf.value("loop",  true);
            bool  ping_pong  = pf.value("ping_pong", false);
            bool  look_dir   = pf.value("look_at_direction", true);
            int   idx        = pf.value("_wp_index", 0);
            float t_elapsed  = pf.value("_segment_t", 0.f);
            bool  going_fwd  = pf.value("_going_fwd", true);
            int   n          = (int)wps.size();

            if (idx < 0 || idx >= n-1) { idx=0; t_elapsed=0.f; }

            // Current segment
            auto& wa = wps[idx];
            auto& wb = wps[idx+1];
            float ax=wa.value("x",0.f), ay=wa.value("y",0.f);
            float bx=wb.value("x",0.f), by=wb.value("y",0.f);
            float seg_len = std::sqrt((bx-ax)*(bx-ax)+(by-ay)*(by-ay));
            if (seg_len < 0.001f) { idx++; pf["_wp_index"]=idx; continue; }

            float seg_frac = seg_len > 0.f ? speed*dt/seg_len : 1.f;
            t_elapsed += seg_frac;

            while (t_elapsed >= 1.f) {
                t_elapsed -= 1.f;
                if (going_fwd) {
                    idx++;
                    if (idx >= n-1) {
                        if (ping_pong)     { going_fwd=false; idx=n-2; }
                        else if (loop)     { idx=0; }
                        else               { idx=n-2; t_elapsed=1.f; break; }
                    }
                } else {
                    idx--;
                    if (idx < 0) {
                        going_fwd=true; idx=0;
                    }
                }
            }

            pf["_wp_index"]  = idx;
            pf["_segment_t"] = t_elapsed;
            pf["_going_fwd"] = going_fwd;

            // Interpolate position
            float wa2x_v=wps[idx].value("x",0.f), wa2y_v=wps[idx].value("y",0.f);
            float wb2x_v=wps[idx+1<n?idx+1:idx].value("x",0.f);
            float wb2y_v=wps[idx+1<n?idx+1:idx].value("y",0.f);
            float nx = wa2x_v + (wb2x_v-wa2x_v)*t_elapsed;
            float ny = wa2y_v + (wb2y_v-wa2y_v)*t_elapsed;

            if (has_component(e,"Transform")) {
                e["components"]["Transform"]["x"] = nx;
                e["components"]["Transform"]["y"] = ny;
                if (look_dir) {
                    float dx=wb2x_v-wa2x_v, dy=wb2y_v-wa2y_v;
                    float ang = std::atan2(dy,dx) * 57.2957795f;
                    e["components"]["Transform"]["rotation"] = ang;
                }
            }
        }
    }
};
