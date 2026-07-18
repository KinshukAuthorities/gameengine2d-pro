#pragma once
// replication_props.hpp — general-purpose "replicate anything" property system.
//
// WHAT THIS FILE ADDS
// ───────────────────
// The existing replication.hpp covers existence (spawn/despawn), transform,
// health, and pickup. That leaves a large gap: if a script wants to sync
// an arbitrary field — a door's open/closed state, a coin's "collected" flag,
// a chest's loot tier, an enemy's alert state, a switch's activated flag —
// it has to hand-craft an event, serialise/deserialise it, guard with
// IsHost(), and wire the EventBus on both sides. Tedious and error-prone.
//
// This header gives scripts a one-liner API:
//
//   // In a CoinScript::on_trigger():
//   Replicate(self, "collected", true);
//
//   // In a DoorScript::update():
//   Replicate(self, "open", door_open_);
//
//   // In a BossScript (batching multiple fields in one packet):
//   ReplicateGroup(self, {{"phase", phase_}, {"enrage", enrage_}, {"shield", shield_hp_}});
//
// Host semantics (default, safe for most cases)
// ─────────────────────────────────────────────
// By default Replicate() is host-authoritative:
//   - Called on the HOST   → applies locally + broadcasts to all clients.
//   - Called on a CLIENT   → sends a ReplicateRequest to the host which
//                             validates (via an optional guard) and, if
//                             approved, applies+broadcasts.
// This matches the same model as RequestDamage/RequestPickup and keeps
// the host as the single source of truth.
//
// Client-authoritative shortcut
// ─────────────────────────────
// For properties that are OWNED by the calling client and can't be faked
// to meaningful advantage (cosmetics, chat bubbles, emoticons, local death
// animations, etc.) pass ReplicateMode::ClientAuth — the packet is sent
// direct to everyone without the host-validate round-trip.
//
// Reliable vs Unreliable
// ──────────────────────
// State that must never be missed (door toggled, coin collected) → reliable.
// State sent every frame (aim angle, charge %, animation blend) → unreliable
// (older update is superseded by the next tick anyway).
// Pass ReplicateFlags::Unreliable to opt into the cheaper path.
//
// Filtering / interest management
// ────────────────────────────────
// Pass a peer_id to ReplicateTo() to send only to one specific peer (useful
// for "you picked this up" confirmations, or late-join catch-up where the
// host sends the current world state to a newly connected client only).
//
// Field-level change detection
// ─────────────────────────────
// SetReplicatedField() records a "last sent" value alongside the entity.
// ReplicateDirty() / ReplicateDirtyGroup() compares current vs last-sent
// and skips the network call if nothing changed — safe to call every frame
// from Update() without hammering the wire.
//
// INCLUDE RULES (same as replication_rpc.hpp)
// ────────────────────────────────────────────
// This file only pulls in network.hpp + entity.hpp — no prefab or
// script_system includes — so it is safe to include from
// unity2d_script_api.hpp and therefore from gameplay scripts.
// The listener/handler wiring in replication.hpp's HandleNetworkEvent
// already dispatches "net_replicate_prop" events; this header adds
// the sending side (and the client-side apply) that replication.hpp
// doesn't cover.

#include "network.hpp"
#include "replication_rpc.hpp"
#include "../entity.hpp"
#include "../feature_systems.hpp"

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

