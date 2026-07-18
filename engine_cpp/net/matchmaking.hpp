#pragma once

#include "transport.hpp"
#include "network.hpp"
#include "../entity.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <filesystem>

namespace SceneManager {
    bool HasLoadSceneHandler();
    void LoadScene(const std::string& scene_path);
}

namespace Matchmaking {

class DiscoverySocket;
inline DiscoverySocket& _discovery_sender();
inline DiscoverySocket& _discovery_listener();

struct ChatLine {
    std::string timestamp;
    std::string sender;
    std::string message;
};

struct LobbyMember {
    std::uint32_t peer_id = 0;
    std::string    name = "Player";
    bool           ready = false;
    bool           host = false;
    std::uint32_t  ping_ms = 0;
};

struct BrowserEntry {
    std::string address;
    std::uint16_t port = 0;
    std::string lobby_name = "Lobby";
    std::string player_name = "Host";
    std::string map_name;
    std::string project_name;
    std::string mode_name;
    std::string region;
    std::string build_version;
    std::vector<std::string> tags;
    int players = 0;
    int max_players = 0;
    bool public_lobby = true;
    bool password_required = false;
    bool auto_start = false;
    bool in_match = false;
    int ping_ms = 0;
    std::uint64_t last_seen_ms = 0;
};

struct State {
    bool visible = false;
    bool hosting = false;
    bool connected = false;
    bool in_lobby = false;
    bool in_match = false;
    bool public_lobby = true;
    bool password_required = false;
    bool auto_start = false;
    bool lan_discovery = true;
    bool ready = false;
    bool quickmatch_active = false;
    bool pending_scene_valid = false;
    bool join_confirmed = false;

    std::uint16_t port = 7777;
    std::uint16_t discovery_port = 45454;
    int max_players = 8;

    std::string player_name = "Player";
    std::string project_name = "";
    std::string lobby_name = "New Lobby";
    std::string map_name = "";
    std::string mode_name = "Casual";
    std::string region = "LAN";
    std::string build_version = "1.0";
    std::string address = "127.0.0.1";
    std::string password;
    std::string join_code;
    std::string last_error;
    std::string chat_input;
    std::string quickmatch_mode_filter;
    std::string quickmatch_map_filter;
    std::string pending_scene;
    std::string player_prefab_path = "AbyssPlayer.prefab";
    float spawn_spacing_x = 2.0f;
    std::uint64_t quickmatch_started_ms = 0;
    std::uint64_t join_started_ms = 0;
    int quickmatch_party_size = 1;

    std::uint64_t lobby_id = 0;
    std::uint64_t last_advertise_ms = 0;
    std::uint64_t last_browser_cleanup_ms = 0;
    std::uint64_t last_browser_poll_ms = 0; // throttle UDP browser poll to 10 Hz

    std::vector<LobbyMember> members;
    std::vector<BrowserEntry> browser;
    std::deque<ChatLine> chat;

