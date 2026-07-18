#pragma once
// replication_rpc.hpp — the part of host-authoritative replication that
// gameplay SCRIPTS need direct access to (NetId lookup, requesting damage,
// requesting an item pickup), split out from replication.hpp so it can be
// included from unity2d_script_api.hpp without dragging in
// prefab_system.hpp's editor dependencies (editor_state.hpp/scene_io.hpp),
// which would otherwise create an include cycle back through
// script_system.hpp.
//
// replication.hpp depends on this header (not the other way around) and
// adds the host-only pieces that DO need prefab::instantiate: Spawn(),
// Despawn(), and the net_spawn/net_despawn client-side handlers. Everything
// here is the stable, engine-level surface; everything there is the
// scene-mutating surface, only ever included at the top level (core.cpp /
// panels.hpp), the same way player_spawn.hpp already is.
//
// See replication.hpp for the full design rationale (host-authoritative
// damage/pickup, why clients never apply effects locally, etc.) — this
// file is deliberately just the calling convention, not the explanation.

#include "network.hpp"
#include "../entity.hpp"

#include <cstdint>
#include <string>
#include <functional>

namespace Replication {

// ── NetId ────────────────────────────────────────────────────────────────
// See replication.hpp for the full allocation/registry story. Scripts only
// ever need to read a NetId off an entity they already have a pointer to
// (e.g. to pass to RequestDamage), never allocate one directly.
inline std::uint32_t NetIdOf(const Entity& e) {
    return (std::uint32_t)e.value("net_id", 0);
}
inline bool IsNetworked(const Entity& e) { return NetIdOf(e) != 0; }

// ── EntityRef overloads (SFINAE — matches any type with operator->, e.g. EntityRef) ──
template <class T, decltype(std::declval<T>().operator->(), 0) = 0>
inline std::uint32_t NetIdOf(const T& e) {
    return e ? NetIdOf(*e) : 0;
}
template <class T, decltype(std::declval<T>().operator->(), 0) = 0>
inline bool IsNetworked(const T& e) { return NetIdOf(e) != 0; }

// ── Host-side handler hooks ──────────────────────────────────────────────
// replication.hpp installs the real implementations here at static-init
// time (needs HealthComponent / scene / prefab access this header
// deliberately doesn't have). If a project never includes replication.hpp
// at all (e.g. a host build that only ever calls RequestPickup from a
// client), these stay unset and the host-side branches below simply do
// nothing rather than crash.
inline std::function<void(const Network::PendingEvent&)>& _damage_handler() {
    static std::function<void(const Network::PendingEvent&)> fn;
    return fn;
}
inline std::function<void(const Network::PendingEvent&)>& _pickup_handler() {
    static std::function<void(const Network::PendingEvent&)> fn;
    return fn;
}
inline void InstallHostHandlers(std::function<void(const Network::PendingEvent&)> on_damage,
                                 std::function<void(const Network::PendingEvent&)> on_pickup) {
    _damage_handler() = std::move(on_damage);
    _pickup_handler() = std::move(on_pickup);
}

// ── Authoritative damage / death request ────────────────────────────────
//
// Call this from a weapon/hit/trap script instead of ScriptBase::take_damage()
// for anything that should work correctly in multiplayer. See
// replication.hpp's _apply_damage_request for what happens on the host side
// once this arrives — the short version: only the host's HealthComponent
// mutation is real, and the result gets broadcast back as "net_health" for
// every machine (including this one, via EventBus) to pick up.
//
// `attacker_peer_id` should be Network::LocalPeerId() of whoever is
// dealing the damage (0 for environmental/host-driven damage), so kill
// feeds / score-keeping can attribute it correctly.
inline void RequestDamage(std::uint32_t target_net_id, float amount,
                           std::uint32_t attacker_peer_id, float hit_x = 0.f, float hit_y = 0.f) {
    if (target_net_id == 0 || amount <= 0.f) return;
    Entity payload = Entity::object();
    payload["target_net_id"] = (int)target_net_id;
    payload["amount"] = amount;
    payload["attacker_peer_id"] = (int)attacker_peer_id;
    payload["hit_x"] = hit_x;
    payload["hit_y"] = hit_y;
    if (Network::IsHost()) {
        Network::PendingEvent ev;
        ev.from_peer = 0;
        ev.name = "net_damage_request";
        ev.data = payload;
        if (_damage_handler()) _damage_handler()(ev);
    } else {
        Network::SendEvent("net_damage_request", payload, true);
    }
}

// ── Item pickup request ──────────────────────────────────────────────────
// Same request/validate/broadcast shape as damage. See replication.hpp's
// _apply_pickup_request for the host-side validation (item must still
// exist — prevents two players grabbing the same drop the same frame).
inline void RequestPickup(std::uint32_t item_net_id, std::uint32_t requester_peer_id) {
    if (item_net_id == 0) return;
    Entity payload = Entity::object();
    payload["item_net_id"] = (int)item_net_id;
    payload["requester_peer_id"] = (int)requester_peer_id;
    if (Network::IsHost()) {
        Network::PendingEvent ev;
        ev.from_peer = 0;
        ev.name = "net_pickup_request";
        ev.data = payload;
        if (_pickup_handler()) _pickup_handler()(ev);
    } else {
        Network::SendEvent("net_pickup_request", payload, true);
    }
}

} // namespace Replication
