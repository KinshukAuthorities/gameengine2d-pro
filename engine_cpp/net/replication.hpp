#pragma once
// replication.hpp — host-authoritative world replication (extended).
//
// ═══════════════════════════════════════════════════════════════════
// WHAT'S NEW IN THIS REVISION
// ═══════════════════════════════════════════════════════════════════
//
//  ① General property replication  (replication_props.hpp)
//     ─────────────────────────────
//     One-liner to sync any JSON-serialisable field to all peers:
//
//       Replication::Replicate(self, "collected", true);
//       Replication::Replicate(self, "open", is_open_);
//       Replication::ReplicateGroup(self, {{"phase",2},{"enrage",true}});
//
//     Host-authoritative (client sends a request, host validates and
//     broadcasts) by default. Pass ReplicateMode::ClientAuth for fields
//     that are safe to trust from the sender directly (cosmetics, emotes).
//     ReplicateDirty() / ReplicateDirtyGroup() add change-detection so it's
//     safe to call every frame from Update() without spamming the wire.
//     See replication_props.hpp for full API.
//
//  ② Late-join full-world sync
//     ─────────────────────────
//     Replication::SyncLatejoin(scene, peer_id) — call from a
//     "peer_connected" handler on the host. Sends:
//       • A net_spawn for every tracked entity the new peer doesn't have.
//       • All current "replicated" field values (from replication_props.hpp).
//       • Current health for every entity with HealthComponent.
//     The new player sees the world correctly from the first frame.
//
//  ③ Ownership transfer
//     ──────────────────
//     Replication::TransferOwnership(scene, net_id, new_peer_id)
//     Host-only. Changes which peer "owns" a networked entity (e.g. a
//     vehicle a player boards, a carried crate). The new owner's transform
//     then gets the same treatment as a player-owned entity — their local
//     position is trusted, not overwritten by the world-state tick.
//
//  ④ Custom RPC events
//     ─────────────────
//     Replication::FireRPC(self, "event_name", payload)
//     A thin, scene-aware wrapper around Network::SendEvent that:
//       • Guards against calling from non-networked entities.
//       • Automatically attaches the caller's net_id.
//       • Has a FireRPCTo() variant for unicast.
//     Scripts listen via EventBus::on("rpc:event_name", …).
//
//  ⑤ Reliable entity-state snapshot on demand
//     ──────────────────────────────────────────
//     Replication::BroadcastFullState(scene) — host sends the complete
//     current transform+health of every tracked entity in one reliable
//     packet ("net_full_state"). Useful after a brief disconnect/reconnect
//     or after a host-side bulk event (explosion that moves many objects).
//
//  ⑥ Tag-based interest filtering
//     ────────────────────────────
//     Replication::SetInterestTag(entity, "zone_a")
//     SetPeerInterest(peer_id, {"zone_a", "zone_b"})
//     The per-tick world-state Tick() skips entities whose interest tags
//     don't overlap with the receiving peer's subscribed set. Dramatically
//     reduces bandwidth in large levels (only nearby/visible entities are
//     sent to each peer). All events (spawn/despawn/health/props) still go
//     to everyone; only the high-frequency transform tick is filtered.
//
//  ⑦ Replicated events (fire-and-forget, no entity attachment)
//     ─────────────────────────────────────────────────────────
//     Replication::FireWorldEvent(name, payload)   — host-only broadcast
//     Replication::FireWorldEventTo(peer_id, name, payload) — unicast
//     For world-level signals that don't belong to any entity: round-start,
//     checkpoint reached, boss phase change, score update, etc.
//
// ─── ORIGINAL DESIGN PRESERVED ───────────────────────────────────────────────
// Everything in the original replication.hpp (Spawn, Despawn, RequestDamage,
// RequestPickup, Tick, HandleNetworkEvent, RegisterExisting) is unchanged
// in semantics. The new features are strictly additive.
//
// ─── INCLUDE RULES ───────────────────────────────────────────────────────────
// Same as before: include at the top level (core.cpp / panels.hpp).
// Gameplay scripts should include replication_rpc.hpp (already via
// unity2d_script_api.hpp) for NetIdOf/RequestDamage/RequestPickup, and
// replication_props.hpp for Replicate/ReplicateDirty.

#include "network.hpp"
#include "replication_rpc.hpp"
#include "replication_props.hpp"       // ← new: general property replication
#include "../entity.hpp"
#include "../transform_system.hpp"
#include "../prefab_system.hpp"
#include "../script_system.hpp"        // Debug
#include "../feature_systems.hpp"      // EventBus
#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>