    std::unique_ptr<DiscoverySocket> discovery_socket;
    std::unordered_map<std::uint32_t, std::string> peer_names;
    std::unordered_map<std::uint32_t, bool> peer_ready;
};

inline State& _local_state() { static State s; return s; }
inline State*& _state_ptr() { static State* p = nullptr; return p; }
inline void bind_state(State* host) { _state_ptr() = host; }
inline State& _state() { return _state_ptr() ? *_state_ptr() : _local_state(); }

inline bool IsOpen() { return _state().hosting || _state().connected || _state().visible; }
inline bool IsHosting() { return _state().hosting; }
inline bool IsConnected() { return _state().connected; }
inline bool InLobby() { return _state().in_lobby; }
inline bool InMatch() { return _state().in_match; }
inline bool LocalReady() { return _state().ready; }
inline std::uint16_t Port() { return _state().port; }
inline const std::vector<LobbyMember>& Members() { return _state().members; }
inline const std::vector<BrowserEntry>& Browser() { return _state().browser; }
inline const std::deque<ChatLine>& Chat() { return _state().chat; }
inline const std::string& LastError() { return _state().last_error; }
inline bool QuickMatchActive() { return _state().quickmatch_active; }
inline bool HasPendingSceneRequest() { return _state().pending_scene_valid; }
inline const std::string& PendingSceneRequest() { return _state().pending_scene; }

inline void RefreshBrowser();

inline std::string LobbySummary() {
    auto& s = _state();
    return s.lobby_name + " [" + std::to_string((int)s.members.size()) + "/" + std::to_string(s.max_players) + "]";
}

inline std::string _timestamp() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

inline std::uint64_t _now_ms() { return net::detail::now_ms(); }

inline std::string _active_project_name() {
    return _state().project_name;
}

inline std::vector<std::filesystem::path> _project_roots() {
    namespace fs = std::filesystem;
    std::vector<fs::path> roots;
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    std::string project = _active_project_name();

    auto add_root = [&](const fs::path& p) {
        if (p.empty()) return;
        const std::string norm = p.lexically_normal().generic_string();
        for (const auto& existing : roots) {
            if (existing.lexically_normal().generic_string() == norm) return;
        }
        roots.push_back(p);
    };

    if (!project.empty() && !ec) {
        for (fs::path cur = cwd; !cur.empty(); cur = cur.parent_path()) {
            add_root(cur / "games" / project);
            add_root(cur / "export" / project);
            add_root(cur / project);
            if (cur == cur.parent_path()) break;
        }
    }

    if (!ec && !cwd.empty()) add_root(cwd);
    return roots;
}

inline std::string NormalizeMapName(const std::string& raw) {
    namespace fs = std::filesystem;
    if (raw.empty()) return {};
    fs::path scene(raw);
    if (scene.is_absolute()) {
        std::error_code ec;
        for (const auto& root : _project_roots()) {
            fs::path rel = fs::relative(scene, root, ec);
            if (!ec) {
                auto rels = rel.lexically_normal().generic_string();
                if (!rels.empty() && rels.rfind("..", 0) != 0) return rels;
            }
        }
        return scene.lexically_normal().generic_string();
    }
    return scene.lexically_normal().generic_string();
}

inline std::string ResolveMapName(const std::string& raw) {
    namespace fs = std::filesystem;
    if (raw.empty()) return {};
    fs::path scene(raw);
    if (scene.is_absolute()) return scene.lexically_normal().generic_string();

    std::error_code ec;
    std::vector<fs::path> candidates;
    fs::path cwd = fs::current_path(ec);
    const std::string project = _active_project_name();

    if (!project.empty()) {
        for (const auto& root : _project_roots()) {
            candidates.push_back(root / scene);
        }
    }
    if (!ec && !cwd.empty()) candidates.push_back(cwd / scene);
    candidates.push_back(scene);

    for (const auto& candidate : candidates) {
        if (fs::exists(candidate, ec) && fs::is_regular_file(candidate, ec))
            return candidate.lexically_normal().generic_string();
    }

    if (!candidates.empty()) return candidates.front().lexically_normal().generic_string();
    return scene.lexically_normal().generic_string();
}

inline std::string _make_join_code(std::uint64_t id) {
    static const char* digits = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    if (id == 0) return "00000000";
    std::string out;
    while (id > 0) {
        out.push_back(digits[id % 32]);
        id /= 32;
    }
    while (out.size() < 8) out.push_back('0');
    std::reverse(out.begin(), out.end());
    return out;
}

inline Entity _member_to_entity(const LobbyMember& m) {
    Entity e = Entity::object();
    e["peer_id"] = (int)m.peer_id;
    e["name"] = m.name;
    e["ready"] = m.ready;
    e["host"] = m.host;
    e["ping_ms"] = (int)m.ping_ms;
    return e;
}

inline LobbyMember _member_from_entity(const Entity& e) {
    LobbyMember m;
    m.peer_id = (std::uint32_t)e.value("peer_id", 0);
    m.name = e.value("name", std::string("Player"));
    m.ready = e.value("ready", false);
    m.host = e.value("host", false);
    m.ping_ms = (std::uint32_t)e.value("ping_ms", 0);
    return m;
}

inline Entity _lobby_snapshot() {
    auto& s = _state();
    Entity payload = Entity::object();
    payload["lobby_id"] = (double)s.lobby_id;
    payload["lobby_name"] = s.lobby_name;
    payload["player_name"] = s.player_name;
    payload["map_name"] = NormalizeMapName(s.map_name);
    payload["mode_name"] = s.mode_name;
    payload["region"] = s.region;
    payload["build_version"] = s.build_version;
    payload["address"] = s.address;
    payload["port"] = (int)s.port;
    payload["public_lobby"] = s.public_lobby;
    payload["password_required"] = s.password_required;
    payload["auto_start"] = s.auto_start;
    payload["lan_discovery"] = s.lan_discovery;
    payload["in_match"] = s.in_match;
    payload["max_players"] = s.max_players;
    payload["join_code"] = s.join_code;
    payload["local_ready"] = s.ready;

    Entity tags = Entity::array();
    for (const auto& t : s.browser.empty() ? std::vector<std::string>{} : s.browser.front().tags) tags.push_back(t);
    if (tags.size() > 0) payload["tags"] = tags;

    Entity members = Entity::array();
    for (auto& m : s.members) {
        // Host's own entry (peer_id 0) has no transport RTT to itself;
        // every other member's ping is read live from the heartbeat.
        if (s.hosting && m.peer_id != 0) m.ping_ms = Network::GetPeerRttMs(m.peer_id);
        members.push_back(_member_to_entity(m));
    }
    payload["members"] = members;

    Entity chat = Entity::array();
    for (const auto& line : s.chat) {
        Entity row = Entity::object();
        row["timestamp"] = line.timestamp;
        row["sender"] = line.sender;
        row["message"] = line.message;
        chat.push_back(row);
    }
    payload["chat"] = chat;
    return payload;
}

inline void _set_browser_entry(BrowserEntry& b, const Entity& e) {
    b.address = e.value("address", b.address);
    b.port = (std::uint16_t)e.value("port", (int)b.port);
    b.lobby_name = e.value("lobby_name", b.lobby_name);
    b.player_name = e.value("player_name", b.player_name);
    b.map_name = NormalizeMapName(e.value("map_name", b.map_name));
    b.mode_name = e.value("mode_name", b.mode_name);
    b.region = e.value("region", b.region);
    b.build_version = e.value("build_version", b.build_version);
    b.players = e.value("players", b.players);
    b.max_players = e.value("max_players", b.max_players);
    b.public_lobby = e.value("public_lobby", b.public_lobby);
    b.password_required = e.value("password_required", b.password_required);
    b.auto_start = e.value("auto_start", b.auto_start);
    b.in_match = e.value("in_match", b.in_match);
    b.ping_ms = e.value("ping_ms", b.ping_ms);
    b.last_seen_ms = _now_ms();
    b.tags.clear();
    if (e.contains("tags") && e["tags"].is_array()) {
        for (const auto& t : e["tags"]) if (t.is_string()) b.tags.push_back(t.get<std::string>());
    }
}

inline Entity _browser_to_entity(const BrowserEntry& b) {
    Entity e = Entity::object();
    e["address"] = b.address;
    e["port"] = (int)b.port;
    e["lobby_name"] = b.lobby_name;
    e["player_name"] = b.player_name;
    e["map_name"] = b.map_name;
    e["mode_name"] = b.mode_name;
    e["region"] = b.region;
    e["build_version"] = b.build_version;
    e["players"] = b.players;
    e["max_players"] = b.max_players;
    e["public_lobby"] = b.public_lobby;
    e["password_required"] = b.password_required;
    e["auto_start"] = b.auto_start;
    e["in_match"] = b.in_match;
    e["ping_ms"] = b.ping_ms;
    Entity tags = Entity::array();
    for (const auto& t : b.tags) tags.push_back(t);
    e["tags"] = tags;
    return e;
}

inline BrowserEntry* _find_browser(const std::string& address, std::uint16_t port) {
    auto& s = _state();
    for (auto& b : s.browser) if (b.address == address && b.port == port) return &b;
    return nullptr;
}

inline LobbyMember* _find_member(std::uint32_t peer_id) {
    auto& s = _state();
    for (auto& m : s.members) if (m.peer_id == peer_id) return &m;
    return nullptr;
}

inline void _push_chat(const std::string& sender, const std::string& message) {
    auto& s = _state();
    s.chat.push_back(ChatLine{_timestamp(), sender, message});
    while (s.chat.size() > 128) s.chat.pop_front();
}

inline void _broadcast_lobby_event(const std::string& event_name, const Entity& payload, bool reliable = true) {
    Network::SendEvent(event_name, payload, reliable);
}

inline void _sync_to_peer(std::uint32_t peer_id) {
    auto snap = _lobby_snapshot();
    Network::SendEventTo(peer_id, "lobby_sync", snap, true);
}

inline void _broadcast_sync() {
    _broadcast_lobby_event("lobby_sync", _lobby_snapshot(), true);
}

inline void _queue_scene_load(const std::string& scene_path) {
    auto& s = _state();
    if (scene_path.empty()) return;
    s.pending_scene = scene_path;
    s.pending_scene_valid = true;
}

inline void _deliver_pending_scene() {
    auto& s = _state();
    if (!s.pending_scene_valid) return;
    if (!SceneManager::HasLoadSceneHandler()) return;
    std::string scene = s.pending_scene;
    s.pending_scene.clear();
    s.pending_scene_valid = false;
    SceneManager::LoadScene(scene);
}

inline void _mark_local_member(bool ready) {
    auto& s = _state();
    s.ready = ready;
    if (auto* m = _find_member(Network::LocalPeerId() ? Network::LocalPeerId() : 0)) {
        m->ready = ready;
    }
}

inline void _reset_members_for_host() {
    auto& s = _state();
    s.members.clear();
    s.peer_names.clear();
    s.peer_ready.clear();
    s.members.push_back(LobbyMember{0, s.player_name, true, true, 0});
    s.ready = true;
    s.in_lobby = true;
    s.in_match = false;
    Network::SetInMatch(false);
    s.join_code = _make_join_code(s.lobby_id);
}

// Detects the local IP address that would be used to reach the LAN/internet
// (not 127.0.0.1). Uses the classic "connect a UDP socket, then read back
// getsockname()" trick: for UDP, connect() never actually sends a packet,
// it just asks the OS routing table to pick an outbound interface/source
// address for that destination — which is exactly the address other
// machines on the LAN would need to use to reach us back. Falls back to
// 127.0.0.1 (same-machine only) if detection fails for any reason, e.g. no
// network interface at all.
//
// This exists because Host() previously never set s.address to anything
// real, so every lobby advertisement told a joining player to connect to
// 127.0.0.1 — which is always the JOINER's own loopback, never the host.
// On the same machine that accidentally "worked" (loopback = correct);
// across two different machines it silently could never connect, the
// 5-second join timeout fired, and the joiner fell back to hosting their
// own separate lobby — which is exactly "both players end up in separate
// matches".
inline std::string DetectLocalIp() {
    net_socket_t sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == net_invalid_socket) return "127.0.0.1";

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(53); // arbitrary; nothing is actually sent for UDP connect()
    // A public IP is used as the destination purely to make the OS pick a
    // real outbound interface (not loopback). No packet is sent to it.
    if (inet_pton(AF_INET, "8.8.8.8", &target.sin_addr) != 1) {
#if defined(_WIN32)
        closesocket(sock);
#else
        ::close(sock);
#endif
        return "127.0.0.1";
    }

