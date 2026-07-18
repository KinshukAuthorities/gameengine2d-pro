#pragma once

#include "runtime_value.hpp"
#include <algorithm>
#include <atomic>
#include <limits>
#include <string>
#include <vector>

using Entity = runtime::Value;
using EntityList = std::vector<Entity>;

inline std::atomic<int>& entity_id_counter() {
    static std::atomic<int> counter{1};
    return counter;
}

inline int next_entity_id() {
    return entity_id_counter().fetch_add(1, std::memory_order_relaxed);
}

// Scene files are user-authored and commonly carry their own positive IDs.
// Reserve the global runtime allocator past those IDs before scripts or visual
// graphs instantiate anything.  Without this, a freshly launched process
// starts allocating from 1 even if the loaded scene already owns 1..N,
// producing duplicate IDs and corrupting id-indexed physics/script state.
inline void reserve_entity_id_through(int used_id) {
    if (used_id < 0 || used_id == std::numeric_limits<int>::max()) return;
    const int desired = used_id + 1;
    auto& counter = entity_id_counter();
    int observed = counter.load(std::memory_order_relaxed);
    while (observed < desired &&
           !counter.compare_exchange_weak(observed, desired,
                                          std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {
        // compare_exchange refreshes `observed`; another runtime thread may
        // have already reserved a larger range, in which case this exits.
    }
}

inline void reserve_entity_ids(const EntityList& entities) {
    int largest = 0;
    for (const auto& entity : entities)
        largest = std::max(largest, entity.value("id", 0));
    reserve_entity_id_through(largest);
}

inline bool entity_active(const Entity& e) {
    return e.value("active", true);
}

inline Entity& get_component(Entity& e, const std::string& name) {
    return e["components"][name];
}

inline bool has_component(const Entity& e, const std::string& name) {
    return e.contains("components") && e["components"].contains(name) && !e["components"][name].is_null();
}

template <class T>
inline T get_value(const Entity& j, const std::string& key, T def = T{}) {
    if (!j.contains(key)) return def;
    const auto& v = j[key];
    return v.is_null() ? def : v.template get<T>(def);
}

inline float get_float(const Entity& j, const std::string& key, float def = 0.f) {
    return get_value<float>(j, key, def);
}

inline Entity make_object() { return Entity::object(); }
inline Entity make_array() { return Entity::array(); }
