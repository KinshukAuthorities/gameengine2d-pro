#pragma once
// net_predict.hpp — client-side prediction, reconciliation, and remote-entity
// interpolation.
//
// Two different problems, two different techniques, both living here:
//
//  1. THE LOCAL PLAYER (the one this machine controls): the player's own
//     script already moves it immediately, every frame, with zero added
//     latency — that's correct and unchanged ("client-side prediction" is
//     just "don't wait for the network before moving the thing you
//     control"). The part that was missing is RECONCILIATION: the host is
//     still authoritative (anti-cheat, server-side physics/collision), so
//     when it disagrees with where prediction landed, the correction must
//     be blended in smoothly instead of teleporting the player sideways
//     every time a packet arrives. ReconcileLocalPlayer() does that.
//
//  2. EVERYONE/EVERYTHING ELSE (remote players, host-simulated enemies,
//     moving platforms): this machine has no input for them at all, only
//     whatever position updates arrive over the network — at most ~20-60
//     times/sec (see replication.hpp's Tick / player_spawn.hpp's per-frame
//     broadcast), far below the render frame rate. Snapping to each update
//     looks jittery/stepped. InterpolateRemote() smoothly walks toward the
//     latest known authoritative position every render frame instead,
//     using the engine's existing net::SnapshotBuffer for players and a
//     small per-entity buffer here for everything else.
//
// Both pieces are deliberately transform-only and component-agnostic
// (they never touch Animator/Script logic) so they compose with whatever
// gameplay scripts are already doing.

#include "network.hpp"
#include "replication_rpc.hpp"
#include "../entity.hpp"
#include "../transform_system.hpp"

#include <unordered_map>
#include <deque>
#include <cmath>
#include <algorithm>