    if (::connect(sock, reinterpret_cast<sockaddr*>(&target), sizeof(target)) != 0) {
#if defined(_WIN32)
        closesocket(sock);
#else
        ::close(sock);
#endif
        return "127.0.0.1";
    }

    sockaddr_in local{};
    socklen_t local_len = sizeof(local);
    std::string result = "127.0.0.1";
    if (::getsockname(sock, reinterpret_cast<sockaddr*>(&local), &local_len) == 0) {
        char buf[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf))) {
            std::string addr(buf);
            if (!addr.empty() && addr != "0.0.0.0") result = addr;
        }
    }
#if defined(_WIN32)
    closesocket(sock);
#else
    ::close(sock);
#endif
    return result;
}

inline void Host(const std::string& lobby_name, const std::string& player_name, std::uint16_t port, int max_players,
                 const std::string& mode_name = "Casual", const std::string& map_name = "", bool public_lobby = true,
                 bool password_required = false, const std::string& password = "", const std::string& region = "LAN",
                 bool auto_start = false, bool lan_discovery = true, const std::string& build_version = "1.0") {
    auto& s = _state();
    Network::Shutdown();
    s.hosting = true;
    s.connected = false;
    s.in_lobby = true;
    s.in_match = false;
    Network::SetInMatch(false);
    s.public_lobby = public_lobby;
    s.password_required = password_required;
    s.auto_start = auto_start;
    s.lan_discovery = lan_discovery;
    s.ready = true;
    s.port = port;
    s.max_players = std::max(1, max_players);
    s.lobby_name = lobby_name.empty() ? std::string("New Lobby") : lobby_name;
    s.player_name = player_name.empty() ? std::string("Host") : player_name;
    s.mode_name = mode_name.empty() ? std::string("Casual") : mode_name;
    s.map_name = NormalizeMapName(map_name);
    s.region = region.empty() ? std::string("LAN") : region;
    s.build_version = build_version.empty() ? std::string("1.0") : build_version;
    s.password = password;
    s.last_error.clear();
    s.address = DetectLocalIp();
    s.quickmatch_active = false;
    s.pending_scene_valid = false;
    s.pending_scene.clear();
    s.lobby_id = (std::uint64_t)_now_ms() ^ ((std::uint64_t)port << 32) ^ (std::uint64_t)s.max_players;
    s.join_code = _make_join_code(s.lobby_id);
    try {
        Network::Host(port, s.max_players);
    } catch (const std::exception& e) {
        s.hosting = false;
        s.in_lobby = false;
        s.last_error = std::string("Failed to host lobby: ") + e.what() +
                       " (port " + std::to_string(port) + " may already be in use)";
        _push_chat("System", s.last_error);
        return;
    }
    _reset_members_for_host();
    _broadcast_sync();
    _push_chat("System", "Hosted lobby '" + s.lobby_name + "'.");
}

