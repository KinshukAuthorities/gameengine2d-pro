#pragma once
// lag_compensation.hpp — host-side hit validation against where a target
// ACTUALLY was at the moment a client's shot was fired, not where it is by
// the time the packet arrives.
//
// Why this matters: a client at 100ms RTT is always shooting at a ~50-100ms
// stale picture of the world (their own screen shows everyone else slightly
// in the past, because that's the most recent update they've received).
// When they fire and their shot lands on-screen, the host — which is
// running the REAL, current positions — may find that the target has
// already moved away from that spot by the time the "I hit them" request
// arrives. Without compensation, anyone with non-trivial ping effectively
// can't land hits on moving targets, which is exactly backwards from how
// it should feel.
//
// The standard fix (used by every competitive shooter, including the
// games this is modeled after) is: when validating a hit, rewind the
// TARGET back to where the position history says they were at
// (now - shooter's RTT), and hit-test against that rewound position
// instead of the target's current one. The shooter's own client-side
// prediction already drew the shot as landing where they saw it, so this
// makes the host's authoritative call match what the shooter actually saw
// on their screen.
//
// This deliberately does NOT rewind the whole world/physics step (that's
// what net/rollback.hpp's RollbackController + EntityList snapshots are
// for, and is overkill for 2-4 player co-op hit validation) — just the one
// target entity's position, for one hit-test, for one frame of host logic.

#include "network.hpp"
#include "replication_rpc.hpp"
#include "../entity.hpp"
#include "../transform_system.hpp"

#include <cstdint>
#include <cmath>
#include <algorithm>

namespace LagCompensation {

// Returns the world position `target` is recorded to have been at
// `rewind_ms` milliseconds ago, using the host's recorded position history
// for that entity's owning peer (see Network::GetPeerPositionAt). Falls
// back to the entity's current live position if there's no history yet
// (e.g. they only just connected) — never returns a position so old it
// predates anything we've recorded.
inline bool RewindPosition(const Entity& target, std::uint32_t rewind_ms, float& out_x, float& out_y) {
    if (!Network::IsHost()) return false; // only meaningful on the host
    std::uint32_t owner = (std::uint32_t)target.value("net_owner_peer_id", 0);
    if (owner == 0) return false; // not a player-owned entity — nothing to rewind

    std::uint64_t now = net::detail::now_ms();
    std::uint64_t at_time = (rewind_ms < now) ? (now - rewind_ms) : 0;

    if (Network::GetPeerPositionAt(owner, at_time, out_x, out_y)) return true;

    // No history yet — use current position rather than failing the hit
    // outright (better to validate against "now" than to always reject a
    // brand-new player).
    out_x = transform::world_x(target);
    out_y = transform::world_y(target);
    return true;
}

// Convenience overload: rewinds using the SHOOTER's current RTT (the
// natural choice — "where was the target when this shooter's screen showed
// them getting hit" is approximately one RTT ago, since the shooter saw a
// stale snapshot and their shot took another half-trip to reach us).
// Clamped to a sane max so a disconnecting/lagging shooter can't claim hits
// arbitrarily far in the past.
inline bool RewindPositionForShooter(const Entity& target, std::uint32_t shooter_peer_id,
                                      float& out_x, float& out_y, std::uint32_t max_rewind_ms = 500) {
    std::uint32_t rtt = Network::GetPeerRttMs(shooter_peer_id);
    std::uint32_t rewind_ms = std::min(rtt, max_rewind_ms);
    return RewindPosition(target, rewind_ms, out_x, out_y);
}

// Host-side lag-compensated hit test: did `shooter_peer_id`'s shot, fired
// at `(shot_x, shot_y)` with the given hit radius, actually land on
// `target` once the target is rewound to where it was at shot-time? Use
// this INSTEAD of testing against the target's live position before
// calling Replication::RequestDamage — e.g. from a host-side weapon/hit
// script:
//
//   if (LagCompensation::ValidateHit(*target, shooter_id, hit_x, hit_y, 16.f)) {
//       Replication::RequestDamage(Replication::NetIdOf(*target), dmg, shooter_id, hit_x, hit_y);
//   }
inline bool ValidateHit(const Entity& target, std::uint32_t shooter_peer_id,
                         float shot_x, float shot_y, float hit_radius,
                         std::uint32_t max_rewind_ms = 500) {
    if (!Network::IsHost()) return false;
    float tx, ty;
    if (!RewindPositionForShooter(target, shooter_peer_id, tx, ty, max_rewind_ms)) {
        // No owner/history at all (e.g. target is a host-simulated enemy,
        // not a player) — fall back to a live-position check.
        tx = transform::world_x(target);
        ty = transform::world_y(target);
    }
    float dx = tx - shot_x, dy = ty - shot_y;
    return (dx * dx + dy * dy) <= (hit_radius * hit_radius);
}

} // namespace LagCompensation