namespace Replication {

// ─────────────────────────────────────────────────────────────────────────────
// NetId allocation (host-only) — unchanged from original
// ─────────────────────────────────────────────────────────────────────────────

inline std::uint32_t& _next_id() { static std::uint32_t n = 1; return n; }
inline std::uint32_t AllocateNetId() { return _next_id()++; }
inline void ResetIdAllocator() { _next_id() = 1; }

// ─────────────────────────────────────────────────────────────────────────────
// TrackedEntity & State — extended with ownership + interest tags
// ─────────────────────────────────────────────────────────────────────────────

struct TrackedEntity {
    std::uint32_t net_id             = 0;
    std::string   prefab_path;
    bool          replicate_transform = true;
    std::uint32_t owner_peer_id       = 0;  // 0 = host-simulated, >0 = peer-owned
    std::string   interest_tag;             // optional zone/area tag for filtering
};

// Per-peer interest subscription (set of interest tags that peer cares about).
using InterestSet = std::unordered_set<std::string>;

struct State {
    bool active         = false;
    std::unordered_map<std::uint32_t, TrackedEntity> tracked;   // net_id → info
    std::unordered_map<std::uint32_t, InterestSet>   peer_interest; // peer_id → tags
    float tick_accum    = 0.f;
    float tick_interval = 1.f / 20.f; // 20 world-state updates/sec