inline void Join(const std::string& address, std::uint16_t port, const std::string& player_name = "Player") {
    auto& s = _state();
    Network::Shutdown();
    s.hosting = false;
    s.connected = true;
    s.join_confirmed = false;
    s.join_started_ms = _now_ms();
    s.in_lobby = true;
    s.in_match = false;
    Network::SetInMatch(false);
    s.ready = false;
    s.address = address;
    s.port = port;
    s.player_name = player_name.empty() ? std::string("Player") : player_name;
    s.last_error.clear();
    s.quickmatch_active = false;
    s.pending_scene_valid = false;
    s.pending_scene.clear();
    Network::Connect(address, port);
    s.members.clear();
    s.members.push_back(LobbyMember{0, s.player_name, false, false, 0});
    _push_chat("System", "Connecting to " + address + ":" + std::to_string(port) + "...");
}

inline void Leave() {
    auto& s = _state();
    if (s.hosting || s.connected) {
        Entity payload = Entity::object();
        payload["player_name"] = s.player_name;
        _broadcast_lobby_event("lobby_leave", payload, true);
    }
    Network::Shutdown();
    s.hosting = false;
    s.connected = false;
    s.in_lobby = false;
    s.in_match = false;
    Network::SetInMatch(false);
    s.ready = false;
    s.members.clear();
    s.peer_names.clear();
    s.peer_ready.clear();
    s.quickmatch_active = false;
    s.pending_scene_valid = false;
    s.pending_scene.clear();
    s.quickmatch_mode_filter.clear();
    s.quickmatch_map_filter.clear();
    s.quickmatch_started_ms = 0;
    s.quickmatch_party_size = 1;
}

inline void SetPlayerName(const std::string& name) { _state().player_name = name.empty() ? std::string("Player") : name; }
inline void SetLobbyName(const std::string& name) { _state().lobby_name = name.empty() ? std::string("Lobby") : name; }
inline void SetMapName(const std::string& name) { _state().map_name = NormalizeMapName(name); }
inline void SetPlayerPrefabPath(const std::string& path) { _state().player_prefab_path = path; }
inline std::string GetPlayerPrefabPath() { return _state().player_prefab_path; }
inline void SetProjectName(const std::string& name) { _state().project_name = name; }
inline void SetModeName(const std::string& name) { _state().mode_name = name.empty() ? std::string("Casual") : name; }
inline void SetRegion(const std::string& name) { _state().region = name.empty() ? std::string("LAN") : name; }
inline void SetBuildVersion(const std::string& v) { _state().build_version = v.empty() ? std::string("1.0") : v; }
inline void SetPassword(const std::string& password) { _state().password = password; _state().password_required = !password.empty(); }
inline void SetPublicLobby(bool v) { _state().public_lobby = v; }
inline void SetAutoStart(bool v) { _state().auto_start = v; }
inline void SetLanDiscovery(bool v) { _state().lan_discovery = v; }
inline void SetMaxPlayers(int v) { _state().max_players = std::max(1, v); }
inline void SetPort(std::uint16_t v) { _state().port = v; }
inline void SetDiscoveryPort(std::uint16_t v) { _state().discovery_port = v; }

inline void SetReady(bool ready) {
    auto& s = _state();
    s.ready = ready;
    if (s.hosting) {
        if (auto* m = _find_member(0)) m->ready = ready;
        _broadcast_sync();
    } else if (s.connected) {
        Entity payload = Entity::object();
        payload["ready"] = ready;
        payload["player_name"] = s.player_name;
        _broadcast_lobby_event("lobby_ready", payload, true);
    }
}

inline void SendChat(const std::string& message) {
    if (message.empty()) return;
    auto& s = _state();
    Entity payload = Entity::object();
    payload["player_name"] = s.player_name;
    payload["message"] = message;
    if (s.hosting) {
        _push_chat(s.player_name, message);
        _broadcast_lobby_event("lobby_chat", payload, true);
        _broadcast_sync();
    } else if (s.connected) {
        _broadcast_lobby_event("lobby_chat", payload, true);
    } else {
        _push_chat(s.player_name, message);
    }
}

