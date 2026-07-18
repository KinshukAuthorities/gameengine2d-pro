#pragma once
#include "../../../engine_cpp/script_system.hpp"
#include <algorithm>
#include <string>

namespace AbyssGame {
// Persisted save schema.  Keep this small and explicit: PlayerPrefs is the
// save backend for both the editor and exported game, so a version marker lets
// us repair older demo saves without silently discarding player progress.
inline constexpr int kProfileVersion = 5;

enum class CombatAction { None, Blade, ArcBolt, Parry, FocusArt, Heal };

struct DamagePacket {
    int amount = 1;
    int team = 0;
    float knockback_x = 0.0f;
    float knockback_y = 0.0f;
    bool parryable = true;
    bool pierces = false;
};

struct EnemyBrain {
    string state = "idle";
    float state_time = 0.0f;
    float telegraph_time = 0.0f;
    float stagger_time = 0.0f;
    int phase = 0;
};

struct EncounterSpec {
    string room_id;
    int wave_count = 1;
    int reward_focus = 1;
    bool lock_on_enter = true;
};

struct RoomMapState {
    string room_id;
    bool visited = false;
    bool cleared = false;
    bool checkpoint = false;
};

inline string ProfileKey(string suffix) { return "abyss_" + suffix; }

inline void MigrateProfile() {
    const int version = PlayerPrefs::get_int(ProfileKey("profile_version"), 0);
    // A pre-showcase save used the accidental "doublejump" spelling in a
    // couple of scenes.  Accept either value and write the canonical key.
    if (version < 2) {
        if (PlayerPrefs::get_int(ProfileKey("ability_doublejump"), 0) != 0)
            PlayerPrefs::set_int(ProfileKey("ability_double_jump"), 1);
        if (PlayerPrefs::get_int(ProfileKey("ability_walljump"), 0) != 0)
            PlayerPrefs::set_int(ProfileKey("ability_wall_jump"), 1);
    }
    // Version 3 makes the opening movement kit immediately playable and
    // establishes inventory/map values for existing saves without erasing
    // player progress.
    if (version < 3) {
        // Dash is part of the first-room controls.  A few pre-showcase saves
        // contained an explicit zero here, so checking only has_key() left
        // those players with a dead Shift key.  Version 3 intentionally
        // grants the base dash to every existing profile.
        PlayerPrefs::set_int(ProfileKey("ability_dash"), 1);
        if (!PlayerPrefs::has_key(ProfileKey("relic_case_open")))
            PlayerPrefs::set_int(ProfileKey("relic_case_open"), 0);
        if (!PlayerPrefs::has_key(ProfileKey("relic_blade")))
            PlayerPrefs::set_string(ProfileKey("relic_blade"), "Ember Sigil");
        if (!PlayerPrefs::has_key(ProfileKey("relic_arc")))
            PlayerPrefs::set_string(ProfileKey("relic_arc"), "Prism Core");
        if (!PlayerPrefs::has_key(ProfileKey("relic_mobility")))
            PlayerPrefs::set_string(ProfileKey("relic_mobility"), "Swift Coil");
        if (!PlayerPrefs::has_key(ProfileKey("relic_ward")))
            PlayerPrefs::set_string(ProfileKey("relic_ward"), "Warden Charm");
    }
    // V4 consolidates pause, discovery, and equipment keys under the profile
    // prefix. Older builds left a few of these as loose values; retain them
    // rather than resetting a judge's in-progress campaign.
    if (version < 4) {
        if (PlayerPrefs::get_int("abyss_map_open", 0) != 0)
            PlayerPrefs::set_int(ProfileKey("map_open"), 1);
        if (PlayerPrefs::get_int("abyss_relic_case_open", 0) != 0)
            PlayerPrefs::set_int(ProfileKey("relic_case_open"), 1);
        if (PlayerPrefs::get_int(ProfileKey("ability_blink"), 0) != 0)
            PlayerPrefs::set_int(ProfileKey("ability_wall_jump"), 1);
        PlayerPrefs::set_int(ProfileKey("paused"), 0);
        if (!PlayerPrefs::has_key(ProfileKey("master_volume"))) PlayerPrefs::set_float(ProfileKey("master_volume"), 1.0f);
        if (!PlayerPrefs::has_key(ProfileKey("music_volume")))  PlayerPrefs::set_float(ProfileKey("music_volume"), 0.8f);
        if (!PlayerPrefs::has_key(ProfileKey("sfx_volume")))    PlayerPrefs::set_float(ProfileKey("sfx_volume"), 0.9f);
    }
    // V5 replaces the legacy copied tile layouts. Their saved coordinates
    // referred to platforms that no longer exist, so preserve progression but
    // safely resume the campaign at the authored Home Hollow start.
    if (version < 5) {
        PlayerPrefs::set_float(ProfileKey("spawn_x"), 416.0f);
        PlayerPrefs::set_float(ProfileKey("spawn_y"), 1432.0f);
        PlayerPrefs::set_string(ProfileKey("spawn_name"), "Lantern Refuge");
        PlayerPrefs::set_string(ProfileKey("spawn_scene"), "scene_home.json");
        PlayerPrefs::set_string(ProfileKey("last_scene_path"), "scene_home.json");
        PlayerPrefs::set_string(ProfileKey("current_room"), "home_lantern_refuge");
        PlayerPrefs::set_int(ProfileKey("paused"), 0);
    }
    PlayerPrefs::set_int(ProfileKey("profile_version"), kProfileVersion);
}
inline bool HasUi(EntityRef e) {
    if (!e || !e.Contains("components")) return false;
    for (auto [k, v] : e["components"].items()) {
        if (k.rfind("UI", 0) == 0) return true;
    }
    return false;
}

inline bool NameHasPrefix(string s, string prefix) {
    return s.rfind(prefix, 0) == 0;
}

inline bool GameplayEnabled() {
    return PlayerPrefs::get_int("abyss_gameplay_enabled", 0) != 0;
}

inline void SetGameplayEnabled(bool on) {
    PlayerPrefs::set_int("abyss_gameplay_enabled", on ? 1 : 0);
}

inline bool Paused() { return PlayerPrefs::get_int(ProfileKey("paused"), 0) != 0; }
inline void SetPaused(bool on) {
    PlayerPrefs::set_int(ProfileKey("paused"), on ? 1 : 0);
    Time::time_scale = on ? 0.0 : 1.0;
}
inline void TogglePause() { SetPaused(!Paused()); }
inline void ClearPauseState() { SetPaused(false); }

inline bool CombatAssist() {
    return PlayerPrefs::get_int("abyss_combat_assist", 0) != 0;
}

inline bool ScreenShake() {
    return PlayerPrefs::get_int("abyss_screen_shake", 1) != 0;
}

inline void EnsureDefaults() {
    MigrateProfile();
    if (!PlayerPrefs::has_key("abyss_spawn_x")) {
        PlayerPrefs::set_float("abyss_spawn_x", 416.0f);
        PlayerPrefs::set_float("abyss_spawn_y", 1432.0f);
        PlayerPrefs::set_string("abyss_spawn_name", "Lantern Refuge");
        PlayerPrefs::set_string("abyss_spawn_scene", "scene_home.json");
    }
    if (!PlayerPrefs::has_key("abyss_gameplay_enabled")) PlayerPrefs::set_int("abyss_gameplay_enabled", 1);
    if (!PlayerPrefs::has_key("abyss_screen_shake")) PlayerPrefs::set_int("abyss_screen_shake", 1);
    if (!PlayerPrefs::has_key("abyss_combat_assist")) PlayerPrefs::set_int("abyss_combat_assist", 0);
    // The showcase has one integrated blade-and-arc kit.  Retain the old key
    // only for compatibility with previous prefabs/network snapshots.
    if (!PlayerPrefs::has_key("abyss_weapon_mode")) PlayerPrefs::set_int("abyss_weapon_mode", 1);
    if (!PlayerPrefs::has_key("abyss_boss_defeated")) PlayerPrefs::set_int("abyss_boss_defeated", 0);
    if (!PlayerPrefs::has_key("abyss_victory")) PlayerPrefs::set_int("abyss_victory", 0);
    if (!PlayerPrefs::has_key("abyss_last_scene_path")) PlayerPrefs::set_string("abyss_last_scene_path", "scene_home.json");
    // Dash is part of the advertised opening control set.  Later upgrades
    // improve it, but a new player must never press Shift and get nothing.
    // Dash is a starting movement action, not a gated ability.  Preserve
    // later mobility upgrades separately, but repair legacy saves that
    // explicitly stored zero for this original control.
    if (PlayerPrefs::get_int("abyss_ability_dash", 0) == 0) PlayerPrefs::set_int("abyss_ability_dash", 1);
    if (!PlayerPrefs::has_key("abyss_ability_double_jump")) PlayerPrefs::set_int("abyss_ability_double_jump", 0);
    if (!PlayerPrefs::has_key("abyss_ability_wall_jump")) PlayerPrefs::set_int("abyss_ability_wall_jump", 0);
    if (!PlayerPrefs::has_key("abyss_focus")) PlayerPrefs::set_float("abyss_focus", 3.0f);
    if (!PlayerPrefs::has_key("abyss_map_open")) PlayerPrefs::set_int("abyss_map_open", 0);
    if (!PlayerPrefs::has_key("abyss_current_room")) PlayerPrefs::set_string("abyss_current_room", "Home Hollow");
    if (!PlayerPrefs::has_key("abyss_relic_case_open")) PlayerPrefs::set_int("abyss_relic_case_open", 0);
    if (!PlayerPrefs::has_key("abyss_relic_blade")) PlayerPrefs::set_string("abyss_relic_blade", "Ember Sigil");
    if (!PlayerPrefs::has_key("abyss_relic_arc")) PlayerPrefs::set_string("abyss_relic_arc", "Prism Core");
    if (!PlayerPrefs::has_key("abyss_relic_mobility")) PlayerPrefs::set_string("abyss_relic_mobility", "Swift Coil");
    if (!PlayerPrefs::has_key("abyss_relic_ward")) PlayerPrefs::set_string("abyss_relic_ward", "Warden Charm");
    if (!PlayerPrefs::has_key(ProfileKey("paused"))) PlayerPrefs::set_int(ProfileKey("paused"), 0);
    // A crash/forced close must never strand a fresh launch at time scale 0.
    ClearPauseState();
}

inline int WeaponMode() {
    return 1;
}

inline void SetWeaponMode(int mode) {
    PlayerPrefs::set_int("abyss_weapon_mode", mode == 0 ? 0 : 1);
}

inline string WeaponName() {
    return "Blade + Arc";
}

inline void SetCurrentRoom(string room);

inline void SetSpawn(float x, float y, string name, string scene_path = "") {
    PlayerPrefs::set_float("abyss_spawn_x", x);
    PlayerPrefs::set_float("abyss_spawn_y", y);
    PlayerPrefs::set_string("abyss_spawn_name", name);
    if (scene_path.empty())
        scene_path = PlayerPrefs::get_string("abyss_last_scene_path", "scene_home.json");
    PlayerPrefs::set_string("abyss_spawn_scene", scene_path);
    SetCurrentRoom(name);
}

// ── Ability gating ─────────────────────────────────────────────────────────
inline string CanonicalAbility(string ability) {
    // Old scene assets used both `doublejump`/`double_jump` and
    // `walljump`/`wall_jump` (plus a short-lived `blink` name). Normalize at
    // the profile boundary so gates and shrines cannot silently disagree.
    if (ability == "doublejump" || ability == "double-jump") return "double_jump";
    if (ability == "walljump" || ability == "wall-jump" || ability == "blink") return "wall_jump";
    return ability;
}
inline bool HasAbility(string ability) {
    return PlayerPrefs::get_int("abyss_ability_" + CanonicalAbility(ability), 0) != 0;
}
inline void GrantAbility(string ability) {
    PlayerPrefs::set_int("abyss_ability_" + CanonicalAbility(ability), 1);
}
inline bool DashUnlocked()   { return HasAbility("dash"); }
inline bool DoubleJumpUnlocked() { return HasAbility("double_jump"); }
inline bool WallJumpUnlocked() { return HasAbility("wall_jump"); }

inline int UpgradeValue(const string& upgrade_key);
inline float FocusCapacity() { return 6.0f + (float)UpgradeValue("focus"); }
inline float Focus() { return PlayerPrefs::get_float("abyss_focus", 3.0f); }
inline void SetFocus(float value) { PlayerPrefs::set_float("abyss_focus", Clamp(value, 0.0f, FocusCapacity())); }
inline void AddFocus(float amount) { SetFocus(Focus() + amount); }

inline void SetCurrentRoom(string room) {
    PlayerPrefs::set_string(ProfileKey("current_room"), room);
    PlayerPrefs::set_int(ProfileKey("room_" + room + "_visited"), 1);
}
inline bool RoomVisited(string room) { return PlayerPrefs::get_int(ProfileKey("room_" + room + "_visited"), 0) != 0; }
inline bool RoomCleared(string room) { return PlayerPrefs::get_int(ProfileKey("room_" + room + "_cleared"), 0) != 0; }
inline void MarkRoomCleared(string room) { PlayerPrefs::set_int(ProfileKey("room_" + room + "_cleared"), 1); }
inline bool MapOpen() { return PlayerPrefs::get_int(ProfileKey("map_open"), 0) != 0; }
inline void ToggleMap() { PlayerPrefs::set_int(ProfileKey("map_open"), MapOpen() ? 0 : 1); }
inline bool RelicCaseOpen() { return PlayerPrefs::get_int(ProfileKey("relic_case_open"), 0) != 0; }
inline void ToggleRelicCase() { PlayerPrefs::set_int(ProfileKey("relic_case_open"), RelicCaseOpen() ? 0 : 1); }
inline string EquippedRelic(string slot) { return PlayerPrefs::get_string(ProfileKey("relic_" + slot), ""); }
inline void EquipRelic(string slot, string name) { PlayerPrefs::set_string(ProfileKey("relic_" + slot), name); }

// Pickups are explicitly keyed instead of relying on an entity id: scene
// edits and template copies can safely renumber entities without reviving an
// item the player has already collected.
inline bool PickupCollected(const string& pickup_id) {
    return !pickup_id.empty() && PlayerPrefs::get_int(ProfileKey("pickup_" + pickup_id), 0) != 0;
}
inline void MarkPickupCollected(const string& pickup_id) {
    if (!pickup_id.empty()) PlayerPrefs::set_int(ProfileKey("pickup_" + pickup_id), 1);
}
inline int UpgradeValue(const string& upgrade_key) {
    return PlayerPrefs::get_int(ProfileKey("upgrade_" + upgrade_key), 0);
}
inline void AddUpgrade(const string& upgrade_key, int amount = 1) {
    PlayerPrefs::set_int(ProfileKey("upgrade_" + upgrade_key),
                         std::max(0, UpgradeValue(upgrade_key) + amount));
}
inline string CurrentRoom() { return PlayerPrefs::get_string(ProfileKey("current_room"), "Home Hollow"); }

#ifndef expose_fields
#define expose_fields(member) EXPOSE_FIELD(member)
#endif
} // namespace AbyssGame
