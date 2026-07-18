#pragma once
// player_spawn.hpp — spawns one instance of the project's player prefab per
// connected lobby member once a match's scene has actually finished loading,
// and keeps everyone's player entities in sync over the network.
//
// This sits on top of (but outside) matchmaking.hpp on purpose: matchmaking
// only knows about peers/lobbies/events, never about EntityList or prefabs,
// so it can't call instantiate() itself. This header is the bridge — it's
// included by the editor (editor_main.cpp / panels.hpp) and the standalone
// runtime (core.cpp), both of which already have a live EntityList and
// prefab_system.hpp in scope.
//
// Ownership is tracked by a top-level "net_owner_peer_id" field on the
// spawned root entity (sibling of "id"/"name", not inside "components"), not
// by the engine's local entity id — each machine allocates entity ids
// independently when it instantiates the prefab, so the peer id is the only
// thing guaranteed to mean the same player on every machine. A second field,
// "net_is_local", marks exactly one spawned player as the one this machine
// actually controls (see Matchmaking's peer-id-equals-LocalPeerId rule,
// which works identically on host and client).

#include "matchmaking.hpp"
#include "../prefab_system.hpp"
#include "../entity.hpp"

#include <string>

namespace NetSpawn {

inline Entity* find_by_id(EntityList& scene, int id) {
    for (auto& e : scene) {
        if (e.value("id", 0) == id) return &e;
    }
    return nullptr;
}

inline Entity* find_by_owner_peer(EntityList& scene, std::uint32_t peer_id) {
    for (auto& e : scene) {
        if (e.contains("net_owner_peer_id") && (std::uint32_t)e.value("net_owner_peer_id", -1) == peer_id)
            return &e;
    }
    return nullptr;
}

// Removes any previously-spawned net player entities. Called before
// spawning a fresh set so re-starting a match (or a late state resync)
// never leaves stale/duplicate player entities behind.
inline void despawn_all(EntityList& scene) {
    scene.erase(std::remove_if(scene.begin(), scene.end(),
        [](const Entity& e) { return e.contains("net_owner_peer_id"); }), scene.end());
}

// Spawns one player prefab instance per current lobby member. Call this
// once, right after the match's scene has actually finished loading on this
// machine — not from StartMatch() itself, since that only queues a deferred
// scene switch (see matchmaking.hpp's _queue_scene_load) and the real swap
// happens later in the frame loop. Calling this before the new scene is
// live would instantiate into the scene that's about to be replaced.
inline void SpawnAllPlayers(EntityList& scene, const std::string& asset_dir) {
    despawn_all(scene);
    std::string prefab_path = Matchmaking::GetPlayerPrefabPath();
    if (prefab_path.empty()) {
        Debug::log_warning("NetSpawn::SpawnAllPlayers: player_prefab_path is empty, nothing to spawn.");
        return;
    }

    auto members = Matchmaking::Members();
    std::uint32_t local_id = Network::LocalPeerId();
    // 120 world-units apart — enough to see both players on screen at spawn.
    // (The old value of 2.0f placed everyone on top of each other.)
    float spacing = 120.0f;

    Debug::log(std::string("NetSpawn::SpawnAllPlayers: spawning ") + std::to_string(members.size()) +
               " player(s) from '" + prefab_path + "' (asset_dir='" + asset_dir + "', local_id=" + std::to_string(local_id) + ")");

    int index = 0;
    for (const auto& m : members) {
        float x = (float)index * spacing;
        float y = 0.f;
        int root_id = prefab::instantiate(prefab_path, scene, asset_dir, x, y);
        if (root_id < 0) {
            Debug::log_warning(std::string("NetSpawn::SpawnAllPlayers: instantiate() FAILED for peer ") +
                                std::to_string(m.peer_id) + " (\"" + m.name + "\") — prefab not found/invalid at '" +
                                prefab_path + "' relative to '" + asset_dir + "'.");
            ++index;
            continue;
        }
        Entity* e = find_by_id(scene, root_id);
        ++index;
        if (!e) {
            Debug::log_warning("NetSpawn::SpawnAllPlayers: instantiate() returned id " + std::to_string(root_id) +
                                " but it could not be found in the scene right after creation.");
            continue;
        }
        (*e)["net_owner_peer_id"] = (int)m.peer_id;
        (*e)["net_is_local"] = (m.peer_id == local_id);
        (*e)["net_player_name"] = m.name;
        Debug::log(std::string("NetSpawn::SpawnAllPlayers: spawned entity id ") + std::to_string(root_id) +
                   " for peer " + std::to_string(m.peer_id) + " (\"" + m.name + "\") local=" +
                   (m.peer_id == local_id ? "true" : "false") + " at x=" + std::to_string(x));
    }
    transform::rebuild_registry(scene);
}

// Call once per frame (host and client both) while InMatch() is true, after
// your own player script has already updated its local position for this
// frame. Broadcasts the local player's transform to every other connected
// peer, and relays any remote players' transforms that arrived this frame
// onward to the right local entity.
//
// This replaces calling Network::BroadcastTransform() directly from a
// script for multiplayer purposes: that call only sends client->host or
// host->all — a client's update reaching the host was never being
// forwarded on to *other* clients, which is why two non-host players could
// never see each other moving. ReplicateLocalPlayer takes care of that
// relay in one place instead of requiring every script to know about it.
inline void ReplicateLocalPlayer(EntityList& scene) {
    std::uint32_t local_id = Network::LocalPeerId();
    Entity* local = find_by_owner_peer(scene, local_id);
    if (local && has_component(*local, "Transform")) {
        Network::BroadcastTransform(*local);
    }
}

} // namespace NetSpawn