inline void StartMatch() {
    auto& s = _state();
    if (!s.hosting) return;
    s.in_match = true;
    Network::SetInMatch(true);
    s.quickmatch_active = false;
    Entity payload = Entity::object();
    payload["lobby_name"] = s.lobby_name;
    payload["map_name"] = NormalizeMapName(s.map_name);
    payload["mode_name"] = s.mode_name;
    payload["join_code"] = s.join_code;
    _broadcast_lobby_event("lobby_start", payload, true);
    _broadcast_sync();
    _push_chat("System", "Match started.");
    if (!s.map_name.empty()) {
        _queue_scene_load(ResolveMapName(s.map_name));
        // BUG FIX: Do NOT call _deliver_pending_scene() here.
        //
        // In the editor, SetLoadSceneHandler() hasn't been installed yet at
        // the moment StartMatch() is called — start_play() is triggered by
        // the editor_main loop checking InMatch() on the NEXT frame.
        // Calling _deliver_pending_scene() here silently drops the scene
        // load (HasLoadSceneHandler() == false) AND clears pending_scene_valid,
        // so the editor's HasPendingSceneRequest() check also returns false,
        // start_play() is never triggered, and the HOST stays on the lobby
        // scene while clients load the match scene. Every player ends up in
        // a different scene — the root cause of the "separate matches" bug.
        //
        // The deferred _deliver_pending_scene() call at the bottom of
        // Update() handles this correctly: by the time Update() runs next
        // frame the editor has called start_play() → SetLoadSceneHandler()
        // and the deliver succeeds. For the standalone runtime (core.cpp)
        // the handler is already installed before the game loop starts, so
        // the Update()-path also works there — no change needed.
    }
    // Player spawning happens later, once each machine's own scene swap has
    // actually finished loading — see SpawnAllPlayers(), called from the
    // editor/runtime right after the deferred scene load completes. Doing
    // it here would race the spawn against an async scene swap (LoadScene's
    // handler just queues the switch; the real swap happens later in the
    // frame loop), so a client could try to instantiate into the old scene.
}

inline void _upsert_browser(const Entity& e) {
    auto& s = _state();
    std::string address = e.value("address", std::string("127.0.0.1"));
    std::uint16_t port = (std::uint16_t)e.value("port", 0);
    if (address.empty() || port == 0) return;
    BrowserEntry* b = _find_browser(address, port);
    if (!b) {
        s.browser.push_back(BrowserEntry{});
        b = &s.browser.back();
        b->address = address;
        b->port = port;
    }
    _set_browser_entry(*b, e);
}

inline void _cleanup_browser() {
    auto& s = _state();
    const std::uint64_t now = _now_ms();
    s.browser.erase(std::remove_if(s.browser.begin(), s.browser.end(), [&](const BrowserEntry& b) {
        return (now - b.last_seen_ms) > 6000ULL;
    }), s.browser.end());
    s.last_browser_cleanup_ms = now;
}

inline int ScoreLobby(const BrowserEntry& b, const std::string& mode_filter = std::string(), const std::string& map_filter = std::string(), int desired_party_size = 1) {
    if (!b.public_lobby) return -10000;
    int score = 0;
    if (!mode_filter.empty() && b.mode_name == mode_filter) score += 600;
    if (!map_filter.empty() && b.map_name == map_filter) score += 350;
    if (!mode_filter.empty() && b.mode_name != mode_filter) score -= 120;
    if (!map_filter.empty() && b.map_name != map_filter) score -= 80;
    score += std::max(0, 8 - b.ping_ms);
    score += std::max(0, b.max_players - b.players) * 8;
    score += std::max(0, desired_party_size - (b.max_players - b.players)) * 50;
    if (b.auto_start) score += 40;
    if (b.in_match) score -= 100;
    if (b.password_required) score -= 25;
    return score;
}

inline int FindBestLobbyIndex(const std::string& mode_filter = std::string(), const std::string& map_filter = std::string(), int desired_party_size = 1) {
    auto& s = _state();
    int best_idx = -1;
    int best_score = -1000000;
    for (int i = 0; i < (int)s.browser.size(); ++i) {
        int score = ScoreLobby(s.browser[(std::size_t)i], mode_filter, map_filter, desired_party_size);
        if (score > best_score) { best_score = score; best_idx = i; }
    }
    return best_idx;
}

inline bool _attempt_quickmatch_join() {
    auto& s = _state();
    int idx = FindBestLobbyIndex(s.quickmatch_mode_filter, s.quickmatch_map_filter, s.quickmatch_party_size);
    if (idx < 0 || idx >= (int)s.browser.size()) return false;
    const auto lobby = s.browser[(std::size_t)idx];
    s.quickmatch_active = false;
    Join(lobby.address, lobby.port, s.player_name);
    return true;
}

inline void QuickMatch(const std::string& mode_filter = std::string(), const std::string& map_filter = std::string(), int desired_party_size = 1) {
    auto& s = _state();
    s.quickmatch_active = true;
    s.quickmatch_started_ms = _now_ms();
    s.quickmatch_mode_filter = mode_filter;
    s.quickmatch_map_filter = map_filter;
    s.quickmatch_party_size = std::max(1, desired_party_size);
    s.last_error.clear();
    RefreshBrowser();
    if (_attempt_quickmatch_join()) return;
    _push_chat("System", "Searching for a lobby...");
}

// Discovery helper socket used only internally by Update(). It is kept here so
// the header remains self-contained and can be shared by the editor, the
// standalone engine, and scripts compiled into a hot-reloaded DLL.
class DiscoverySocket {
public:
    DiscoverySocket() = default;
    ~DiscoverySocket() { close(); }

