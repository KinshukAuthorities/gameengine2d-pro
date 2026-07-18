#pragma once
#include "transport.hpp"
#include "snapshot.hpp"
#include "rollback.hpp"
#include "../entity.hpp"
#include "../transform_system.hpp"
#include "../determinism.hpp"
#include <nlohmann/json.hpp>
#include <memory>
#include <unordered_map>
#include <deque>
#include <vector>
#include <string>
#include <functional>
#include <utility>

namespace Network {

struct PendingEvent {
    std::uint32_t from_peer = 0;
    std::string name;
    Entity data;
};

// One historical position sample, keyed by the owning peer id. Used for
// host-side lag compensation (see net/lag_compensation.hpp): when the host
// validates a hit a client claims it landed at some time in the recent
// past, it needs to know where the TARGET actually was at that past time,
// not where it is right now — otherwise high-ping players are effectively
// impossible to hit (they've moved on by the time their shot's packet
// reaches the host) or appear to take damage from behind cover they'd
// already reached. Recorded for every owner_peer_id transform packet the
// host receives, players only (matches what BroadcastTransform sends).
struct PositionSample {
    std::uint64_t t_ms = 0;
    float x = 0.f, y = 0.f;
};

struct State {
    bool host = false;
    bool client = false;
    std::uint32_t local_peer_id = 0;
    std::uint32_t local_server_peer_id = 0; // client-side logical server id (0)
    std::uint16_t port = 0;
    int max_clients = 0;
    std::string address;
    std::unique_ptr<net::Transport> transport;
    net::SnapshotBuffer snapshot_buffer;
    net::RollbackController rollback;
    net::InputRingBuffer input_ring;
    EntityList* scene = nullptr;
    std::vector<PendingEvent> inbox;
    std::unordered_map<std::uint32_t, net::Peer> peers;
    bool transport_ready = false;
    bool in_match = false;  // set by Matchmaking when a match starts/ends; guards rollback capture