    // O(1) net_id → Entity* lookup. Populated by Spawn/RegisterExisting,
    // cleared by Despawn, and rebuilt on demand by RebuildNetIndex().
    // All code must go through FindByNetId() — never read this directly.
    std::unordered_map<std::uint32_t, Entity*> net_index;
};

inline State& _state() { static State s; return s; }

// Rebuild the O(1) net_id → Entity* index from scratch.
// Called automatically by FindByNetId on a cache miss, and manually after
// bulk scene mutations (scene load, late-join spawn flood, etc.).
inline void RebuildNetIndex(EntityList& scene) {
    auto& idx = _state().net_index;
    idx.clear();
    for (auto& e : scene) {
        std::uint32_t nid = (std::uint32_t)e.value("net_id", 0);
        if (nid != 0) idx[nid] = &e;
    }
}

// O(1) lookup via the index.  Falls back to a linear rebuild if the index is
// empty (first call, or after a scene reload where Init() wiped it).
inline Entity* FindByNetId(EntityList& scene, std::uint32_t net_id) {
    if (net_id == 0) return nullptr;
    auto& idx = _state().net_index;
    if (idx.empty() && !scene.empty()) RebuildNetIndex(scene);
    auto it = idx.find(net_id);
    return (it != idx.end()) ? it->second : nullptr;
}

// ─── forward declarations ────────────────────────────────────────────────────
inline void _apply_damage_request(const Network::PendingEvent& ev);
inline void _apply_pickup_request(EntityList& scene, const Network::PendingEvent& ev);

namespace detail {
    inline bool _install_handlers_once() {
        InstallHostHandlers(
            [](const Network::PendingEvent& ev) { _apply_damage_request(ev); },
            [](const Network::PendingEvent& ev) {
                if (Network::_state().scene) _apply_pickup_request(*Network::_state().scene, ev);
            }
        );
        return true;
    }
    inline bool _handlers_installed = _install_handlers_once();
} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Init / Shutdown / SetTickRate — extended to clear prop cache
// ─────────────────────────────────────────────────────────────────────────────

inline void Init() {
    auto& s = _state();
    s.tracked.clear();
    s.peer_interest.clear();
    s.net_index.clear();       // ← reset O(1) lookup index
    s.tick_accum = 0.f;
    s.active = true;
    if (Network::IsHost() || !Network::IsClient()) ResetIdAllocator();
    ClearPropCache();          // ← clear change-detection cache between matches
}

inline void Shutdown() {
    auto& s = _state();
    s.tracked.clear();
    s.peer_interest.clear();
    s.net_index.clear();       // ← reset O(1) lookup index
    s.active = false;
    ClearPropCache();
}

inline void SetTickRate(float updates_per_second) {
    if (updates_per_second <= 0.f) return;
    _state().tick_interval = 1.f / updates_per_second;
}

// ─────────────────────────────────────────────────────────────────────────────
// ⑥ Tag-based interest management (NEW)
// ─────────────────────────────────────────────────────────────────────────────

// Tag an entity so that high-frequency transform ticks are only sent to peers
// subscribed to that tag. Pass an empty string to reset (always replicate).
inline void SetInterestTag(Entity& e, const std::string& tag) {
    std::uint32_t net_id = NetIdOf(e);
    if (net_id == 0) return;
    auto it = _state().tracked.find(net_id);
    if (it != _state().tracked.end()) it->second.interest_tag = tag;
}

// Tell the system which interest tags a given peer cares about.
// Called on the host, typically after the peer connects/changes area.
// Passing an empty set means "send me everything" (default behavior).
inline void SetPeerInterest(std::uint32_t peer_id, const InterestSet& tags) {
    _state().peer_interest[peer_id] = tags;
}

// Returns true if the tracked entity should be ticked to a given peer.
inline bool _peer_interested(std::uint32_t peer_id, const TrackedEntity& te) {
    if (te.interest_tag.empty()) return true; // untagged → everyone sees it
    auto& ps = _state().peer_interest;
    auto it = ps.find(peer_id);
    if (it == ps.end() || it->second.empty()) return true; // peer has no filter → sees all
    return it->second.count(te.interest_tag) > 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Spawn — unchanged in semantics, extended with owner_peer_id tracking
// ─────────────────────────────────────────────────────────────────────────────

inline Entity* Spawn(EntityList& scene, const std::string& asset_dir,
                      const std::string& prefab_path, float x, float y,
                      const Entity& init_state = Entity::object(),
                      bool replicate_transform = true,
                      std::uint32_t owner_peer_id = 0 /*0=host*/) {
    if (!Network::IsHost()) {
        Debug::log_warning("Replication::Spawn called on a non-host machine; ignored.");
        return nullptr;
    }
    int local_id = prefab::instantiate(prefab_path, scene, asset_dir, x, y);
    if (local_id < 0) return nullptr;
    Entity* e = nullptr;
    for (auto& ent : scene) if (ent.value("id", 0) == local_id) { e = &ent; break; }
    if (!e) return nullptr;

    std::uint32_t net_id = AllocateNetId();
    (*e)["net_id"] = (int)net_id;
    if (owner_peer_id != 0) (*e)["net_owner_peer_id"] = (int)owner_peer_id;
    if (!init_state.is_null() && init_state.is_object() && !init_state.empty())
        (*e)["net_init_state"] = init_state;

    auto& s = _state();
    s.tracked[net_id] = TrackedEntity{net_id, prefab_path, replicate_transform, owner_peer_id};
    s.net_index[net_id] = e;   // ← keep O(1) index in sync

    Entity payload = Entity::object();
    payload["net_id"]        = (int)net_id;
    payload["prefab_path"]   = prefab_path;
    payload["x"]             = x;
    payload["y"]             = y;
    payload["owner_peer_id"] = (int)owner_peer_id;
    if (!init_state.is_null() && init_state.is_object()) payload["init_state"] = init_state;
    Network::SendEvent("net_spawn", payload, true);

    // Mark the transform hierarchy dirty so TransformSystem::update() picks
    // up the new entity next frame. Calling rebuild_registry() here was a
    // full O(N) rebuild every time Spawn() was called — now deferred.
    transform::mark_structure_dirty();
    return e;
}

inline Entity* Spawn(EntityList* scene, const std::string& asset_dir,
                     const std::string& prefab_path, float x, float y,
                     const Entity& init_state = Entity::object(),
                     bool replicate_transform = true,
                     std::uint32_t owner_peer_id = 0) {
    return scene ? Spawn(*scene, asset_dir, prefab_path, x, y, init_state, replicate_transform, owner_peer_id) : nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Despawn — unchanged
// ─────────────────────────────────────────────────────────────────────────────

inline void Despawn(EntityList& scene, std::uint32_t net_id, const std::string& reason = "") {
    if (!Network::IsHost()) {
        Debug::log_warning("Replication::Despawn called on a non-host machine; ignored.");
        return;
    }
    if (Entity* e = FindByNetId(scene, net_id)) (*e)["_destroyed"] = true;
    _state().tracked.erase(net_id);
    _state().net_index.erase(net_id); // ← keep O(1) index in sync
    _prop_cache().erase(net_id);   // clean up change-detection cache

    Entity payload = Entity::object();
    payload["net_id"] = (int)net_id;
    if (!reason.empty()) payload["reason"] = reason;
    Network::SendEvent("net_despawn", payload, true);
}

inline void Despawn(EntityList* scene, std::uint32_t net_id, const std::string& reason = "") {
    if (scene) Despawn(*scene, net_id, reason);
}

// ─────────────────────────────────────────────────────────────────────────────
// ③ Ownership transfer (NEW)
// ─────────────────────────────────────────────────────────────────────────────
//
// Changes which peer "owns" a tracked entity. The new owner's client-side
// position for that entity is trusted (same as a player entity), so the host
// stops overwriting it from the world-state tick. Broadcasts "net_ownership"
// so all clients update their local records.
//
// Example use: player boards a vehicle, picks up a crate, catches a ball.

inline void TransferOwnership(EntityList& scene, std::uint32_t net_id,
                               std::uint32_t new_owner_peer_id) {
    if (!Network::IsHost()) return;
    auto& s = _state();
    auto it = s.tracked.find(net_id);
    if (it == s.tracked.end()) {
        Debug::log_warning("Replication::TransferOwnership: net_id not tracked.");
        return;
    }
    it->second.owner_peer_id = new_owner_peer_id;

    Entity* e = FindByNetId(scene, net_id);
    if (e) {
        if (new_owner_peer_id != 0) (*e)["net_owner_peer_id"] = (int)new_owner_peer_id;
        else e->erase("net_owner_peer_id");
    }

    Entity payload = Entity::object();
    payload["net_id"]          = (int)net_id;
    payload["new_owner_peer_id"] = (int)new_owner_peer_id;
    Network::SendEvent("net_ownership", payload, true);
}

// ─────────────────────────────────────────────────────────────────────────────
// RequestDamageInRadius — unchanged
// ─────────────────────────────────────────────────────────────────────────────

inline void RequestDamageInRadius(EntityList& scene, float x, float y, float radius,
                                   float amount, std::uint32_t attacker_peer_id) {
    if (!Network::IsHost()) return;
    for (auto& e : scene) {
        if (!has_component(e, "HealthComponent")) continue;
        if (!IsNetworked(e)) continue;
        float ex = transform::world_x(e), ey = transform::world_y(e);
        float dx = ex - x, dy = ey - y;
        if (dx * dx + dy * dy <= radius * radius)
            RequestDamage(NetIdOf(e), amount, attacker_peer_id, x, y);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ④ Custom entity RPC (NEW)
// ─────────────────────────────────────────────────────────────────────────────
//
// Fire a named event from a networked entity to all peers (or one peer).
// Scripts listen via EventBus::on("rpc:<event_name>", …).
//
//   // In a TrapScript that triggers for everyone:
//   Replication::FireRPC(self, "triggered", {{"dmg", 20}});
//
//   // Private message to one player:
//   Replication::FireRPCTo(Network::LocalPeerId(), self, "quest_update", payload);

inline void FireRPC(const Entity& self,
                     const std::string& event_name,
                     const Entity& payload = Entity::object(),
                     bool reliable = true) {
    std::uint32_t net_id = NetIdOf(self);
    if (net_id == 0) return;
    Entity data = payload;
    data["_rpc_net_id"]   = (int)net_id;
    data["_rpc_event"]    = event_name;
    data["_rpc_peer"]     = (int)Network::LocalPeerId();
    if (Network::IsHost()) {
        Network::SendEvent("net_rpc:" + event_name, data, reliable);
        EventBus::instance().emit("rpc:" + event_name, data, const_cast<Entity*>(&self));
    } else {
        Network::SendEvent("net_rpc_request:" + event_name, data, reliable);
    }
}

inline void FireRPC(EntityRef self, const std::string& event_name,
                     const Entity& payload = Entity::object(),
                     bool reliable = true) {
    if (self) FireRPC(*self, event_name, payload, reliable);
}

inline void FireRPCTo(std::uint32_t peer_id,
                       const Entity& self,
                       const std::string& event_name,
                       const Entity& payload = Entity::object()) {
    if (!Network::IsHost()) return; // unicast is host-only authority
    std::uint32_t net_id = NetIdOf(self);
    if (net_id == 0) return;
    Entity data = payload;
    data["_rpc_net_id"] = (int)net_id;
    data["_rpc_event"]  = event_name;
    data["_rpc_peer"]   = 0;
    Network::SendEventTo(peer_id, "net_rpc:" + event_name, data, true);
}

// ─────────────────────────────────────────────────────────────────────────────
// ⑦ World-level events (NEW) — no entity attachment
// ─────────────────────────────────────────────────────────────────────────────
//
//   Replication::FireWorldEvent("round_start",  {{"countdown_sec", 3}});
//   Replication::FireWorldEvent("boss_defeated", {{"boss_id", 5}});
//
// Listeners: EventBus::on("world:round_start", …)

inline void FireWorldEvent(const std::string& name,
                            const Entity& payload = Entity::object(),
                            bool reliable = true) {
    if (!Network::IsHost()) {
        Debug::log_warning("FireWorldEvent: must be called on host.");
        return;
    }
    Entity data = payload;
    data["_world_event"] = name;
    Network::SendEvent("net_world_event:" + name, data, reliable);
    EventBus::instance().emit("world:" + name, data, nullptr);
}

inline void FireWorldEventTo(std::uint32_t peer_id,
                               const std::string& name,
                               const Entity& payload = Entity::object()) {
    if (!Network::IsHost()) return;
    Entity data = payload;
    data["_world_event"] = name;
    Network::SendEventTo(peer_id, "net_world_event:" + name, data, true);
}

// ─────────────────────────────────────────────────────────────────────────────
// Host-side request handlers — unchanged core, extended for RPC forwarding
// ─────────────────────────────────────────────────────────────────────────────

inline void _apply_damage_request(const Network::PendingEvent& ev) {
    if (!Network::IsHost()) return;
    auto& s = Network::_state();
    if (!s.scene) return;

    std::uint32_t target_id = (std::uint32_t)ev.data.value("target_net_id", 0);
    float amount             = ev.data.value("amount", 0.f);
    std::uint32_t attacker   = (std::uint32_t)ev.data.value("attacker_peer_id", 0);
    float hit_x              = ev.data.value("hit_x", 0.f);
    float hit_y              = ev.data.value("hit_y", 0.f);
    if (target_id == 0 || amount <= 0.f) return;

    Entity* target = FindByNetId(*s.scene, target_id);
    if (!target) return;

    // Support both entities with a HealthComponent and those that store
    // health as a plain top-level "hp" field (e.g. game5 enemies).
    float cur = 0.f, max_health = 100.f;
    bool use_health_comp = has_component(*target, "HealthComponent");
    if (use_health_comp) {
        auto& h = (*target)["components"]["HealthComponent"];
        if (h.value("invincible", false)) return;
        cur        = h.value("current_health", 0.f);
        max_health = h.value("max_health", 100.f);
    } else if (target->contains("hp")) {
        cur        = (float)target->value("hp", 0);
        max_health = cur > 0 ? cur : 1.f;  // treat starting hp as max for display
        // Read a persisted max if available
        if (target->contains("hp_max")) max_health = (float)target->value("hp_max", max_health);
    } else {
        return;  // no health data at all
    }
    if (cur <= 0.f) return;

    float new_health = std::max(0.f, cur - amount);
    bool died = new_health <= 0.f;
    if (use_health_comp) {
        auto& h = (*target)["components"]["HealthComponent"];
        h["current_health"] = new_health;
    } else {
        (*target)["hp"] = (int)new_health;
    }

    Entity payload = Entity::object();
    payload["target_net_id"]  = (int)target_id;
    payload["current_health"] = new_health;
    payload["max_health"]     = max_health;
    payload["amount"]         = amount;
    payload["attacker_peer_id"] = (int)attacker;
    payload["hit_x"]          = hit_x;
    payload["hit_y"]          = hit_y;
    payload["died"]           = died;
    Network::SendEvent("net_health", payload, true);
    EventBus::instance().emit("net_health", payload, target);

    if (died) Despawn(*s.scene, target_id, "killed");
}

inline void _apply_pickup_request(EntityList& scene, const Network::PendingEvent& ev) {
    if (!Network::IsHost()) return;
    std::uint32_t item_id   = (std::uint32_t)ev.data.value("item_net_id", 0);
    std::uint32_t requester = (std::uint32_t)ev.data.value("requester_peer_id", 0);
    if (item_id == 0) return;

    Entity* item = FindByNetId(scene, item_id);
    if (!item) return;

    Entity payload = Entity::object();
    payload["item_net_id"]      = (int)item_id;
    payload["requester_peer_id"]= (int)requester;
    payload["item_prefab"]      = item->value("prefab_source", std::string());
    if (item->contains("net_pickup_payload")) payload["payload"] = (*item)["net_pickup_payload"];

    Network::SendEvent("net_item_collected", payload, true);
    EventBus::instance().emit("net_item_collected", payload, item);
    Despawn(scene, item_id, "collected");
}

// ─────────────────────────────────────────────────────────────────────────────
// ⑤ Reliable full-state broadcast (NEW)
// ─────────────────────────────────────────────────────────────────────────────
//
// Sends every tracked entity's transform + health in one reliable packet.
// Use after bulk events (explosion, teleport, etc.) or when a newly-joined
// peer needs a guaranteed accurate snapshot right now rather than waiting
// for the next unreliable tick.

inline void BroadcastFullState(EntityList& scene, std::uint32_t to_peer = 0) {
    if (!Network::IsHost()) return;
    auto& s = _state();

    Entity payload = Entity::object();
    Entity list    = Entity::array();

    for (const auto& [net_id, info] : s.tracked) {
        Entity* e = FindByNetId(scene, net_id);
        if (!e) continue;
        Entity row = Entity::object();
        row["net_id"] = (int)net_id;
        if (has_component(*e, "Transform")) {
            auto& t = (*e)["components"]["Transform"];
            row["x"]        = transform::world_x(*e);
            row["y"]        = transform::world_y(*e);
            row["rotation"] = t.value("rotation", 0.f);
            row["vx"]       = t.value("velocity_x", 0.f);
            row["vy"]       = t.value("velocity_y", 0.f);
        }
        if (has_component(*e, "HealthComponent")) {
            auto& h = (*e)["components"]["HealthComponent"];
            row["current_health"] = h.value("current_health", 0.f);
            row["max_health"]     = h.value("max_health", 100.f);
        }
        // Include current replicated props for late-joiners.
        if (e->contains("replicated")) row["replicated"] = (*e)["replicated"];
        list.push_back(row);
    }

    payload["entities"]       = list;
    payload["server_time_ms"] = (double)net::detail::now_ms();

    if (to_peer != 0)
        Network::SendEventTo(to_peer, "net_full_state", payload, true);
    else
        Network::SendEvent("net_full_state", payload, true);
}

// ─────────────────────────────────────────────────────────────────────────────
// ② Late-join world sync (NEW, extended from replication_props.hpp's helper)
// ─────────────────────────────────────────────────────────────────────────────
//
// Call this from the host's "peer_connected" handler after the match is live.
// Sends all net_spawn events + full state to the new peer so they join
// mid-match accurately. Complements NetSpawn::SpawnAllPlayers which handles
// the player entities separately.
//
// Usage:
//   EventBus::on("peer_connected", [&](Entity& data, Entity*) {
//       uint32_t peer = (uint32_t)data.value("peer_id", 0);
//       Replication::FullSyncPeer(scene, asset_dir, peer);
//   });

inline void FullSyncPeer(EntityList& scene,
                          const std::string& /*asset_dir*/,
                          std::uint32_t peer_id) {
    if (!Network::IsHost()) return;
    auto& s = _state();

    // 1. Re-send net_spawn for every currently tracked entity.
    for (const auto& [net_id, info] : s.tracked) {
        Entity* e = FindByNetId(scene, net_id);
        if (!e) continue;
        Entity payload = Entity::object();
        payload["net_id"]          = (int)net_id;
        payload["prefab_path"]     = info.prefab_path;
        payload["x"]               = transform::world_x(*e);
        payload["y"]               = transform::world_y(*e);
        payload["owner_peer_id"]   = (int)info.owner_peer_id;
        payload["preplaced"]       = true; // tells receiver not to fire full awake setup
        if (e->contains("net_init_state")) payload["init_state"] = (*e)["net_init_state"];
        Network::SendEventTo(peer_id, "net_spawn", payload, true);
    }

    // 2. Send full state (transforms + health + replicated props) reliably.
    BroadcastFullState(scene, peer_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-tick world-state broadcast — extended with interest filtering
// ─────────────────────────────────────────────────────────────────────────────

inline void Tick(EntityList& scene, float dt) {
    if (!Network::IsHost()) return;
    auto& s = _state();
    if (!s.active) return;
    s.tick_accum += dt;
    if (s.tick_accum < s.tick_interval) return;
    s.tick_accum = 0.f;
    if (s.tracked.empty()) return;

    // Build one list per peer, filtered by interest tag.
    // If no peer has interest filters, fall back to the old broadcast path
    // (single packet to everyone) to avoid per-peer packet construction cost.
    bool any_filter = !s.peer_interest.empty();

    auto build_list = [&](std::uint32_t peer_id) -> Entity {
        Entity list = Entity::array();
        for (const auto& [net_id, info] : s.tracked) {
            if (!info.replicate_transform) continue;
            // Skip if the entity's transform is driven by its owning peer
            // (that peer's own BroadcastTransform already does the job and
            // is lower-latency; sending the world-tick on top would cause
            // the host to overwrite the owner's prediction with a stale copy).
            if (info.owner_peer_id != 0) continue;
            if (!_peer_interested(peer_id, info)) continue;
            Entity* e = FindByNetId(scene, net_id);
            if (!e || !has_component(*e, "Transform")) continue;
            auto& t = (*e)["components"]["Transform"];
            Entity row = Entity::object();
            row["net_id"]   = (int)net_id;
            row["x"]        = transform::world_x(*e);
            row["y"]        = transform::world_y(*e);
            row["rotation"] = t.value("rotation", 0.f);
            row["vx"]       = t.value("velocity_x", 0.f);
            row["vy"]       = t.value("velocity_y", 0.f);
            list.push_back(row);
        }
        return list;
    };

    double srv_time = (double)net::detail::now_ms();

    if (!any_filter) {
        // Fast path: single packet to everyone.
        Entity list = build_list(0 /*unused, no filter*/);
        if (list.empty()) return;
        Entity payload = Entity::object();
        payload["entities"]       = list;
        payload["server_time_ms"] = srv_time;
        Network::SendEvent("net_world_state", payload, false);
    } else {
        // Per-peer packets.
        for (const auto& [peer_id, _] : Network::_state().peers) {
            Entity list = build_list(peer_id);
            if (list.empty()) continue;
            Entity payload = Entity::object();
            payload["entities"]       = list;
            payload["server_time_ms"] = srv_time;
            Network::SendEventTo(peer_id, "net_world_state", payload, false);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// HandleNetworkEvent — original cases preserved + new ones added
// ─────────────────────────────────────────────────────────────────────────────

inline void HandleNetworkEvent(EntityList& scene, const std::string& asset_dir,
                                const Network::PendingEvent& ev) {
    auto& s = _state();

    // ── Existing: net_spawn ──────────────────────────────────────────────────
    if (ev.name == "net_spawn") {
        if (Network::IsHost()) return;
        std::uint32_t net_id = (std::uint32_t)ev.data.value("net_id", 0);
        if (net_id == 0 || FindByNetId(scene, net_id)) return;
        std::string prefab_path = ev.data.value("prefab_path", std::string());
        float x = ev.data.value("x", 0.f), y = ev.data.value("y", 0.f);
        bool preplaced = ev.data.value("preplaced", false);

        // For preplaced entities (registered via RegisterExisting on the host),
        // the entity already exists in the client's scene — just stamp the
        // net_id onto the closest unregistered entity with a matching prefab
        // source near the spawn position. Do NOT instantiate a new copy.
        if (preplaced) {
            Entity* best = nullptr;
            float best_dist = 1e9f;
            for (auto& e : scene) {
                if (IsNetworked(e)) continue;  // already has a net_id
                std::string src = e.value("prefab_source", std::string());
                if (!prefab_path.empty() && !src.empty() && src != prefab_path) continue;
                float ex = transform::world_x(e);
                float ey = transform::world_y(e);
                float dist = std::abs(ex - x) + std::abs(ey - y);
                if (dist < best_dist) { best_dist = dist; best = &e; }
            }
            if (best) {
                (*best)["net_id"] = (int)net_id;
                std::uint32_t owner = (std::uint32_t)ev.data.value("owner_peer_id", 0);
                if (owner != 0) (*best)["net_owner_peer_id"] = (int)owner;
                s.net_index[net_id] = best;  // ← keep O(1) index in sync
            } else {
                Debug::log_warning("Replication: net_spawn preplaced could not find matching entity for '" + prefab_path + "'");
            }
            transform::mark_structure_dirty();
            return;
        }

        int local_id = prefab::instantiate(prefab_path, scene, asset_dir, x, y);
        if (local_id < 0) {
            Debug::log_warning("Replication: net_spawn could not instantiate '" + prefab_path + "'");
            return;
        }
        for (auto& e : scene) {
            if (e.value("id", 0) != local_id) continue;
            e["net_id"] = (int)net_id;
            std::uint32_t owner = (std::uint32_t)ev.data.value("owner_peer_id", 0);
            if (owner != 0) e["net_owner_peer_id"] = (int)owner;
            if (ev.data.contains("init_state")) e["net_init_state"] = ev.data["init_state"];
            s.net_index[net_id] = &e;  // ← keep O(1) index in sync
            break;
        }
        transform::mark_structure_dirty();
        return;
    }

    // ── Existing: net_despawn ────────────────────────────────────────────────
    if (ev.name == "net_despawn") {
        std::uint32_t net_id = (std::uint32_t)ev.data.value("net_id", 0);
        if (Entity* e = FindByNetId(scene, net_id)) (*e)["_destroyed"] = true;
        s.tracked.erase(net_id);
        s.net_index.erase(net_id);  // ← keep O(1) index in sync
        _prop_cache().erase(net_id);
        return;
    }

    // ── Existing: net_health ─────────────────────────────────────────────────
    if (ev.name == "net_health") {
        if (Network::IsHost()) return;
        std::uint32_t net_id = (std::uint32_t)ev.data.value("target_net_id", 0);
        Entity* e = FindByNetId(scene, net_id);
        if (!e) return;
        // Update whichever health representation the entity uses.
        if (has_component(*e, "HealthComponent")) {
            auto& h = (*e)["components"]["HealthComponent"];
            h["current_health"] = ev.data.value("current_health", h.value("current_health", 0.f));
            h["max_health"]     = ev.data.value("max_health",     h.value("max_health", 100.f));
        } else if (e->contains("hp")) {
            (*e)["hp"] = (int)ev.data.value("current_health", (float)e->value("hp", 0));
        }
        EventBus::instance().emit("net_health", ev.data, e);
        return;
    }

    // ── Existing: net_item_collected ─────────────────────────────────────────
    if (ev.name == "net_item_collected") {
        EventBus::instance().emit("net_item_collected", ev.data, nullptr);
        return;
    }

    // ── Existing: net_world_state ────────────────────────────────────────────
    if (ev.name == "net_world_state") {
        if (Network::IsHost()) return;
        if (!ev.data.contains("entities") || !ev.data["entities"].is_array()) return;
        for (const auto& row : ev.data["entities"]) {
            std::uint32_t net_id = (std::uint32_t)row.value("net_id", 0);
            Entity* e = FindByNetId(scene, net_id);
            if (!e || !has_component(*e, "Transform")) continue;
            auto& t = (*e)["components"]["Transform"];
            t["x"]        = row.value("x",        t.value("x",        0.f));
            t["y"]        = row.value("y",        t.value("y",        0.f));
            t["rotation"] = row.value("rotation", t.value("rotation", 0.f));
            if (t.contains("velocity_x")) t["velocity_x"] = row.value("vx", 0.f);
            if (t.contains("velocity_y")) t["velocity_y"] = row.value("vy", 0.f);
        }
        transform::mark_structure_dirty();
        return;
    }

    // ── Existing: host-side RPC requests ────────────────────────────────────
    if (ev.name == "net_damage_request")  { _apply_damage_request(ev); return; }
    if (ev.name == "net_pickup_request")  { _apply_pickup_request(scene, ev); return; }

    // ── NEW: net_full_state (⑤ reliable full-state broadcast) ──────────────
    if (ev.name == "net_full_state") {
        if (Network::IsHost()) return;
        if (!ev.data.contains("entities")) return;
        for (const auto& row : ev.data["entities"]) {
            std::uint32_t net_id = (std::uint32_t)row.value("net_id", 0);
            Entity* e = FindByNetId(scene, net_id);
            if (!e) continue;
            if (has_component(*e, "Transform") && row.contains("x")) {
                auto& t = (*e)["components"]["Transform"];
                t["x"]        = row.value("x", t.value("x", 0.f));
                t["y"]        = row.value("y", t.value("y", 0.f));
                t["rotation"] = row.value("rotation", t.value("rotation", 0.f));
            }
            if (has_component(*e, "HealthComponent") && row.contains("current_health")) {
                auto& h = (*e)["components"]["HealthComponent"];
                h["current_health"] = row.value("current_health", h.value("current_health", 0.f));
                h["max_health"]     = row.value("max_health",     h.value("max_health", 100.f));
            }
            if (row.contains("replicated") && row["replicated"].is_object()) {
                (*e)["replicated"] = row["replicated"];
                // Fire prop-changed for each so scripts get their callbacks.
                for (const auto& [k, v] : row["replicated"].items()) {
                    Entity ev_data = Entity::object();
                    ev_data["net_id"]    = (int)net_id;
                    ev_data["field"]     = k;
                    ev_data["value"]     = v;
                    ev_data["from_peer"] = 0;
                    EventBus::instance().emit("net_prop_changed", ev_data, e);
                }
            }
        }
        transform::mark_structure_dirty();
        return;
    }

    // ── NEW: net_ownership (③ ownership transfer) ────────────────────────────
    if (ev.name == "net_ownership") {
        std::uint32_t net_id = (std::uint32_t)ev.data.value("net_id", 0);
        std::uint32_t new_owner = (std::uint32_t)ev.data.value("new_owner_peer_id", 0);
        Entity* e = FindByNetId(scene, net_id);
        if (e) {
            if (new_owner != 0) (*e)["net_owner_peer_id"] = (int)new_owner;
            else e->erase("net_owner_peer_id");
        }
        // Update tracked record on host too (already done in TransferOwnership;
        // this branch fires on host only if somehow the event loops back).
        auto it = s.tracked.find(net_id);
        if (it != s.tracked.end()) it->second.owner_peer_id = new_owner;
        EventBus::instance().emit("net_ownership", ev.data, e);
        return;
    }

    // ── NEW: net_rpc: prefix (④ entity RPC, host relays) ────────────────────
    // The host receives "net_rpc_request:<name>" from a client, validates the
    // sender is the entity's owner (or skips validation for global RPCs), then
    // re-broadcasts as "net_rpc:<name>" to everyone including emitting locally.
    if (ev.name.rfind("net_rpc_request:", 0) == 0) {
        if (!Network::IsHost()) return;
        std::string rpc_name = ev.name.substr(std::string("net_rpc_request:").size());
        std::uint32_t net_id = (std::uint32_t)ev.data.value("_rpc_net_id", 0);

        // Optional: verify sender owns this entity.
        auto it = s.tracked.find(net_id);
        bool owner_check_pass = true;
        if (it != s.tracked.end() && it->second.owner_peer_id != 0) {
            owner_check_pass = (it->second.owner_peer_id == ev.from_peer);
        }
        if (!owner_check_pass) return; // reject spoofed RPC

        Entity* e = (net_id != 0) ? FindByNetId(scene, net_id) : nullptr;
        Network::SendEvent("net_rpc:" + rpc_name, ev.data, true);
        EventBus::instance().emit("rpc:" + rpc_name, const_cast<Entity&>(ev.data), e);
        return;
    }

    if (ev.name.rfind("net_rpc:", 0) == 0) {
        // Client receives a relayed RPC.
        if (Network::IsHost()) return; // host handled it above already
        std::string rpc_name = ev.name.substr(std::string("net_rpc:").size());
        std::uint32_t net_id = (std::uint32_t)ev.data.value("_rpc_net_id", 0);
        Entity* e = (net_id != 0) ? FindByNetId(scene, net_id) : nullptr;
        EventBus::instance().emit("rpc:" + rpc_name, const_cast<Entity&>(ev.data), e);
        return;
    }

    // ── NEW: net_world_event: prefix (⑦ world-level events) ─────────────────
    if (ev.name.rfind("net_world_event:", 0) == 0) {
        if (Network::IsHost()) return; // host fired it and emitted locally already
        std::string wname = ev.name.substr(std::string("net_world_event:").size());
        EventBus::instance().emit("world:" + wname, const_cast<Entity&>(ev.data), nullptr);
        return;
    }

    // ── NEW: property replication events ─────────────────────────────────────
    // net_replicate_prop / net_replicate_group and their _request variants
    // are handled by the Props subsystem (replication_props.hpp).
    // It's in the same Replication namespace; forward via the free function
    // that replication_props.hpp defines as HandleNetworkEvent — the compiler
    // picks the correct overload because this file's HandleNetworkEvent has a
    // different signature (it takes asset_dir too). Both are in scope here.
    HandleNetworkEvent(scene, ev); // calls replication_props.hpp's version
                                   // (no asset_dir arg → different overload)
}

// ─────────────────────────────────────────────────────────────────────────────
// RegisterExisting — unchanged
// ─────────────────────────────────────────────────────────────────────────────

inline void RegisterExisting(Entity& e, bool replicate_transform = true,
                              std::uint32_t owner_peer_id = 0) {
    if (!Network::IsHost()) return;
    if (IsNetworked(e)) return;
    std::uint32_t net_id = AllocateNetId();
    e["net_id"] = (int)net_id;
    if (owner_peer_id != 0) e["net_owner_peer_id"] = (int)owner_peer_id;
    _state().tracked[net_id] = TrackedEntity{
        net_id,
        e.value("prefab_source", std::string()),
        replicate_transform,
        owner_peer_id
    };
    _state().net_index[net_id] = &e;  // ← keep O(1) index in sync

    Entity payload = Entity::object();
    payload["net_id"]          = (int)net_id;
    payload["prefab_path"]     = e.value("prefab_source", std::string());
    payload["x"]               = transform::world_x(e);
    payload["y"]               = transform::world_y(e);
    payload["owner_peer_id"]   = (int)owner_peer_id;
    payload["preplaced"]       = true;
    Network::SendEvent("net_spawn", payload, true);
}

} // namespace Replication