    bool is_listening_on(std::uint16_t port) const { return _listening && _sock != net_invalid_socket && _port == port; }
    bool is_sending() const { return _sending && _sock != net_invalid_socket; }

    void open_listener(std::uint16_t port) {
        if (is_listening_on(port)) return; // already bound; don't tear down a working socket every frame
        close();
        open_socket();
        _listening = true;
        _sending = false;
        _port = port;
        _reuse();
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        if (::bind(_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close();
            throw std::runtime_error("DiscoverySocket::open_listener bind failed");
        }
    }

    void open_sender() {
        if (is_sending()) return; // already open; avoid needless close/reopen churn every frame
        close();
        open_socket();
        _listening = false;
        _sending = true;
        _reuse();
        _broadcast();
    }

    void close() {
        if (_sock == net_invalid_socket) return;
#if defined(_WIN32)
        closesocket(_sock);
#else
        ::close(_sock);
#endif
        _sock = net_invalid_socket;
    }

    void send_broadcast(const std::uint8_t* data, std::size_t len, std::uint16_t port, bool include_loopback = true) {
        if (_sock == net_invalid_socket || !data || !len) return;
        auto send_to = [&](const char* host) {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) return;
            ::sendto(_sock, reinterpret_cast<const char*>(data), (int)len, 0, reinterpret_cast<sockaddr*>(&addr), (int)sizeof(addr));
        };
        send_to("255.255.255.255");
        if (include_loopback) send_to("127.0.0.1");
    }

    template <class Fn>
    void poll(Fn&& on_recv) {
        if (_sock == net_invalid_socket) return;
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(_sock, &readfds);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        int ready = select((int)(_sock + 1), &readfds, nullptr, nullptr, &tv);
        if (ready <= 0) return;
        while (true) {
            std::array<std::uint8_t, 1600> buffer{};
            sockaddr_storage from{};
            socklen_t from_len = sizeof(from);
            int received = (int)recvfrom(_sock, reinterpret_cast<char*>(buffer.data()), (int)buffer.size(), 0,
                                         reinterpret_cast<sockaddr*>(&from), &from_len);
            if (received <= 0) break;
            on_recv(from, buffer.data(), (std::size_t)received);
        }
    }

private:
    net_socket_t _sock = net_invalid_socket;
    bool _listening = false;
    bool _sending = false;
    std::uint16_t _port = 0;

    void open_socket() {
#if defined(_WIN32)
        static bool wsa_ok = false;
        if (!wsa_ok) {
            WSADATA wsa{};
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) throw std::runtime_error("DiscoverySocket: WSAStartup failed");
            wsa_ok = true;
        }
#endif
        _sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (_sock == net_invalid_socket) throw std::runtime_error("DiscoverySocket: socket() failed");
        int yes = 1;
        ::setsockopt(_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, (int)sizeof(yes));
#if defined(SO_BROADCAST)
        ::setsockopt(_sock, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, (int)sizeof(yes));
#endif
#if defined(_WIN32)
        u_long mode = 1;
        ioctlsocket(_sock, FIONBIO, &mode);
#else
        int flags = fcntl(_sock, F_GETFL, 0);
        if (flags >= 0) fcntl(_sock, F_SETFL, flags | O_NONBLOCK);
#endif
    }

    void _reuse() {
        int yes = 1;
        ::setsockopt(_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, (int)sizeof(yes));
    }

    void _broadcast() {
        int yes = 1;
        ::setsockopt(_sock, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, (int)sizeof(yes));
    }
};

inline DiscoverySocket& _discovery_sender() {
    static DiscoverySocket s;
    return s;
}
inline DiscoverySocket& _discovery_listener() {
    static DiscoverySocket s;
    return s;
}

inline void _open_discovery_listener() {
    auto& s = _state();
    if (!s.lan_discovery) return;
    try {
        _discovery_listener().open_listener(s.discovery_port);
    } catch (...) {}
}

inline void _close_discovery_listener() {
    _discovery_listener().close();
}

inline void _broadcast_advertisement() { /* advertisements are broadcast from Update() */ }

inline void _ensure_discovery_sockets() {
    auto& s = _state();
    if (s.hosting) {
        try {
            _discovery_sender().open_sender();
        } catch (...) {}
    }
    if (s.lan_discovery && (s.hosting || s.connected || s.quickmatch_active || s.visible)) {
        try {
            _discovery_listener().open_listener(s.discovery_port);
        } catch (...) {}
    }
}

inline void _refresh_browser_from_network() {
    auto& s = _state();
    if (!s.lan_discovery) return;
    DiscoverySocket& listener = _discovery_listener();
    listener.poll([&](const sockaddr_storage&, const std::uint8_t* data, std::size_t len) {
        if (!data || len < 9) return;
        if (!(data[0] == 'G' && data[1] == 'E' && data[2] == '2' && data[3] == 'D' && data[4] == '-' && data[5] == 'L' && data[6] == 'B')) return;
        std::size_t off = 8;
        if (len < off + 4) return;
        auto read_u32 = [&](const std::uint8_t* p) -> std::uint32_t {
            return (std::uint32_t)p[0] | ((std::uint32_t)p[1] << 8) | ((std::uint32_t)p[2] << 16) | ((std::uint32_t)p[3] << 24);
        };
        std::uint32_t payload_len = read_u32(data + off); off += 4;
        if (off + payload_len > len) return;
        try {
            auto j = nlohmann::json::parse(std::string((const char*)data + off, payload_len));
            Entity e(j);
            _upsert_browser(e);
        } catch (...) {}
    });
    _cleanup_browser();
}

inline void RefreshBrowser() {
    _open_discovery_listener();
    _refresh_browser_from_network();
}