    // Lag-compensation history: owner_peer_id -> recent position samples,
    // newest at the back. ~1 second at typical broadcast rates is enough
    // to cover any reasonable RTT; see net/lag_compensation.hpp::RewindTo().
    // deque is used so pop_front() is O(1) instead of the O(N) vector shift
    // that was happening on every received transform packet.
    std::unordered_map<std::uint32_t, std::deque<PositionSample>> position_history;
};

inline State& _local_state() { static State s; return s; }
inline State*& _state_ptr() { static State* p = nullptr; return p; }
inline void bind_state(State* host) { _state_ptr() = host; }
inline State& _state() { return _state_ptr() ? *_state_ptr() : _local_state(); }

inline bool IsHost() { return _state().host; }
inline bool IsClient() { return _state().client; }
inline bool IsConnected() { return _state().transport_ready && _state().transport != nullptr; }
inline std::uint32_t LocalPeerId() { return _state().local_peer_id; }

// RTT for a specific peer (host side, given a client's peer id), measured by
// the transport's heartbeat ping/pong (see UdpTransport::poll). Returns 0 if
// unknown or not yet measured.
inline std::uint32_t GetPeerRttMs(std::uint32_t peer_id) {
    auto& s = _state();
    if (!s.transport) return 0;
    return s.transport->get_rtt_ms(peer_id);
}

// RTT to the host, for use on the client side (always logical peer 0).
inline std::uint32_t GetHostRttMs() { return GetPeerRttMs(0); }

inline void BindScene(EntityList* scene) { _state().scene = scene; }
inline void SetInMatch(bool v) { _state().in_match = v; }

inline void _ensure_transport() {
    auto& s = _state();
    if (!s.transport) s.transport = net::make_udp_transport();
}

inline void _reset_modes() {
    auto& s = _state();
    s.host = s.client = false;
    s.transport_ready = false;
    s.in_match = false;
    s.peers.clear();
    s.local_peer_id = 0;
    s.local_server_peer_id = 0;
    s.address.clear();
    s.port = 0;
    s.max_clients = 0;
    s.inbox.clear();
    s.snapshot_buffer.clear();
    s.rollback.clear();
    s.input_ring.clear();
    s.position_history.clear();
}

inline void Host(std::uint16_t port, int max_clients) {
    auto& s = _state();
    _ensure_transport();
    s.transport->host(port, max_clients);
    _reset_modes();
    s.host = true;
    s.transport_ready = true;
    s.port = port;
    s.max_clients = max_clients;
    s.local_peer_id = 0;
}

inline void Connect(const std::string& addr, std::uint16_t port) {
    auto& s = _state();
    _ensure_transport();
    s.transport->connect(addr, port);
    _reset_modes();
    s.client = true;
    s.transport_ready = true;
    s.address = addr;
    s.port = port;
    s.local_server_peer_id = 0;
}

inline void Shutdown() {
    auto& s = _state();
    s.transport.reset();
    s.host = s.client = false;
    s.transport_ready = false;
    s.local_peer_id = 0;
    s.local_server_peer_id = 0;
    s.inbox.clear();
    s.scene = nullptr;
    s.snapshot_buffer.clear();
    s.rollback.clear();
    s.input_ring.clear();
    s.peers.clear();
    s.position_history.clear();
}

inline std::vector<PendingEvent> ConsumeEvents() {
    auto& s = _state();
    std::vector<PendingEvent> out;
    out.swap(s.inbox);
    return out;
}

inline void _encode_event_packet(std::vector<std::uint8_t>& bytes, const std::string& event_name, const Entity& data) {
    nlohmann::json j = data;
    std::string payload = j.dump();
    bytes.clear();
    bytes.reserve(1 + 4 + event_name.size() + 4 + payload.size());
    bytes.push_back(0xEE);
    std::uint32_t name_len = (std::uint32_t)event_name.size();
    for (int i = 0; i < 4; ++i) bytes.push_back((std::uint8_t)((name_len >> (i * 8)) & 0xFF));
    bytes.insert(bytes.end(), event_name.begin(), event_name.end());
    std::uint32_t len = (std::uint32_t)payload.size();
    for (int i = 0; i < 4; ++i) bytes.push_back((std::uint8_t)((len >> (i * 8)) & 0xFF));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
}

inline void _encode_transform_packet(std::vector<std::uint8_t>& bytes, const Entity& e) {
    nlohmann::json j;
    auto& t = e["components"]["Transform"];
    j["entity_id"] = e.value("id", 0);
    j["owner_peer_id"] = e.contains("net_owner_peer_id") ? e.value("net_owner_peer_id", 0) : 0;
    // net_id (see net/replication.hpp) is included, when present, so
    // receivers can feed this same update into the generic interpolation /
    // lag-compensation history (net/net_predict.hpp, net/lag_compensation.hpp)
    // keyed identically to every other replicated entity, instead of player
    // transforms being a special case with no history at all.
    j["net_id"] = e.contains("net_id") ? e.value("net_id", 0) : 0;
    j["x"] = t.value("x", 0.f);
    j["y"] = t.value("y", 0.f);
    j["rotation"] = t.value("rotation", 0.f);
    j["scale_x"] = t.value("scale_x", 1.f);
    j["scale_y"] = t.value("scale_y", 1.f);
    j["vx"] = t.value("velocity_x", 0.f);
    j["vy"] = t.value("velocity_y", 0.f);
    std::string payload = j.dump();
    bytes.clear();
    bytes.reserve(1 + 4 + payload.size());
    bytes.push_back(0xEF);
    std::uint32_t len = (std::uint32_t)payload.size();
    for (int i = 0; i < 4; ++i) bytes.push_back((std::uint8_t)((len >> (i * 8)) & 0xFF));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
}

inline void SendEvent(const std::string& event_name, const Entity& data, bool reliable = true) {
    auto& s = _state();
    if (!s.transport_ready || !s.transport) return;
    std::vector<std::uint8_t> bytes;
    _encode_event_packet(bytes, event_name, data);
    if (s.host) {
        for (const auto& [peer_id, peer] : s.peers) {
            (void)peer;
            s.transport->send(peer_id, bytes.data(), bytes.size(), reliable);
        }
    } else {
        s.transport->send(0, bytes.data(), bytes.size(), reliable);
    }
}

inline void SendEventTo(std::uint32_t peer_id, const std::string& event_name, const Entity& data, bool reliable = true) {
    auto& s = _state();
    if (!s.transport_ready || !s.transport) return;
    std::vector<std::uint8_t> bytes;
    _encode_event_packet(bytes, event_name, data);
    s.transport->send(peer_id, bytes.data(), bytes.size(), reliable);
}

inline void BroadcastTransform(Entity& e) {
    auto& s = _state();
    if (!s.transport_ready || !s.transport || !has_component(e, "Transform")) return;
    std::vector<std::uint8_t> bytes;
    _encode_transform_packet(bytes, e);
    if (s.host) {
        for (const auto& [peer_id, peer] : s.peers) {
            (void)peer;
            s.transport->send(peer_id, bytes.data(), bytes.size(), false);
        }
    } else {
        s.transport->send(0, bytes.data(), bytes.size(), false);
    }
}

inline void CaptureSnapshot(std::uint32_t frame) {
    auto& s = _state();
    if (!s.scene) return;
    // Skip all snapshot work when there are no peers or not in a match —
    // interpolation and rollback are only meaningful during live gameplay.
    // Running capture_snapshot every frame with no peers (or in lobby) was
    // iterating the full entity list for nothing, causing editor FPS drops
    // on the client side after joining a match.
    if (s.peers.empty() || !s.in_match) return;
    // Lightweight transform-only snapshot every frame (used for interpolation).
    auto snap = net::capture_snapshot(*s.scene);
    snap.tick = frame;
    s.snapshot_buffer.push(std::move(snap));
    // Rollback deep-clone disabled: too expensive during editor play mode
    // (gradual FPS drain as entity count grows). Rollback is only needed
    // in exported standalone builds, not in-editor sessions.
    // if (frame % 6 == 0) { s.rollback.capture(frame, *s.scene); }
}

inline bool ResimulateFrom(std::uint32_t from_frame, std::uint32_t to_frame, net::RollbackController::StepFn step) {
    auto& s = _state();
    if (!s.scene) return false;
    return s.rollback.resimulate_from(from_frame, to_frame, *s.scene, std::move(step));
}

inline void _apply_transform_packet(const nlohmann::json& j) {
    auto& s = _state();
    if (!s.scene) return;

    auto apply_to = [&](Entity& e) -> bool {
        if (!has_component(e, "Transform")) return false;
        auto& t = e["components"]["Transform"];
        t["x"] = j.value("x", 0.f);
        t["y"] = j.value("y", 0.f);
        t["rotation"] = j.value("rotation", 0.f);
        t["scale_x"] = j.value("scale_x", 1.f);
        t["scale_y"] = j.value("scale_y", 1.f);
        if (t.contains("velocity_x")) t["velocity_x"] = j.value("vx", 0.f);
        if (t.contains("velocity_y")) t["velocity_y"] = j.value("vy", 0.f);
        return true;
    };

    int owner_peer = j.value("owner_peer_id", -1);
    // Record a lag-compensation sample for this peer's position regardless
    // of whether we end up finding a live entity to apply it to below —
    // the history only needs (peer, time, x, y), it doesn't need the
    // Entity itself.
    if (s.host && owner_peer > 0) {
        auto& hist = s.position_history[(std::uint32_t)owner_peer];
        hist.push_back(PositionSample{net::detail::now_ms(), j.value("x", 0.f), j.value("y", 0.f)});
        while (hist.size() > 64) hist.pop_front();
    }

    if (owner_peer >= 0) {
        for (auto& e : *s.scene) {
            if (!e.contains("net_owner_peer_id")) continue;
            if ((int)e.value("net_owner_peer_id", -1) != owner_peer) continue;
            if (apply_to(e)) {
                transform::mark_structure_dirty();
                return;
            }
        }
    }

    int id = j.value("entity_id", 0);
    for (auto& e : *s.scene) {
        if (e.value("id", 0) != id) continue;
        if (apply_to(e)) break;
    }
    transform::mark_structure_dirty();
}

// Returns the best-effort historical (x, y) for a given peer at or before
// `at_time_ms`, by scanning that peer's recorded samples. Returns false if
// there is no history yet (e.g. peer just connected) — callers should fall
// back to the entity's current live position in that case.
inline bool GetPeerPositionAt(std::uint32_t peer_id, std::uint64_t at_time_ms, float& out_x, float& out_y) {
    auto& s = _state();
    auto it = s.position_history.find(peer_id);
    if (it == s.position_history.end() || it->second.empty()) return false;
    const auto& hist = it->second;

    // Samples are in arrival order (push_back), which is monotonic in
    // practice since they come off a single reliable-ordered-enough stream,
    // but be defensive: find the last sample at or before at_time_ms, or
    // interpolate between the bracketing pair if both exist.
    const PositionSample* before = nullptr;
    const PositionSample* after = nullptr;
    for (const auto& smp : hist) {
        if (smp.t_ms <= at_time_ms) before = &smp;
        else { after = &smp; break; }
    }
    if (before && after && after->t_ms > before->t_ms) {
        float alpha = (float)(at_time_ms - before->t_ms) / (float)(after->t_ms - before->t_ms);
        alpha = std::max(0.f, std::min(1.f, alpha));
        out_x = before->x + (after->x - before->x) * alpha;
        out_y = before->y + (after->y - before->y) * alpha;
        return true;
    }
    const PositionSample* use = before ? before : (after ? after : &hist.back());
    out_x = use->x;
    out_y = use->y;
    return true;
}

inline void Update(float timeout_ms = 0.f) {
    auto& s = _state();
    if (!s.transport_ready || !s.transport) return;

    s.transport->poll(timeout_ms,
        [&](std::uint32_t peer_id, const std::uint8_t* data, std::size_t len) {
            if (!data || !len) return;
            if (data[0] == 0xEE) {
                if (len < 1 + 4) return;
                auto read_u32 = [&](const std::uint8_t* p) -> std::uint32_t {
                    return (std::uint32_t)p[0] | ((std::uint32_t)p[1] << 8) | ((std::uint32_t)p[2] << 16) | ((std::uint32_t)p[3] << 24);
                };
                std::size_t off = 1;
                std::uint32_t name_len = read_u32(data + off); off += 4;
                if (off + name_len + 4 > len) return;
                std::string name((const char*)data + off, (std::size_t)name_len); off += name_len;
                std::uint32_t payload_len = read_u32(data + off); off += 4;
                if (off + payload_len > len) return;
                std::string payload((const char*)data + off, (std::size_t)payload_len);
                try {
                    nlohmann::json j = nlohmann::json::parse(payload);
                    s.inbox.push_back(PendingEvent{peer_id, std::move(name), Entity(j)});
                } catch (...) {}
                // Relay: the host is a relay point for client<->client
                // traffic, not just a client<->host endpoint. Without this,
                // a non-host player's event only ever reached the host
                // itself — other clients never saw it at all.
                if (s.host) {
                    for (const auto& [other_id, other_peer] : s.peers) {
                        (void)other_peer;
                        if (other_id == peer_id) continue;
                        s.transport->send(other_id, data, len, true);
                    }
                }
                return;
            }
            if (data[0] == 0xEF) {
                if (len < 1 + 4) return;
                auto read_u32 = [&](const std::uint8_t* p) -> std::uint32_t {
                    return (std::uint32_t)p[0] | ((std::uint32_t)p[1] << 8) | ((std::uint32_t)p[2] << 16) | ((std::uint32_t)p[3] << 24);
                };
                std::uint32_t payload_len = read_u32(data + 1);
                if (len < 5 + payload_len) return;
                try {
                    nlohmann::json j = nlohmann::json::parse(std::string((const char*)data + 5, payload_len));
                    _apply_transform_packet(j);
                } catch (...) {}
                // Same relay as above: a client's transform update needs to
                // reach every *other* client, not just update the host's
                // own local copy of that entity. This is what makes two
                // non-host players actually see each other move.
                if (s.host) {
                    for (const auto& [other_id, other_peer] : s.peers) {
                        (void)other_peer;
                        if (other_id == peer_id) continue;
                        s.transport->send(other_id, data, len, false);
                    }
                }
                return;
            }
        },
        [&](net::Peer peer) {
            if (!s.host && s.client) {
                s.local_peer_id = peer.id;
                s.local_server_peer_id = 0;
                s.inbox.push_back(PendingEvent{peer.id, "peer_connected", Entity::object()});
                return;
            }
            s.peers[peer.id] = peer;
            s.inbox.push_back(PendingEvent{peer.id, "peer_connected", Entity::object()});
        },
        [&](net::Peer peer) {
            s.peers.erase(peer.id);
            bool was_server = (!s.host && s.client && peer.id == s.local_peer_id);
            if (was_server) {
                s.local_peer_id = 0;
            }
            Entity data = Entity::object();
            data["address"] = peer.address;
            data["rtt_ms"] = (int)peer.rtt_ms;
            data["was_server"] = was_server;
            s.inbox.push_back(PendingEvent{peer.id, "peer_disconnected", data});
        }
    );
}

inline void SeedSession(std::uint64_t seed) { engine_det::seed_session(seed); }
inline void SeedSessionFromString(const std::string& s) { engine_det::seed_session_from_string(s); }

} // namespace Network