namespace NetPredict {

// ── 1. Local player reconciliation ──────────────────────────────────────────
//
// The host doesn't currently send the local player's own position back
// (player replication is "everyone broadcasts their own transform, host
// relays it on" — see network.hpp's BroadcastTransform/relay). For
// reconciliation to mean anything, the host needs to be the one deciding
// where the local player ACTUALLY is and telling the client. We add that
// here as an opt-in authoritative correction channel ("net_player_correct")
// rather than changing the existing always-trust-the-owner transform path,
// so single-host-no-validation projects are unaffected if they never call
// RequestServerMove / never enable corrections.
//
// Usage from a player movement script (host-authoritative anti-cheat /
// server-reconciled movement):
//   - Client moves locally every frame as normal (unchanged).
//   - Client periodically calls NetPredict::SendMoveState(x, y, vx, vy) —
//     tells the host "this is where I think I am".
//   - Host validates (speed clamp, collision, etc. — project-specific;
//     left to the host's own physics/script logic since that's already
//     authoritative there) and, only if it disagrees by more than a small
//     tolerance, calls NetPredict::CorrectPlayer(peer_id, x, y) to push the
//     true position back down.
//   - Client's ReconcileLocalPlayer() call each frame blends toward any
//     pending correction over a few frames instead of snapping, so a minor
//     correction is invisible and even a large one (rollback after a wall
//     the client clipped through) reads as a quick slide, not a teleport.

struct PendingCorrection {
    bool active = false;
    float target_x = 0.f, target_y = 0.f;
    float blend = 0.f; // 0..1, advances each frame until correction is fully applied
};

inline PendingCorrection& _correction() { static PendingCorrection c; return c; }

// Client calls this every N frames (a few times/sec is enough — this is
// not the per-frame movement path, just a periodic "here's my state" check
// the host can use for anti-cheat / drift correction).
inline void SendMoveState(float x, float y, float vx, float vy) {
    if (!Network::IsClient()) return;
    Entity payload = Entity::object();
    payload["x"] = x; payload["y"] = y; payload["vx"] = vx; payload["vy"] = vy;
    payload["client_time_ms"] = (double)net::detail::now_ms();
    Network::SendEvent("net_move_state", payload, false);
}

// Host calls this (typically from a script reacting to "net_move_state" via
// EventBus, after running its own authoritative collision/speed check)
// to push a corrected position to a specific client. Does nothing if
// called on a client.
inline void CorrectPlayer(std::uint32_t peer_id, float x, float y) {
    if (!Network::IsHost()) return;
    Entity payload = Entity::object();
    payload["x"] = x; payload["y"] = y;
    Network::SendEventTo(peer_id, "net_player_correct", payload, true);
}

// Client calls this when a "net_player_correct" event is consumed (wire it
// up next to Matchmaking::HandleNetworkEvent in the frame loop — see
// HandleEvent() below for the one-line version that does both).
inline void QueueCorrection(float x, float y) {
    auto& c = _correction();
    c.active = true;
    c.target_x = x;
    c.target_y = y;
    c.blend = 0.f;
}

// Call once per frame for the local player's entity, AFTER your movement
// script has already updated Transform for this frame. Blends at most
// `rate` of the remaining distance per second toward any pending
// correction (default: closes ~85% of the gap per second, i.e. corrections
// resolve in well under a second and small ones are imperceptible) rather
// than snapping outright. No-ops if there is no pending correction.
inline void ReconcileLocalPlayer(Entity& player, float dt, float rate = 10.0f) {
    auto& c = _correction();
    if (!c.active || !has_component(player, "Transform")) return;
    auto& t = player["components"]["Transform"];
    float cx = t.value("x", 0.f), cy = t.value("y", 0.f);
    float dx = c.target_x - cx, dy = c.target_y - cy;
    float dist2 = dx * dx + dy * dy;
    if (dist2 < 0.01f) { c.active = false; return; } // close enough, done

    float alpha = std::min(1.f, rate * dt);
    t["x"] = cx + dx * alpha;
    t["y"] = cy + dy * alpha;
    transform::mark_local_dirty(player.value("id", 0));
}

// ── 2. Remote-entity interpolation ──────────────────────────────────────────
//
// A small ring buffer of recent (time, x, y, rotation) samples per net_id,
// fed by replication.hpp's net_world_state handler and player_spawn's
// transform relay alike. Render-time lookups interpolate between the two
// samples bracketing "now minus a small delay buffer", which trades a
// little extra latency (one buffer interval, typically ~50-100ms) for
// motion that's always smooth even though updates arrive in bursts at a
// much lower rate than the render frame rate.

struct Sample {
    std::uint64_t t_ms = 0;
    float x = 0.f, y = 0.f, rotation = 0.f;
};

struct RemoteBuffer {
    std::deque<Sample> samples;
    void push(const Sample& s) {
        samples.push_back(s);
        while (samples.size() > 8) samples.pop_front();
    }
};

inline std::unordered_map<std::uint32_t, RemoteBuffer>& _buffers() {
    static std::unordered_map<std::uint32_t, RemoteBuffer> b;
    return b;
}

inline void PushRemoteSample(std::uint32_t net_id, float x, float y, float rotation) {
    _buffers()[net_id].push(Sample{net::detail::now_ms(), x, y, rotation});
}

inline void ClearRemoteBuffers() { _buffers().clear(); }

// Interpolation delay: render slightly in the past so there are (almost)
// always two real samples to interpolate between instead of extrapolating
// off the front of the buffer. 100ms is a reasonable default for LAN/co-op
// play; raise it if your tick_interval (replication.hpp::SetTickRate) is
// lower-frequency than the default 20/s.
inline float& InterpDelayMs() { static float v = 100.f; return v; }

// Call once per render frame for any entity that has a net_id and is NOT
// the local player (i.e. every remote player, every host-simulated enemy/
// item/platform a client is just watching). Looks up that entity's sample
// buffer and writes a smoothly interpolated position into its Transform.
// No-op if there's no buffer yet (e.g. the very first frame after spawn) —
// the position from net_spawn's initial x/y stands until samples arrive.
inline void InterpolateRemote(Entity& e) {
    std::uint32_t net_id = Replication::NetIdOf(e);
    if (net_id == 0 || !has_component(e, "Transform")) return;
    auto it = _buffers().find(net_id);
    if (it == _buffers().end() || it->second.samples.size() < 2) return;

    const auto& buf = it->second.samples;
    std::uint64_t target_t = net::detail::now_ms() - (std::uint64_t)InterpDelayMs();

    // Find the two samples bracketing target_t. Buffer is push_back order
    // (oldest..newest), so walk forward to the first sample newer than
    // target_t; the previous one is the lower bracket.
    const Sample* a = &buf.front();
    const Sample* b = &buf.back();
    for (std::size_t i = 1; i < buf.size(); ++i) {
        if (buf[i].t_ms >= target_t) { a = &buf[i - 1]; b = &buf[i]; break; }
        a = &buf[i];
    }
    float alpha = 0.f;
    if (b->t_ms > a->t_ms) {
        alpha = (float)(target_t - a->t_ms) / (float)(b->t_ms - a->t_ms);
        alpha = std::max(0.f, std::min(1.f, alpha));
    }
    float x = a->x + (b->x - a->x) * alpha;
    float y = a->y + (b->y - a->y) * alpha;
    float rot = a->rotation + (b->rotation - a->rotation) * alpha;

    auto& t = e["components"]["Transform"];
    t["x"] = x; t["y"] = y; t["rotation"] = rot;
    transform::mark_local_dirty(e.value("id", 0));
}

// Convenience: call once per frame (client-side, after Network::Update())
// to interpolate every networked, non-local entity in the scene in one
// pass, instead of calling InterpolateRemote() per-entity from scripts.
// Players still skip their owner's locally-controlled instance (checked
// via net_is_local, same field player_spawn.hpp already stamps).
inline void InterpolateAllRemote(EntityList& scene) {
    if (Network::IsHost()) return; // host has the real, authoritative state already
    for (auto& e : scene) {
        if (e.value("net_is_local", false)) continue; // local player: prediction path, not this
        if (Replication::NetIdOf(e) == 0) continue;
        InterpolateRemote(e);
    }
}

// One-line event hook: call this for every event returned by
// Network::ConsumeEvents() (alongside Matchmaking::HandleNetworkEvent and
// Replication::HandleNetworkEvent) so net_world_state samples also feed the
// interpolation buffers, and net_player_correct feeds reconciliation.
inline void HandleNetworkEvent(const Network::PendingEvent& ev) {
    if (ev.name == "net_player_correct") {
        QueueCorrection(ev.data.value("x", 0.f), ev.data.value("y", 0.f));
        return;
    }
    if (ev.name == "net_world_state") {
        if (!ev.data.contains("entities") || !ev.data["entities"].is_array()) return;
        for (const auto& row : ev.data["entities"]) {
            std::uint32_t net_id = (std::uint32_t)row.value("net_id", 0);
            if (net_id == 0) continue;
            PushRemoteSample(net_id, row.value("x", 0.f), row.value("y", 0.f), row.value("rotation", 0.f));
        }
        return;
    }
}

} // namespace NetPredict