inline void HandleNetworkEvent(const Network::PendingEvent& ev) {
    auto& s = _state();
    if (ev.name == "peer_connected") {
        if (!s.hosting && s.connected) {
            Entity payload = Entity::object();
            payload["player_name"] = s.player_name;
            payload["build_version"] = s.build_version;
            payload["password"] = s.password;
            payload["join_code"] = s.join_code;
            _broadcast_lobby_event("lobby_join_request", payload, true);
        }
        return;
    }
    if (ev.name == "lobby_join_request") {
        if (!s.hosting) return;
        LobbyMember member;
        member.peer_id = ev.from_peer;
        member.name = ev.data.value("player_name", std::string("Player"));
        member.ready = false;
        member.host = false;
        s.peer_names[ev.from_peer] = member.name;
        s.peer_ready[ev.from_peer] = false;
        auto* existing = _find_member(ev.from_peer);
        if (!existing) s.members.push_back(member);
        else *existing = member;
        _sync_to_peer(ev.from_peer);
        _push_chat("System", member.name + " joined the lobby.");
        return;
    }
    if (ev.name == "lobby_ready") {
        if (!s.hosting) return;
        auto* member = _find_member(ev.from_peer);
        if (member) {
            member->ready = ev.data.value("ready", false);
            s.peer_ready[ev.from_peer] = member->ready;
            _broadcast_sync();
            if (s.auto_start) {
                bool all_ready = true;
                for (const auto& m : s.members) if (!m.ready) { all_ready = false; break; }
                if (all_ready) StartMatch();
            }
        }
        return;
    }
    if (ev.name == "lobby_chat") {
        std::string sender = ev.data.value("player_name", std::string("Player"));
        std::string message = ev.data.value("message", std::string());
        if (!message.empty()) _push_chat(sender, message);
        return;
    }
    if (ev.name == "lobby_sync") {
        if (s.hosting) return;
        s.lobby_id = (std::uint64_t)ev.data.value("lobby_id", (double)s.lobby_id);
        s.lobby_name = ev.data.value("lobby_name", s.lobby_name);
        s.player_name = ev.data.value("player_name", s.player_name);
        s.map_name = ev.data.value("map_name", s.map_name);
        s.mode_name = ev.data.value("mode_name", s.mode_name);
        s.region = ev.data.value("region", s.region);
        s.build_version = ev.data.value("build_version", s.build_version);
        s.address = ev.data.value("address", s.address);
        s.port = (std::uint16_t)ev.data.value("port", (int)s.port);
        s.public_lobby = ev.data.value("public_lobby", s.public_lobby);
        s.password_required = ev.data.value("password_required", s.password_required);
        s.auto_start = ev.data.value("auto_start", s.auto_start);
        s.lan_discovery = ev.data.value("lan_discovery", s.lan_discovery);
        s.in_match = ev.data.value("in_match", s.in_match);
        Network::SetInMatch(s.in_match);
        s.max_players = ev.data.value("max_players", s.max_players);
        s.join_code = ev.data.value("join_code", s.join_code);
        s.ready = ev.data.value("local_ready", s.ready);
        s.in_lobby = true;
        s.connected = true;
        s.join_confirmed = true;
        s.members.clear();
        if (ev.data.contains("members") && ev.data["members"].is_array()) {
            for (const auto& m : ev.data["members"]) s.members.push_back(_member_from_entity(Entity(m)));
        }
        if (s.members.empty()) s.members.push_back(LobbyMember{0, s.player_name, s.ready, false, 0});
        if (ev.data.contains("chat") && ev.data["chat"].is_array()) {
            s.chat.clear();
            for (const auto& row : ev.data["chat"]) {
                ChatLine line;
                line.timestamp = row.value("timestamp", std::string());
                line.sender = row.value("sender", std::string());
                line.message = row.value("message", std::string());
                s.chat.push_back(std::move(line));
            }
        }
        return;
    }
    if (ev.name == "lobby_start") {
        s.in_match = true;
        Network::SetInMatch(true);
        s.in_lobby = true;
        s.quickmatch_active = false;
        _push_chat("System", "Match started.");
        std::string map = ev.data.value("map_name", s.map_name);
        if (!map.empty()) {
            s.map_name = NormalizeMapName(map);
            _queue_scene_load(ResolveMapName(map));
            // BUG FIX: Do NOT call _deliver_pending_scene() here (same
            // reason as StartMatch above). On clients, start_play() also
            // hasn't run yet when this event arrives, so the scene load
            // handler isn't installed. Dropping the delivery here clears
            // pending_scene_valid and the client never loads the match scene.
            // The Update() path at the bottom of this function delivers it
            // correctly on the next frame, after the editor's InMatch() /
            // HasPendingSceneRequest() check has triggered start_play().
        }
        return;
    }
    if (ev.name == "lobby_leave") {
        std::string name = ev.data.value("player_name", std::string("Player"));
        if (s.hosting) {
            s.members.erase(std::remove_if(s.members.begin(), s.members.end(),
                [&](const LobbyMember& m){ return m.peer_id == ev.from_peer; }), s.members.end());
            s.peer_names.erase(ev.from_peer);
            s.peer_ready.erase(ev.from_peer);
            _broadcast_sync();
        }
        _push_chat("System", name + " left the lobby.");
        return;
    }
    if (ev.name == "peer_disconnected") {
        // Fired by the transport heartbeat (see UdpTransport::poll) when a
        // peer hasn't sent anything at all (ping, data, ack) for several
        // seconds — i.e. it vanished without a clean lobby_leave (crash,
        // force-quit, network drop), not a normal voluntary leave.
        bool was_server = ev.data.value("was_server", false);
        if (s.hosting) {
            auto* member = _find_member(ev.from_peer);
            std::string name = member ? member->name : std::string("A player");
            s.members.erase(std::remove_if(s.members.begin(), s.members.end(),
                [&](const LobbyMember& m){ return m.peer_id == ev.from_peer; }), s.members.end());
            s.peer_names.erase(ev.from_peer);
            s.peer_ready.erase(ev.from_peer);
            _broadcast_sync();
            _push_chat("System", name + " disconnected (connection lost).");
            return;
        }
        if (was_server && (s.connected || s.in_lobby)) {
            // We lost the host entirely. There's nothing left to talk to,
            // so don't bother sending a lobby_leave (would just go nowhere)
            // — exit straight to a clean, retryable state like Leave() does.
            s.last_error = "Lost connection to the host.";
            Network::Shutdown();
            s.hosting = false;
            s.connected = false;
            s.in_lobby = false;
            s.in_match = false;
            Network::SetInMatch(false);
            s.ready = false;
            s.join_confirmed = false;
            s.members.clear();
            s.peer_names.clear();
            s.peer_ready.clear();
            s.quickmatch_active = false;
            s.pending_scene_valid = false;
            s.pending_scene.clear();
            _push_chat("System", s.last_error);
        }
        return;
    }
}