namespace Replication {

// Forward declaration — defined in replication.hpp which includes this file.
inline Entity* FindByNetId(EntityList& scene, std::uint32_t net_id);

// ── Mode / flags ─────────────────────────────────────────────────────────────

enum class ReplicateMode : std::uint8_t {
    HostAuth    = 0,  // default: host validates, then broadcasts (safe)
    ClientAuth  = 1,  // skip host round-trip; sender broadcasts directly
};

enum class ReplicateFlags : std::uint8_t {
    None        = 0,
    Unreliable  = 1 << 0,  // UDP-unreliable; fine for high-frequency fields
    SkipSelf    = 1 << 1,  // don't echo back to the sender (client-auth only)
};
inline ReplicateFlags operator|(ReplicateFlags a, ReplicateFlags b) {
    return (ReplicateFlags)((std::uint8_t)a | (std::uint8_t)b);
}
inline bool flag_set(ReplicateFlags flags, ReplicateFlags bit) {
    return (std::uint8_t)flags & (std::uint8_t)bit;
}

// ── Optional host-side guard ──────────────────────────────────────────────────
//
// Install a guard to let the host reject or clamp an incoming client
// ReplicateRequest before broadcasting it. Return false to silently drop.
// Example:
//   InstallReplicateGuard([](std::uint32_t from_peer, std::uint32_t net_id,
//                            const std::string& field, Entity& value) {
//       if (field == "collected" && from_peer != /*item owner*/ 0) return false;
//       return true;
//   });

using ReplicateGuard = std::function<bool(
    std::uint32_t from_peer,
    std::uint32_t net_id,
    const std::string& field,
    Entity& value   // mutable so guard can clamp/sanitise
)>;

inline ReplicateGuard& _replicate_guard() {
    static ReplicateGuard g;
    return g;
}
inline void InstallReplicateGuard(ReplicateGuard guard) {
    _replicate_guard() = std::move(guard);
}

// ── Change-detection cache ────────────────────────────────────────────────────
// net_id -> (field -> last-sent value), used by ReplicateDirty().
// Cleared on Replication::Init() so it doesn't leak across matches.

using PropCache = std::unordered_map<std::uint32_t,
                      std::unordered_map<std::string, Entity>>;

inline PropCache& _prop_cache() { static PropCache c; return c; }
inline void ClearPropCache() { _prop_cache().clear(); }

// ── Core send helpers ─────────────────────────────────────────────────────────

namespace detail {

// Builds and sends a "net_replicate_prop" packet for a single field.
// `to_peer`=0 means broadcast to all (host's own SendEvent path).
inline void _send_prop(std::uint32_t net_id,
                        const std::string& field,
                        const Entity& value,
                        std::uint32_t from_peer,
                        bool reliable,
                        std::uint32_t to_peer = 0) {
    Entity payload = Entity::object();
    payload["net_id"]   = (int)net_id;
    payload["field"]    = field;
    payload["value"]    = value;
    payload["from_peer"]= (int)from_peer;
    if (to_peer != 0) {
        Network::SendEventTo(to_peer, "net_replicate_prop", payload, reliable);
    } else {
        Network::SendEvent("net_replicate_prop", payload, reliable);
    }
}

// Builds and sends a "net_replicate_group" packet for multiple fields.
inline void _send_group(std::uint32_t net_id,
                         const std::vector<std::pair<std::string, Entity>>& fields,
                         std::uint32_t from_peer,
                         bool reliable,
                         std::uint32_t to_peer = 0) {
    Entity payload = Entity::object();
    payload["net_id"]    = (int)net_id;
    payload["from_peer"] = (int)from_peer;
    Entity arr = Entity::array();
    for (const auto& [k, v] : fields) {
        Entity item = Entity::object();
        item["field"] = k;
        item["value"] = v;
        arr.push_back(item);
    }
    payload["fields"] = arr;
    if (to_peer != 0) {
        Network::SendEventTo(to_peer, "net_replicate_group", payload, reliable);
    } else {
        Network::SendEvent("net_replicate_group", payload, reliable);
    }
}

// Apply an incoming prop update to the live scene entity and fire EventBus.
// Called on both host (after validation) and client (on receive).
inline void _apply_prop(EntityList& scene, std::uint32_t net_id,
                         const std::string& field, const Entity& value,
                         std::uint32_t from_peer) {
    Entity* e = FindByNetId(scene, net_id);
    if (!e) return;

    // Write into a dedicated "replicated" sub-object so scripts can read
    // e["replicated"]["collected"] etc. without colliding with engine keys.
    (*e)["replicated"][field] = value;

    // Fire an EventBus event so scripts can react immediately:
    //   on("net_prop_changed", [](Entity& data, Entity* target) { ... });
    Entity ev = Entity::object();
    ev["net_id"]    = (int)net_id;
    ev["field"]     = field;
    ev["value"]     = value;
    ev["from_peer"] = (int)from_peer;
    EventBus::instance().emit("net_prop_changed", ev, e);
}

inline void _apply_group(EntityList& scene, std::uint32_t net_id,
                          const Entity& fields_arr, std::uint32_t from_peer) {
    if (!fields_arr.is_array()) return;
    Entity* e = FindByNetId(scene, net_id);
    if (!e) return;
    for (const auto& item : fields_arr) {
        std::string field = item.value("field", std::string());
        if (field.empty()) continue;
        const Entity& value = item["value"];
        (*e)["replicated"][field] = value;
    }
    Entity ev = Entity::object();
    ev["net_id"]    = (int)net_id;
    ev["from_peer"] = (int)from_peer;
    ev["fields"]    = fields_arr;
    EventBus::instance().emit("net_prop_group_changed", ev, e);
}

} // namespace detail

// ── Public API ────────────────────────────────────────────────────────────────

// Replicate a single property of a networked entity to all peers.
//
//   Replicate(self, "collected", true);
//   Replicate(self, "open", is_open, ReplicateMode::HostAuth, ReplicateFlags::Reliable);
//   Replicate(self, "charge_pct", charge_, ReplicateMode::ClientAuth, ReplicateFlags::Unreliable);
//
// `self` is the Entity& that the script is attached to (must be networked).
// `field` is an arbitrary string key — scripts read it back as:
//         entity["replicated"]["collected"]
// `value` accepts any JSON-serialisable type via Entity's implicit
//         constructors: bool, int, float, std::string, Entity object/array.
inline void Replicate(const Entity& self,
                       const std::string& field,
                       const Entity& value,
                       ReplicateMode mode = ReplicateMode::HostAuth,
                       ReplicateFlags flags = ReplicateFlags::None) {
    std::uint32_t net_id = NetIdOf(self);
    if (net_id == 0) return; // not a networked entity
    bool reliable = !flag_set(flags, ReplicateFlags::Unreliable);
    std::uint32_t local_peer = Network::LocalPeerId();

    if (mode == ReplicateMode::ClientAuth) {
        // Sender broadcasts directly; host + other clients all receive.
        detail::_send_prop(net_id, field, value, local_peer, reliable);
        // Also apply locally so the caller sees the update immediately.
        Entity ev = Entity::object();
        ev["net_id"]    = (int)net_id;
        ev["field"]     = field;
        ev["value"]     = value;
        ev["from_peer"] = (int)local_peer;
        EventBus::instance().emit("net_prop_changed", ev, const_cast<Entity*>(&self));
        return;
    }

    // HostAuth
    if (Network::IsHost()) {
        // Host applies and broadcasts immediately.
        Entity ev_data = Entity::object();
        ev_data["net_id"]    = (int)net_id;
        ev_data["field"]     = field;
        ev_data["value"]     = value;
        ev_data["from_peer"] = (int)local_peer;
        EventBus::instance().emit("net_prop_changed", ev_data, const_cast<Entity*>(&self));
        detail::_send_prop(net_id, field, value, local_peer, reliable);
    } else {
        // Client sends a request to the host for validation.
        Entity payload = Entity::object();
        payload["net_id"]    = (int)net_id;
        payload["field"]     = field;
        payload["value"]     = value;
        payload["from_peer"] = (int)local_peer;
        Network::SendEvent("net_replicate_request", payload, true); // always reliable for requests
    }
}

// ── EntityRef overloads ──────────────────────────────────────────────────
template <class T, decltype(std::declval<T>().operator->(), 0) = 0>
inline void Replicate(T self, const std::string& field, const Entity& value,
                       ReplicateMode mode = ReplicateMode::HostAuth,
                       ReplicateFlags flags = ReplicateFlags::None) {
    if (self) Replicate(*self, field, value, mode, flags);
}

// ReplicateTo — send a property update to one specific peer only.
// Useful for late-join sync or per-player confirmations (e.g. "you got the item").
// Always host-only (only the host has authority to dictate state to a specific peer).
inline void ReplicateTo(std::uint32_t peer_id,
                         const Entity& self,
                         const std::string& field,
                         const Entity& value) {
    if (!Network::IsHost()) return;
    std::uint32_t net_id = NetIdOf(self);
    if (net_id == 0) return;
    detail::_send_prop(net_id, field, value, 0, true, peer_id);
}

// ReplicateGroup — batch multiple field updates into one packet.
//
//   ReplicateGroup(self, {{"phase", phase_int}, {"enrage", true}, {"shield_hp", 40.f}});
//
// Cheaper than N separate Replicate() calls when several fields change together.
inline void ReplicateGroup(const Entity& self,
                             const std::vector<std::pair<std::string, Entity>>& fields,
                             ReplicateMode mode = ReplicateMode::HostAuth,
                             ReplicateFlags flags = ReplicateFlags::None) {
    std::uint32_t net_id = NetIdOf(self);
    if (net_id == 0 || fields.empty()) return;
    bool reliable = !flag_set(flags, ReplicateFlags::Unreliable);
    std::uint32_t local_peer = Network::LocalPeerId();

    if (mode == ReplicateMode::ClientAuth || Network::IsHost()) {
        detail::_send_group(net_id, fields, local_peer, reliable);
    } else {
        Entity payload = Entity::object();
        payload["net_id"]    = (int)net_id;
        payload["from_peer"] = (int)local_peer;
        Entity arr = Entity::array();
        for (const auto& [k, v] : fields) {
            Entity item = Entity::object();
            item["field"] = k;
            item["value"] = v;
            arr.push_back(item);
        }
        payload["fields"] = arr;
        Network::SendEvent("net_replicate_group_request", payload, true);
    }
}

template <class T, decltype(std::declval<T>().operator->(), 0) = 0>
inline void ReplicateGroup(T self, const std::vector<std::pair<std::string, Entity>>& fields,
                            ReplicateMode mode = ReplicateMode::HostAuth,
                            ReplicateFlags flags = ReplicateFlags::None) {
    if (self) ReplicateGroup(*self, fields, mode, flags);
}

// ReplicateGroupTo — batch send to one peer (host-only, for late-join catch-up).
inline void ReplicateGroupTo(std::uint32_t peer_id,
                               const Entity& self,
                               const std::vector<std::pair<std::string, Entity>>& fields) {
    if (!Network::IsHost()) return;
    std::uint32_t net_id = NetIdOf(self);
    if (net_id == 0 || fields.empty()) return;
    detail::_send_group(net_id, fields, 0, true, peer_id);
}

// ── Change-detection API ──────────────────────────────────────────────────────
//
// Call ReplicateDirty() every frame from Update() — it only sends the packet
// when the value has actually changed since the last send.
//
//   // In DoorScript::update():
//   ReplicateDirty(self, "open", is_open_);
//
//   // In BossScript::update():
//   ReplicateDirtyGroup(self, {{"phase", phase_}, {"shield_hp", shield_hp_}});

inline void ReplicateDirty(const Entity& self,
                             const std::string& field,
                             const Entity& value,
                             ReplicateMode mode = ReplicateMode::HostAuth,
                             ReplicateFlags flags = ReplicateFlags::None) {
    std::uint32_t net_id = NetIdOf(self);
    if (net_id == 0) return;
    auto& cache = _prop_cache()[net_id];
    auto it = cache.find(field);
    if (it != cache.end() && !(it->second < value) && !(value < it->second)) return; // no change
    cache[field] = value;
    Replicate(self, field, value, mode, flags);
}

inline void ReplicateDirtyGroup(const Entity& self,
                                  const std::vector<std::pair<std::string, Entity>>& fields,
                                  ReplicateMode mode = ReplicateMode::HostAuth,
                                  ReplicateFlags flags = ReplicateFlags::None) {
    std::uint32_t net_id = NetIdOf(self);
    if (net_id == 0 || fields.empty()) return;
    auto& cache = _prop_cache()[net_id];
    std::vector<std::pair<std::string, Entity>> dirty;
    for (const auto& [k, v] : fields) {
        auto it = cache.find(k);
        if (it == cache.end() || (it->second < v) || (v < it->second)) {
            dirty.emplace_back(k, v);
            cache[k] = v;
        }
    }
    if (!dirty.empty()) ReplicateGroup(self, dirty, mode, flags);
}

template <class T, decltype(std::declval<T>().operator->(), 0) = 0>
inline void ReplicateDirty(T self, const std::string& field, const Entity& value,
                            ReplicateMode mode = ReplicateMode::HostAuth,
                            ReplicateFlags flags = ReplicateFlags::None) {
    if (self) ReplicateDirty(*self, field, value, mode, flags);
}

// ── Late-join full-world sync ─────────────────────────────────────────────────
//
// Call this from the host's "peer_connected" handler AFTER SpawnAllPlayers
// and RegisterExisting have run. It walks every entity in the scene that has
// a "replicated" sub-object and sends its full current state to the new peer,
// so they join a mid-match world accurately rather than seeing only subsequent
// deltas.
//
//   EventBus::on("peer_connected", [&](Entity& data, Entity*) {
//       uint32_t peer = (uint32_t)data.value("peer_id", 0);
//       Replication::SyncLatejoin(scene, peer);
//   });

inline void SyncLatejoin(EntityList& scene, std::uint32_t peer_id) {
    if (!Network::IsHost()) return;
    for (const auto& e : scene) {
        if (!IsNetworked(e)) continue;
        if (!e.contains("replicated") || !e["replicated"].is_object()) continue;
        std::uint32_t net_id = NetIdOf(e);
        std::vector<std::pair<std::string, Entity>> fields;
        for (const auto& [k, v] : e["replicated"].items()) {
            fields.emplace_back(k, v);
        }
        if (!fields.empty()) ReplicateGroupTo(peer_id, e, fields);
    }
}

// ── Network event handler ─────────────────────────────────────────────────────
//
// Wire this into Replication::HandleNetworkEvent in replication.hpp. Already
// done if you've applied the updated replication.hpp from this patch; if you
// prefer to call it manually, add:
//   ReplicationProps::HandleNetworkEvent(scene, ev);
// alongside the existing HandleNetworkEvent call in your game loop.

inline void HandleNetworkEvent(EntityList& scene, const Network::PendingEvent& ev) {

    // ── Single-prop broadcast (host → clients, or ClientAuth sender → all) ──
    if (ev.name == "net_replicate_prop") {
        std::uint32_t net_id = (std::uint32_t)ev.data.value("net_id", 0);
        std::string field    = ev.data.value("field", std::string());
        std::uint32_t from   = (std::uint32_t)ev.data.value("from_peer", 0);
        if (net_id == 0 || field.empty() || !ev.data.contains("value")) return;
        detail::_apply_prop(scene, net_id, field, ev.data["value"], from);
        return;
    }

    // ── Group broadcast ─────────────────────────────────────────────────────
    if (ev.name == "net_replicate_group") {
        std::uint32_t net_id = (std::uint32_t)ev.data.value("net_id", 0);
        std::uint32_t from   = (std::uint32_t)ev.data.value("from_peer", 0);
        if (net_id == 0 || !ev.data.contains("fields")) return;
        detail::_apply_group(scene, net_id, ev.data["fields"], from);
        return;
    }

    // ── Single-prop request (client → host, HostAuth only) ──────────────────
    if (ev.name == "net_replicate_request") {
        if (!Network::IsHost()) return;
        std::uint32_t net_id = (std::uint32_t)ev.data.value("net_id", 0);
        std::string field    = ev.data.value("field", std::string());
        std::uint32_t from   = ev.from_peer;
        if (net_id == 0 || field.empty() || !ev.data.contains("value")) return;
        Entity value = ev.data["value"];

        // Run the guard if installed — lets the project reject bad requests.
        if (_replicate_guard() && !_replicate_guard()(from, net_id, field, value)) return;

        // Apply on host + broadcast to all.
        detail::_apply_prop(scene, net_id, field, value, from);
        detail::_send_prop(net_id, field, value, from, true);
        return;
    }

    // ── Group request (client → host, HostAuth only) ────────────────────────
    if (ev.name == "net_replicate_group_request") {
        if (!Network::IsHost()) return;
        std::uint32_t net_id = (std::uint32_t)ev.data.value("net_id", 0);
        std::uint32_t from   = ev.from_peer;
        if (net_id == 0 || !ev.data.contains("fields")) return;

        // Filter through guard field by field.
        Entity filtered = Entity::array();
        for (const auto& item : ev.data["fields"]) {
            std::string field = item.value("field", std::string());
            if (field.empty() || !item.contains("value")) continue;
            Entity value = item["value"];
            if (_replicate_guard() && !_replicate_guard()(from, net_id, field, value)) continue;
            Entity fitem = Entity::object();
            fitem["field"] = field;
            fitem["value"] = value;
            filtered.push_back(fitem);
        }
        if (filtered.empty()) return;

        detail::_apply_group(scene, net_id, filtered, from);
        // Re-pack into vector form for _send_group.
        std::vector<std::pair<std::string, Entity>> vec;
        for (const auto& item : filtered) {
            vec.emplace_back(item.value("field", std::string()), item["value"]);
        }
        detail::_send_group(net_id, vec, from, true);
        return;
    }
}

} // namespace Replication
