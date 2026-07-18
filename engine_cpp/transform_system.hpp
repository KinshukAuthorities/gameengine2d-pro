#pragma once
/*
 * transform_system.hpp — 2D parent/child transform cache with lazy versioning.
 *
 * Key ideas:
 *   - Transform.x/y/rotation/scale_* stay as local values inside JSON.
 *   - World TRS is cached outside JSON.
 *   - A node is recomputed only when its own local revision changed or when a
 *     parent's world revision changed.
 *   - Hierarchy edits rebuild the index once; normal frame updates are O(dirty).
 */

#include "entity.hpp"
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace transform {

#ifndef ENGINE_DEG2RAD
#define ENGINE_DEG2RAD 0.017453292519943295f
#define ENGINE_RAD2DEG 57.29577951308232f
#endif

struct WorldTRS {
    float x = 0.f, y = 0.f;
    float rotation = 0.f;
    float scale_x = 1.f, scale_y = 1.f;
};

inline WorldTRS compose(const WorldTRS& parent, const WorldTRS& local) {
    const float rad = parent.rotation * ENGINE_DEG2RAD;
    const float c = std::cos(rad), s = std::sin(rad);
    const float lx = local.x * parent.scale_x;
    const float ly = local.y * parent.scale_y;
    WorldTRS out;
    out.x = parent.x + (lx * c - ly * s);
    out.y = parent.y + (lx * s + ly * c);
    out.rotation = parent.rotation + local.rotation;
    out.scale_x = parent.scale_x * local.scale_x;
    out.scale_y = parent.scale_y * local.scale_y;
    return out;
}

inline WorldTRS world_to_local(const WorldTRS& parent, const WorldTRS& world) {
    const float rad = -parent.rotation * ENGINE_DEG2RAD;
    const float c = std::cos(rad), s = std::sin(rad);
    const float dx = world.x - parent.x;
    const float dy = world.y - parent.y;
    const float rx = dx * c - dy * s;
    const float ry = dx * s + dy * c;
    const float psx = (std::abs(parent.scale_x) > 1e-8f) ? parent.scale_x : 1e-8f;
    const float psy = (std::abs(parent.scale_y) > 1e-8f) ? parent.scale_y : 1e-8f;
    WorldTRS out;
    out.x = rx / psx;
    out.y = ry / psy;
    out.rotation = world.rotation - parent.rotation;
    out.scale_x = world.scale_x / psx;
    out.scale_y = world.scale_y / psy;
    return out;
}

inline WorldTRS local_of(const Entity& e) {
    WorldTRS t;
    if (!has_component(e, "Transform")) return t;
    const auto& tr = e["components"]["Transform"];
    t.x = get_float(tr, "x", 0.f);
    t.y = get_float(tr, "y", 0.f);
    t.rotation = get_float(tr, "rotation", 0.f);
    t.scale_x = get_float(tr, "scale_x", 1.f);
    t.scale_y = get_float(tr, "scale_y", 1.f);
    return t;
}

inline int parent_id_of(const Entity& e) {
    if (!has_component(e, "Transform")) return -1;
    const auto& tr = e["components"]["Transform"];
    if (!tr.contains("parent") || !tr["parent"].is_number_integer()) return -1;
    return tr["parent"].get<int>();
}

struct NodeState {
    int id = -1;
    int parent = -1;
    Entity* entity = nullptr;
    WorldTRS local{};
    WorldTRS world{};
    bool has_local = false;
    bool has_world = false;
    uint32_t local_revision = 1;
    uint32_t local_revision_applied = 0;
    uint32_t world_revision = 1;
    uint32_t parent_world_revision_seen = 0;
    bool queued = false;
    std::vector<int> children;
};

struct Registry {
    std::unordered_map<int, std::size_t> index_by_id;
    std::vector<NodeState> nodes;
    std::vector<int> dirty_queue;
    bool structure_dirty = true;
};

inline Registry& registry() {
    static Registry r;
    return r;
}

inline std::size_t npos() {
    return std::numeric_limits<std::size_t>::max();
}

inline void invalidate_all() {
    registry().structure_dirty = true;
}

inline std::size_t node_index(int id) {
    auto& r = registry();
    auto it = r.index_by_id.find(id);
    return it == r.index_by_id.end() ? npos() : it->second;
}

inline NodeState* node_ptr(int id) {
    auto idx = node_index(id);
    if (idx == npos()) return nullptr;
    return &registry().nodes[idx];
}

inline void enqueue_dirty(int id) {
    auto& r = registry();
    auto idx = node_index(id);
    if (idx == npos()) {
        r.structure_dirty = true;
        return;
    }
    NodeState& n = r.nodes[idx];
    if (n.queued) return;
    n.queued = true;
    r.dirty_queue.push_back(id);
}

inline bool has_cached_world(const Entity& e) {
    if (registry().structure_dirty) return false;
    const int id = e.value("id", -1);
    if (id < 0) return false;
    auto idx = node_index(id);
    if (idx == npos()) return false;
    const NodeState& n = registry().nodes[idx];
    return n.has_world;
}

inline WorldTRS ensure_world_from_index(std::size_t idx, std::unordered_set<int>& visiting);
inline WorldTRS ensure_world_from_id(int id);
inline bool world_valid_by_index(std::size_t idx) {
    auto& r = registry();
    if (idx >= r.nodes.size()) return false;
    const NodeState& n = r.nodes[idx];
    if (!n.has_world) return false;
    if (n.local_revision_applied != n.local_revision) return false;
    if (n.parent < 0 || n.parent == n.id) return n.parent_world_revision_seen == 0;
    auto pit = r.index_by_id.find(n.parent);
    if (pit == r.index_by_id.end()) return false;
    return r.nodes[pit->second].world_revision == n.parent_world_revision_seen;
}


inline bool sync_local(NodeState& n) {
    if (!n.entity) return false;
    if (n.local_revision_applied == n.local_revision && n.has_local) return false;
    WorldTRS next = local_of(*n.entity);
    n.has_local = true;
    n.local = next;
    n.local_revision_applied = n.local_revision;
    return true;
}

inline WorldTRS ensure_world_from_index(std::size_t idx, std::unordered_set<int>& visiting) {
    auto& r = registry();
    if (idx >= r.nodes.size()) return {};

    NodeState& n = r.nodes[idx];
    if (!visiting.insert(n.id).second) {
        // Break a cycle defensively by treating this node as a root.
        if (!n.has_local) {
            if (n.entity) n.local = local_of(*n.entity);
            n.has_local = true;
        }
        n.world = n.local;
        n.has_world = true;
        n.parent_world_revision_seen = 0;
        n.world_revision++;
        visiting.erase(n.id);
        return n.world;
    }

    const bool local_changed = sync_local(n);

    WorldTRS parent_world{};
    uint32_t parent_world_rev = 0;
    bool parent_valid = false;
    if (n.parent >= 0 && n.parent != n.id) {
        auto pidx = node_index(n.parent);
        if (pidx != npos()) {
            parent_world = ensure_world_from_index(pidx, visiting);
            parent_world_rev = registry().nodes[pidx].world_revision;
            parent_valid = true;
        }
    }

    const bool needs_world = !n.has_world || local_changed ||
        (parent_valid && n.parent_world_revision_seen != parent_world_rev) ||
        (!parent_valid && n.parent_world_revision_seen != 0);

    if (needs_world) {
        if (!parent_valid || n.parent < 0 || n.parent == n.id) {
            n.world = n.local;
            n.parent_world_revision_seen = 0;
        } else {
            n.world = compose(parent_world, n.local);
            n.parent_world_revision_seen = parent_world_rev;
        }
        n.has_world = true;
        n.world_revision++;
    }

    visiting.erase(n.id);
    return n.world;
}

inline WorldTRS ensure_world_from_id(int id) {
    // NodeState owns raw Entity* values.  Callers without an EntityList cannot
    // rebuild safely, so refuse the stale cache until the next explicit
    // simulation/update boundary refreshes it.
    if (registry().structure_dirty) return {};
    auto idx = node_index(id);
    if (idx == npos()) return {};
    if (world_valid_by_index(idx)) return registry().nodes[idx].world;
    std::unordered_set<int> visiting;
    return ensure_world_from_index(idx, visiting);
}

inline WorldTRS cached_world(const Entity& e) {
    // `NodeState::entity` is a raw pointer into EntityList.  Scripts and
    // graphs are allowed to spawn/despawn objects, which can move the
    // vector's storage before TransformSystem gets its normal end-of-frame
    // update.  Never follow a cached NodeState while that refresh is pending:
    // it could point at the old vector allocation.  Returning the entity's
    // own local transform for this short hand-off is both safe and preferable
    // to crashing physics/rendering; `ensure_registry_current()` rebuilds the
    // complete hierarchy before the next physics step.
    if (registry().structure_dirty) return local_of(e);
    const int id = e.value("id", -1);
    if (id < 0) return local_of(e);
    auto idx = node_index(id);
    if (idx == npos()) return local_of(e);
    if (world_valid_by_index(idx)) return registry().nodes[idx].world;
    std::unordered_set<int> visiting;
    return ensure_world_from_index(idx, visiting);
}

inline WorldTRS world_trs(const Entity& e) {
    return cached_world(e);
}

inline void rebuild_registry(EntityList& entities) {
    auto& r = registry();
    r.index_by_id.clear();
    r.nodes.clear();
    r.nodes.reserve(entities.size());
    r.dirty_queue.clear();

    for (auto& e : entities) {
        const int id = e.value("id", -1);
        if (id < 0 || !has_component(e, "Transform")) continue;
        NodeState n;
        n.id = id;
        n.parent = parent_id_of(e);
        n.entity = &e;
        n.local = local_of(e);
        n.has_local = true;
        n.local_revision = 1;
        n.local_revision_applied = 1;
        n.world_revision = 1;
        n.parent_world_revision_seen = 0;
        r.index_by_id[id] = r.nodes.size();
        r.nodes.push_back(std::move(n));
    }

    for (auto& n : r.nodes) n.children.clear();
    for (auto& n : r.nodes) {
        if (n.parent >= 0) {
            auto pit = r.index_by_id.find(n.parent);
            if (pit != r.index_by_id.end() && n.parent != n.id)
                r.nodes[pit->second].children.push_back(n.id);
            else
                n.parent = -1;
        }
    }

    // Everything should be recalculated lazily after a structure rebuild.
    for (auto& n : r.nodes) {
        n.has_world = false;
        n.parent_world_revision_seen = 0;
        n.queued = true;
        r.dirty_queue.push_back(n.id);
    }

    r.structure_dirty = false;
}

// Use this at a simulation boundary after code that may have instantiated or
// removed entities.  Keeping it separate from `update()` lets physics refresh
// raw NodeState pointers before it asks for world-space collider transforms.
inline void ensure_registry_current(EntityList& entities) {
    if (registry().structure_dirty) rebuild_registry(entities);
}

class TransformSystem {
public:
    void update(EntityList& entities) {
        auto& r = registry();
        ensure_registry_current(entities);
        while (!r.dirty_queue.empty()) {
            int id = r.dirty_queue.back();
            r.dirty_queue.pop_back();
            auto idx = node_index(id);
            if (idx == npos()) continue;
            r.nodes[idx].queued = false;
            std::unordered_set<int> visiting;
            ensure_world_from_index(idx, visiting);
        }
    }

    Entity* find(int id) {
        if (registry().structure_dirty) return nullptr;
        auto idx = node_index(id);
        return idx == npos() ? nullptr : registry().nodes[idx].entity;
    }
};

inline float world_x(const Entity& e) {
    return cached_world(e).x;
}
inline float world_y(const Entity& e) {
    return cached_world(e).y;
}
inline float world_rotation(const Entity& e) {
    return cached_world(e).rotation;
}
inline float world_scale_x(const Entity& e) {
    return cached_world(e).scale_x;
}
inline float world_scale_y(const Entity& e) {
    return cached_world(e).scale_y;
}
inline std::pair<float,float> world_position(const Entity& e) {
    auto w = cached_world(e);
    return { w.x, w.y };
}

inline void mark_local_dirty(int id) {
    auto& r = registry();
    auto idx = node_index(id);
    if (idx == npos()) {
        r.structure_dirty = true;
        return;
    }
    NodeState& n = r.nodes[idx];
    ++n.local_revision;
    enqueue_dirty(id);
}

inline void mark_world_dirty(int id) {
    // World-space changes are typically written back into local space first.
    mark_local_dirty(id);
}

inline void mark_structure_dirty() {
    registry().structure_dirty = true;
}

// Cycle-safe SetParent. Keeps scene JSON as source of truth.
template <typename LookupFn>
inline bool set_parent(Entity& e, int new_parent_id, bool keep_world_position,
                       LookupFn lookup) {
    const int eid = e.value("id", 0);

    if (new_parent_id >= 0) {
        int cur = new_parent_id;
        int guard = 0;
        while (cur >= 0 && guard++ < 10000) {
            if (cur == eid) return false;
            Entity* p = lookup(cur);
            if (!p) break;
            cur = parent_id_of(*p);
        }
    }

    WorldTRS world_before = cached_world(e);

    if (!has_component(e, "Transform")) {
        e["components"]["Transform"] = {
            {"x", 0.f}, {"y", 0.f}, {"rotation", 0.f}, {"scale_x", 1.f}, {"scale_y", 1.f}
        };
    }
    auto& tr = e["components"]["Transform"];

    if (new_parent_id < 0) {
        tr["parent"] = -1;
        if (keep_world_position) {
            tr["x"] = world_before.x;
            tr["y"] = world_before.y;
            tr["rotation"] = world_before.rotation;
            tr["scale_x"] = world_before.scale_x;
            tr["scale_y"] = world_before.scale_y;
        }
        mark_structure_dirty();
        return true;
    }

    Entity* new_parent = lookup(new_parent_id);
    if (!new_parent) return false;
    tr["parent"] = new_parent_id;

    if (keep_world_position) {
        WorldTRS parent_world = cached_world(*new_parent);
        WorldTRS new_local = world_to_local(parent_world, world_before);
        tr["x"] = new_local.x;
        tr["y"] = new_local.y;
        tr["rotation"] = new_local.rotation;
        tr["scale_x"] = new_local.scale_x;
        tr["scale_y"] = new_local.scale_y;
    }

    mark_structure_dirty();
    return true;
}

template <typename LookupFn>
inline bool is_descendant_of(int candidate_id, int ancestor_id, LookupFn lookup) {
    int cur = candidate_id;
    int guard = 0;
    while (cur >= 0 && guard++ < 10000) {
        if (cur == ancestor_id) return true;
        Entity* p = lookup(cur);
        if (!p) return false;
        cur = parent_id_of(*p);
    }
    return false;
}

} // namespace transform