inline void Update(float dt) {
    auto& s = _state();
    (void)dt;

    // A join attempt that never gets a lobby_sync back from the host (host
    // unreachable, firewalled, or already gone) would otherwise sit in
    // s.connected = true forever — Join() sets that optimistically before
    // the handshake actually completes, and nothing else ever clears it.
    // Time it out so the player gets a clear error and a clean state to
    // retry from, instead of an editor that just looks "connected" to
    // nothing.
    if (s.connected && !s.join_confirmed && !s.hosting) {
        const std::uint64_t now = _now_ms();
        if (now - s.join_started_ms >= 5000ULL) {
            std::string addr = s.address;
            std::uint16_t port = s.port;
            Leave();
            s.last_error = "Could not connect to " + addr + ":" + std::to_string(port) + " (no response).";
            _push_chat("System", s.last_error);
            return;
        }
    }

    if (s.quickmatch_active) {
        RefreshBrowser();
        if (_attempt_quickmatch_join()) return;
        const std::uint64_t now = _now_ms();
        if (now - s.quickmatch_started_ms >= 5000ULL) {
            s.quickmatch_active = false;
            _push_chat("System", "No lobby found. Hosting your own lobby...");
            Host(s.lobby_name, s.player_name, s.port, s.max_players, s.mode_name, s.map_name, s.public_lobby,
                 s.password_required, s.password, s.region, s.auto_start, s.lan_discovery, s.build_version);
            return;
        }
    }

    // Once a match has started there's no reason to keep polling/advertising
    // over LAN discovery — you're not searching for a lobby anymore, and a
    // started match shouldn't show up as joinable in anyone else's browser
    // either. Leaving this running for the whole match was doing two extra
    // non-blocking socket polls (and, for the host, a broadcast send) every
    // single frame for no purpose, which is wasted work on top of the main
    // game transport's own per-frame poll.
    // Throttle LAN browser polling to 10 Hz — running it every frame
    // (potentially 60+ times/sec) polls a UDP socket and rebuilds the
    // browser list for no benefit since lobbies broadcast once per second.
    if (!s.in_match && (s.hosting || s.connected || s.quickmatch_active || s.visible)) {
        const std::uint64_t now_ms = _now_ms();
        if (now_ms - s.last_browser_poll_ms >= 100ULL) {
            s.last_browser_poll_ms = now_ms;
            _ensure_discovery_sockets();
            _refresh_browser_from_network();
        }
    }

    if (s.hosting && s.lan_discovery && !s.in_match) {
        const std::uint64_t now = _now_ms();
        if (now - s.last_advertise_ms >= 1000ULL) {
            s.last_advertise_ms = now;
            Entity e = Entity::object();
            e["address"] = s.address.empty() ? std::string("127.0.0.1") : s.address;
            e["port"] = (int)s.port;
            e["lobby_name"] = s.lobby_name;
            e["player_name"] = s.player_name;
            e["map_name"] = NormalizeMapName(s.map_name);
            e["mode_name"] = s.mode_name;
            e["region"] = s.region;
            e["build_version"] = s.build_version;
            e["players"] = (int)s.members.size();
            e["max_players"] = s.max_players;
            e["public_lobby"] = s.public_lobby;
            e["password_required"] = s.password_required;
            e["auto_start"] = s.auto_start;
            e["in_match"] = s.in_match;
            e["ping_ms"] = 0;
            e["join_code"] = s.join_code;
            Entity tags = Entity::array();
            tags.push_back(s.mode_name);
            if (!s.region.empty()) tags.push_back(s.region);
            e["tags"] = tags;
            auto payload = nlohmann::json(e).dump();
            std::vector<std::uint8_t> bytes;
            bytes.reserve(8 + 4 + payload.size());
            bytes.insert(bytes.end(), {'G','E','2','D','-','L','B',1});
            std::uint32_t len = (std::uint32_t)payload.size();
            for (int i = 0; i < 4; ++i) bytes.push_back((std::uint8_t)((len >> (i * 8)) & 0xFF));
            bytes.insert(bytes.end(), payload.begin(), payload.end());
            DiscoverySocket& sender = _discovery_sender();
            sender.send_broadcast(bytes.data(), bytes.size(), s.discovery_port, true);
        }
    }

    _deliver_pending_scene();
}

inline void Close() {
    _close_discovery_listener();
    _discovery_sender().close();
}

} // namespace Matchmaking