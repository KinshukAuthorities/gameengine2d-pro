#include "../../../engine_cpp/script_system.hpp"
#include "../../../engine_cpp/unity2d_script_api.hpp"
#include "abyss_shared.hpp"
#include "abyss_fx.hpp"
#include <string>
#include <vector>
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
// AbyssProgressionManager
//
// Place one instance of this script in EVERY gameplay scene.
// It reads the scene name from PlayerPrefs (abyss_last_scene_path) and
// spawns only the shrines / gates that belong to this scene AND are not yet
// collected / unlocked.
//
// Ability unlock order:
//   Home Hollow       → Dash shrine (above spawn). Gate before Ascent portal.
//   Ascent            → Double-Jump shrine (top spire). Gate before Deep portal.
//   Deep / anywhere   → Wall-Jump shrine (deep mines). Gate before Boss.
// ─────────────────────────────────────────────────────────────────────────────
class AbyssProgressionManager : public MonoBehaviour {
public:
    bool initialized = false;
    string scene_key_override;

    struct UnlockDef {
        string scene;        // matches last component of scene path
        float x, y;
        string ability_name;
        string display_name;
    };

    struct GateDef {
        string scene;
        float x, y;
        string required_ability;
    };

    void Start() override {
        if (initialized) return;
        initialized = true;

        scene_key_override = entity ? entity.Value("scene_key", string()) : string();
        EXPOSE_FIELD(scene_key_override);

        // Derive which scene we're in from the PlayerPref path
        // e.g. "scene_home.json" → "scene_home"
        string scene_path = PlayerPrefs::get_string("abyss_last_scene_path", "scene_home.json");
        string scene_key = scene_key_override.empty() ? scene_path : scene_key_override;
        // Strip ".json" suffix if present
        if (scene_key.size() > 5 && scene_key.substr(scene_key.size() - 5) == ".json")
            scene_key = scene_key.substr(0, scene_key.size() - 5);

        const string room_name = RegionName(scene_key);
        AbyssGame::SetCurrentRoom(room_name);
        ShowRegionCard(room_name, RegionObjective(scene_key));

        // ── Unlock shrines ────────────────────────────────────────────────
        // Placed just above the first natural resting/visible spot in each scene.
        vector<UnlockDef> unlocks = {
            // Home Hollow: Dash shrine on the ledge above spawn (y inverted: lower y = higher up)
            { "scene_home",   620.0f,  880.0f,  "dash",        "Dash"        },
            // Ascent:  Double Jump shrine near the mid-checkpoint platform
            { "scene_ascent", 2800.0f, 1980.0f, "double_jump", "Double Jump" },
            // Deep Mines: Wall Jump shrine — tucked in a vertical shaft
            { "scene_deep",   3000.0f, 3750.0f, "wall_jump",   "Wall Jump"   },
        };

        // ── Gates ─────────────────────────────────────────────────────────
        // Placed just in front of the portal that leads to the next zone.
        vector<GateDef> gates = {
            // Home → Ascent portal is at (1041, 2215); gate stands just to the left
            { "scene_home",   980.0f,  2215.0f, "dash"        },
            // Ascent → Deep portal is at (3200, 256); gate just below it
            { "scene_ascent", 3200.0f,  320.0f, "double_jump" },
            // Boss entrance: wall-jump gate on the approach corridor
            { "scene_boss",   480.0f,  1600.0f, "wall_jump"   },
        };

        for (auto u : unlocks) {
            if (u.scene != scene_key) continue;
            if (AbyssGame::HasAbility(u.ability_name)) continue;
            SpawnUnlockShrine(u.x, u.y, u.ability_name, u.display_name);
        }
        for (auto g : gates) {
            if (g.scene != scene_key) continue;
            if (AbyssGame::HasAbility(g.required_ability)) continue;
            SpawnGate(g.x, g.y, g.required_ability);
        }

        // Campaign revision 2 stores collision terrain and the four actual
        // room volumes directly in every scene JSON through
        // tools/author_campaign_maps.py.  Do not generate a second invisible
        // set at runtime: duplicate volumes made rooms feel like portal
        // labels and could double-spawn encounters after a scene reload.
        // Retain the legacy construction path only for older user projects.
        if (!HasAuthoredWorld()) {
            SpawnRegionBeacon(scene_key);
            ApplyRegionPalette(scene_key);
            BuildRegionRooms(scene_key);
            SpawnRegionAtmosphere(scene_key);
        }
    }

    // ── Helper: build an RGBA colour array ───────────────────────────────
    Entity rgba(int r, int g, int b, int a) {
        Entity c = Entity::array();
        c.push_back(r); c.push_back(g); c.push_back(b); c.push_back(a);
        return c;
    }
    Entity str_arr(string s) {
        Entity a = Entity::array();
        a.push_back(s);
        return a;
    }

    bool HasAuthoredWorld() const {
        int terrain_layers = 0;
        int authored_rooms = 0;
        for (const auto& e : entities()) {
            const string name = e.value("name", string());
            if (name.rfind("WorldTerrain_", 0) == 0) ++terrain_layers;
            if (name.rfind("RoomVolume_", 0) == 0) ++authored_rooms;
        }
        // A stale CampaignLayout_v2 marker was present in every Game7 scene
        // despite the scenes containing neither the named terrain entities
        // nor their four physical room volumes.  Trusting that marker hid the
        // only construction path, leaving an empty-looking copied map with no
        // progression rooms.  A layout is now considered authored only when
        // it actually carries both its collision/terrain layer and every
        // room volume.  Legacy scenes correctly receive the safe runtime
        // construction until their JSON is genuinely authored.
        return terrain_layers > 0 && authored_rooms >= 4;
    }

    string RegionName(const string& scene) const {
        if (scene == "scene_home") return "Home Hollow";
        if (scene == "scene_verdant") return "Verdant Hollow";
        if (scene == "scene_crystal") return "Crystal Hall";
        if (scene == "scene_flooded") return "Flooded Ruins";
        if (scene == "scene_deep") return "Deep Mines";
        if (scene == "scene_ascent") return "The Ascent";
        if (scene == "scene_boss") return "Boss Sanctum";
        return "Abyss of Hollows";
    }

    string RegionObjective(const string& scene) const {
        if (scene == "scene_home") return "Learn the blade, arc, and parry";
        if (scene == "scene_verdant") return "Find the wind-carved routes";
        if (scene == "scene_crystal") return "Climb the reflected crystal paths";
        if (scene == "scene_flooded") return "Cross the drowned vaults";
        if (scene == "scene_deep") return "Follow the lanterns through the mines";
        if (scene == "scene_ascent") return "Master the high path";
        if (scene == "scene_boss") return "Face the Sanctum guardian";
        return "Explore the hollow";
    }

    void ShowRegionCard(const string& room, const string& objective) {
        auto hud = Find("HudRoom");
        if (!hud || !hud.Contains("components") || !hud["components"].contains("UIText")) return;
        hud["_popup_text"] = room + "  —  " + objective;
        hud["_popup_timer"] = 3.4f;
    }

    // A small authored beacon gives every region a different readable colour
    // language without duplicating scene-only VFX code.  It uses existing
    // original assets and is deliberately non-colliding.
    void SpawnRegionBeacon(const string& scene) {
        int r = 120, g = 200, b = 255;
        if (scene == "scene_verdant") { r = 120; g = 235; b = 160; }
        else if (scene == "scene_crystal") { r = 110; g = 230; b = 255; }
        else if (scene == "scene_flooded") { r = 80; g = 145; b = 245; }
        else if (scene == "scene_deep") { r = 255; g = 170; b = 95; }
        else if (scene == "scene_ascent") { r = 235; g = 125; b = 190; }
        else if (scene == "scene_boss") { r = 255; g = 80; b = 125; }

        Entity def;
        def["active"] = true;
        def["children"] = Entity::array();
        def["id"] = 30000 + (int)entities().size();
        def["name"] = "RegionBeacon_" + scene;
        def["components"] = Entity::object();
        def["components"]["Transform"] = Entity::object();
        def["components"]["Transform"]["x"] = 100.0;
        def["components"]["Transform"]["y"] = 100.0;
        def["components"]["Transform"]["rotation"] = 0.0;
        def["components"]["Transform"]["scale_x"] = 1.0;
        def["components"]["Transform"]["scale_y"] = 1.0;
        def["components"]["Light2D"] = Entity::object();
        def["components"]["Light2D"]["color"] = rgba(r, g, b, 255);
        def["components"]["Light2D"]["intensity"] = 0.55;
        def["components"]["Light2D"]["radius"] = 240.0;
        entities().push_back(def);
    }

    void ApplyRegionPalette(const string& scene) {
        int r = 205, g = 185, b = 255;
        if (scene == "scene_verdant") { r = 155; g = 235; b = 178; }
        else if (scene == "scene_crystal") { r = 145; g = 220; b = 255; }
        else if (scene == "scene_flooded") { r = 115; g = 155; b = 255; }
        else if (scene == "scene_deep") { r = 255; g = 185; b = 125; }
        else if (scene == "scene_ascent") { r = 235; g = 145; b = 220; }
        else if (scene == "scene_boss") { r = 210; g = 95; b = 145; }
        for (auto& e : entities()) {
            if (!e.contains("components") || !e["components"].contains("ParallaxBackground")) continue;
            e["components"]["ParallaxBackground"]["color"] = rgba(r, g, b, 255);
        }
    }

    struct RoomDef {
        string id;
        string title;
        string objective;
        float x;
        string pickup_kind;
        string pickup_name;
        string pickup_description;
        string relic_slot;
    };

    // Four contiguous, physical map volumes are authored for every region.
    // They follow the actual left-to-right collision route already present in
    // the scenes; entering one reveals a named room rather than simulating
    // exploration through a row of teleporter destinations.
    vector<RoomDef> RoomsFor(const string& scene) {
        if (scene == "scene_home") return {
            {"home_lantern_refuge", "Lantern Refuge", "Learn blade, Arc, and parry", 960, "heart", "Heart Vessel", "A warm vessel strengthens your shell.", ""},
            {"home_old_well", "Old Well", "Follow the lowered bridge", 2880, "lore", "Wellkeeper's Note", "A map fragment from the first explorers.", ""},
            {"home_training_vault", "Training Vault", "Use dash through the broken hall", 4800, "relic", "Swift Coil", "Dash recovers with a brighter trail.", "mobility"},
            {"home_rootway_gate", "Rootway Gate", "Reach the hollow beyond", 6720, "arc", "Arc Cell", "One additional Arc charge is stored in your case.", ""}
        };
        if (scene == "scene_verdant") return {
            {"verdant_mossbridge", "Mossbridge", "Cross the living stone", 960, "heart", "Heart Vessel", "A woven vessel expands maximum health.", ""},
            {"verdant_windroot_canopy", "Windroot Canopy", "Ride the high-root route", 2880, "relic", "Mirror Sigil", "Charged Arc gains a prism afterglow.", "arc"},
            {"verdant_thornwell", "Thornwell", "Find the hidden shrine", 4800, "lore", "Thornwell Etching", "A faded record of the rootwardens.", ""},
            {"verdant_echo_shrine", "Echo Shrine", "Open the distant shortcut", 6720, "focus", "Focus Vessel", "Your Focus capacity grows by one.", ""}
        };
        if (scene == "scene_crystal") return {
            {"crystal_shard_gallery", "Shard Gallery", "Read the reflected danger", 960, "arc", "Arc Cell", "A faceted charge for your Arc Core.", ""},
            {"crystal_prism_wells", "Prism Wells", "Climb the bright wells", 2880, "lore", "Prism Memory", "A shard carries a traveller's route.", ""},
            {"crystal_resonance_shaft", "Resonance Shaft", "Use the vertical route", 4800, "relic", "Warden Charm", "A ward answers a perfect parry.", "ward"},
            {"crystal_glass_archive", "Glass Archive", "Recover the archive key", 6720, "focus", "Focus Vessel", "Your Focus reservoir grows deeper.", ""}
        };
        if (scene == "scene_flooded") return {
            {"flooded_tidal_vault", "Tidal Vault", "Follow the falling water", 960, "heart", "Heart Vessel", "A drowned vessel still holds warmth.", ""},
            {"flooded_sluice_maze", "Sluice Maze", "Find the raised passage", 2880, "lore", "Sluice Chart", "Old tide marks reveal a safer route.", ""},
            {"flooded_drowned_reliquary", "Drowned Reliquary", "Break the flooded seal", 4800, "arc", "Arc Cell", "The Arc Core hums with stored rain-light.", ""},
            {"flooded_moonwell_dock", "Moonwell Dock", "Reach the moonlit exit", 6720, "relic", "Tide Step", "A mobility relic lightens an aerial dash.", "mobility"}
        };
        if (scene == "scene_deep") return {
            {"deep_ember_tram", "Ember Tram", "Follow the ore lamps", 960, "focus", "Focus Vessel", "A coal-bright vessel expands Focus.", ""},
            {"deep_oreworks", "Oreworks", "Survive the hot machinery", 2880, "lore", "Foreman's Ledger", "A ledger marks a sealed side shaft.", ""},
            {"deep_vein_chasm", "Vein Chasm", "Descend through the crystal vein", 4800, "heart", "Heart Vessel", "A forged vessel strengthens your shell.", ""},
            {"deep_candle_quarry", "Candle Quarry", "Find the ascent lift", 6720, "relic", "Cinder Core", "Arc bolts leave a glowing ember trail.", "arc"}
        };
        if (scene == "scene_ascent") return {
            {"ascent_cloud_steps", "Cloud Steps", "Follow the bell wind", 960, "arc", "Arc Cell", "A high-altitude Arc charge.", ""},
            {"ascent_bell_tower", "Bell Tower", "Climb through the ringing hall", 2880, "lore", "Bell Hymn", "A route sung by the old climbers.", ""},
            {"ascent_gale_gauntlet", "Gale Gauntlet", "Master the dangerous updraft", 4800, "focus", "Focus Vessel", "A cool vessel restores focused breath.", ""},
            {"ascent_crown_walk", "Crown Walk", "Reach the sanctum route", 6720, "relic", "Gale Thread", "Your dash leaves a sharper afterimage.", "mobility"}
        };
        return {
            {"sanctum_gate", "Sanctum Gate", "Read the guardian's warning", 780, "lore", "Warden Oath", "An oath names the final trial.", ""},
            {"sanctum_trial_hall", "Trial Hall", "Cross the silent procession", 2340, "heart", "Heart Vessel", "A final vessel steadies your resolve.", ""},
            {"sanctum_warden_arena", "Warden Arena", "Defeat the Sanctum guardian", 3900, "arc", "Arc Cell", "A charged crystal awaits the victor.", ""},
            {"sanctum_afterglow_chamber", "Afterglow Chamber", "Claim the path home", 5460, "relic", "Hollow Crown", "A relic that records the completed journey.", "ward"}
        };
    }

    void BuildRegionRooms(const string& scene) {
        const auto rooms = RoomsFor(scene);
        for (size_t i = 0; i < rooms.size(); ++i) {
            const auto& room = rooms[i];
            // The second room in every biome is an authored combat pocket.
            // The same wide volume still reveals the room name, while the
            // encounter director only begins after the player has entered.
            SpawnRoomVolume(room, 1900.0f, 2040.0f, i == 1);
            // One discovery item per authored room keeps every section from
            // being an empty corridor and gives the Relic Case a real loop.
            SpawnPickup(room, room.x + 250.0f, (i % 2 == 0) ? 1210.0f : 830.0f);
            SpawnRoomLandmarks(scene, room, (int)i);
        }
    }

    void SpawnRoomVolume(const RoomDef& room, float width, float height, bool with_encounter) {
        Entity def;
        def["active"] = true;
        def["children"] = Entity::array();
        def["id"] = 41000 + (int)entities().size();
        def["name"] = "RoomVolume_" + room.id;
        def["room_id"] = room.id;
        def["room_title"] = room.title;
        def["room_objective"] = room.objective;
        def["components"] = Entity::object();
        def["components"]["Transform"] = Entity::object();
        def["components"]["Transform"]["x"] = room.x;
        def["components"]["Transform"]["y"] = 1024.0f;
        def["components"]["Transform"]["rotation"] = 0.0f;
        def["components"]["Transform"]["scale_x"] = 1.0f;
        def["components"]["Transform"]["scale_y"] = 1.0f;
        def["components"]["BoxCollider2D"] = Entity::object();
        def["components"]["BoxCollider2D"]["width"] = width;
        def["components"]["BoxCollider2D"]["height"] = height;
        def["components"]["BoxCollider2D"]["is_trigger"] = true;
        def["components"]["BoxCollider2D"]["offset_x"] = 0.0f;
        def["components"]["BoxCollider2D"]["offset_y"] = 0.0f;
        def["components"]["ScriptComponent"] = Entity::object();
        def["components"]["ScriptComponent"]["scripts"] = str_arr("abyss_room");
        if (with_encounter) {
            def["components"]["ScriptComponent"]["scripts"].push_back("abyss_encounter_controller");
            def["encounter_id"] = room.id + "_encounter";
            def["boss_only"] = false;
            def["waves"] = 2;
            def["encounter_spawn_x"] = room.x + 80.0f;
            def["encounter_spawn_y"] = 1210.0f;
        }
        def["components"]["ScriptComponent"]["field_overrides"] = Entity::object();
        entities().push_back(def);
    }

    // A room needs a silhouette and a close landmark as well as collision.
    // These are intentionally non-colliding, so the existing, tested routes
    // remain authoritative while every biome gains a distinct five-layer
    // composition (backdrop, existing parallax, terrain, landmark, fog).
    void SpawnRoomLandmarks(const string& scene, const RoomDef& room, int index) {
        string backdrop = "abyss/environment/cc0_grotto_far.png";
        string landmark = "rubble_prop_abyss.png";
        int br = 130, bg = 190, bb = 230;
        int lr = 255, lg = 205, lb = 120;
        if (scene == "scene_verdant") {
            backdrop = "abyss/environment/cc0_leaves.png"; landmark = "lantern_abyss.png";
            br = 120; bg = 205; bb = 145; lr = 180; lg = 255; lb = 155;
        } else if (scene == "scene_crystal") {
            backdrop = "abyss/environment/cc0_grotto_mid.png"; landmark = "crystal_prop_abyss.png";
            br = 110; bg = 205; bb = 245; lr = 130; lg = 245; lb = 255;
        } else if (scene == "scene_flooded") {
            backdrop = "abyss/environment/cc0_ruins.png"; landmark = "lantern_abyss.png";
            br = 95; bg = 150; bb = 230; lr = 135; lg = 205; lb = 255;
        } else if (scene == "scene_deep") {
            backdrop = "abyss/environment/cc0_grotto_back.png"; landmark = "lantern_abyss.png";
            br = 205; bg = 125; bb = 80; lr = 255; lg = 168; lb = 90;
        } else if (scene == "scene_ascent") {
            backdrop = "abyss/environment/cc0_grotto_far.png"; landmark = "crystal_prop_abyss.png";
            br = 205; bg = 145; bb = 235; lr = 245; lg = 190; lb = 255;
        } else if (scene == "scene_boss") {
            backdrop = "abyss/environment/cc0_ruins.png"; landmark = "crystal_prop_abyss.png";
            br = 175; bg = 75; bb = 120; lr = 255; lg = 105; lb = 145;
        }

        SpawnDecor("RoomBackdrop_" + room.id, backdrop, room.x, 930.0f,
                   1.08f, 0, -36, 0.27f, br, bg, bb, false, index);
        SpawnDecor("RoomLandmark_" + room.id, landmark, room.x - 330.0f, 1180.0f,
                   1.15f, 2, 5, 0.92f, lr, lg, lb, true, index + 3);
        SpawnDecor("RoomForeground_" + room.id,
                   scene == "scene_verdant" ? "abyss/environment/cc0_leaves.png" : "rubble_prop_abyss.png",
                   room.x + 560.0f, 1135.0f, scene == "scene_verdant" ? 0.72f : 1.25f,
                   3, 18, 0.48f, br, bg, bb, true, index + 7);
    }

    void SpawnDecor(const string& name, const string& texture, float x, float y,
                    float scale, int layer, int order, float opacity,
                    int r, int g, int b, bool drift, int phase) {
        Entity def;
        def["active"] = true;
        def["children"] = Entity::array();
        def["id"] = 44000 + (int)entities().size();
        def["name"] = name;
        def["components"] = Entity::object();
        def["components"]["Transform"] = Entity::object();
        def["components"]["Transform"]["x"] = x;
        def["components"]["Transform"]["y"] = y;
        def["components"]["Transform"]["rotation"] = 0.0f;
        def["components"]["Transform"]["scale_x"] = scale;
        def["components"]["Transform"]["scale_y"] = scale;
        def["components"]["SpriteRenderer"] = Entity::object();
        def["components"]["SpriteRenderer"]["texture"] = texture;
        def["components"]["SpriteRenderer"]["layer"] = layer;
        def["components"]["SpriteRenderer"]["order_in_layer"] = order;
        def["components"]["SpriteRenderer"]["opacity"] = opacity;
        def["components"]["SpriteRenderer"]["color"] = rgba(r, g, b, 255);
        def["components"]["SpriteRenderer"]["pixels_per_unit"] = 1.0f;
        def["components"]["SpriteRenderer"]["flip_x"] = false;
        def["components"]["SpriteRenderer"]["flip_y"] = false;
        if (drift) {
            def["drift_x"] = 7.0f + (phase % 3) * 3.0f;
            def["drift_y"] = 4.0f + (phase % 2) * 3.0f;
            def["drift_speed"] = 0.16f + (phase % 3) * 0.035f;
            def["drift_phase"] = (float)phase * 0.83f;
            def["base_opacity"] = opacity;
            def["components"]["ScriptComponent"] = Entity::object();
            def["components"]["ScriptComponent"]["scripts"] = str_arr("abyss_ambient_drift");
            def["components"]["ScriptComponent"]["field_overrides"] = Entity::object();
        }
        entities().push_back(def);
    }

    void SpawnPickup(const RoomDef& room, float x, float y) {
        Entity def;
        def["active"] = true;
        def["children"] = Entity::array();
        def["id"] = 42000 + (int)entities().size();
        def["name"] = "Pickup_" + room.id;
        def["pickup_id"] = room.id + "_reward";
        def["display_name"] = room.pickup_name;
        def["description"] = room.pickup_description;
        def["pickup_kind"] = room.pickup_kind;
        def["relic_slot"] = room.relic_slot;
        def["relic_name"] = room.pickup_name;
        def["amount"] = 1;
        def["team"] = 0;
        def["components"] = Entity::object();
        def["components"]["Transform"] = Entity::object();
        def["components"]["Transform"]["x"] = x;
        def["components"]["Transform"]["y"] = y;
        def["components"]["Transform"]["rotation"] = 0.0f;
        def["components"]["Transform"]["scale_x"] = 1.0f;
        def["components"]["Transform"]["scale_y"] = 1.0f;
        def["components"]["BoxCollider2D"] = Entity::object();
        def["components"]["BoxCollider2D"]["width"] = 44.0f;
        def["components"]["BoxCollider2D"]["height"] = 48.0f;
        def["components"]["BoxCollider2D"]["is_trigger"] = true;
        def["components"]["BoxCollider2D"]["offset_x"] = 0.0f;
        def["components"]["BoxCollider2D"]["offset_y"] = 0.0f;
        def["components"]["SpriteRenderer"] = Entity::object();
        def["components"]["SpriteRenderer"]["texture"] = room.pickup_kind == "lore" ? "abyss/pickups/cc0_lore_scroll.png" : "crystal_prop_abyss.png";
        def["components"]["SpriteRenderer"]["layer"] = 2;
        def["components"]["SpriteRenderer"]["order_in_layer"] = 20;
        def["components"]["SpriteRenderer"]["opacity"] = 1.0f;
        def["components"]["SpriteRenderer"]["color"] = room.pickup_kind == "heart" ? rgba(255, 130, 145, 255) : rgba(255, 224, 115, 255);
        def["components"]["SpriteRenderer"]["pixels_per_unit"] = 1.0f;
        def["components"]["SpriteRenderer"]["flip_x"] = false;
        def["components"]["SpriteRenderer"]["flip_y"] = false;
        def["components"]["Light2D"] = Entity::object();
        def["components"]["Light2D"]["color"] = rgba(255, 220, 115, 255);
        def["components"]["Light2D"]["intensity"] = 0.75f;
        def["components"]["Light2D"]["radius"] = 95.0f;
        def["components"]["ScriptComponent"] = Entity::object();
        def["components"]["ScriptComponent"]["scripts"] = str_arr("abyss_pickup");
        def["components"]["ScriptComponent"]["field_overrides"] = Entity::object();
        entities().push_back(def);
    }

    // Existing scenes already own four parallax layers.  This extra mist is
    // a real movable foreground layer, tinted per region, so the world has
    // readable depth without changing collision or obscuring the player.
    void SpawnRegionAtmosphere(const string& scene) {
        int r = 160, g = 208, b = 235;
        if (scene == "scene_verdant") { r = 155; g = 235; b = 178; }
        else if (scene == "scene_crystal") { r = 160; g = 225; b = 255; }
        else if (scene == "scene_flooded") { r = 115; g = 165; b = 240; }
        else if (scene == "scene_deep") { r = 235; g = 168; b = 105; }
        else if (scene == "scene_ascent") { r = 220; g = 165; b = 240; }
        else if (scene == "scene_boss") { r = 210; g = 100; b = 155; }

        for (int i = 0; i < 4; ++i) {
            Entity def;
            def["active"] = true;
            def["children"] = Entity::array();
            def["id"] = 43000 + (int)entities().size();
            def["name"] = "AbyssFog_" + scene + "_" + ToStr(i);
            def["components"] = Entity::object();
            def["components"]["Transform"] = Entity::object();
            def["components"]["Transform"]["x"] = 960.0f + i * 1920.0f;
            def["components"]["Transform"]["y"] = 880.0f + (i % 2) * 230.0f;
            def["components"]["Transform"]["rotation"] = 0.0f;
            def["components"]["Transform"]["scale_x"] = 1.45f;
            def["components"]["Transform"]["scale_y"] = 1.15f;
            def["components"]["SpriteRenderer"] = Entity::object();
            def["components"]["SpriteRenderer"]["texture"] = "bg_abyss_mist.png";
            def["components"]["SpriteRenderer"]["layer"] = 4;
            def["components"]["SpriteRenderer"]["order_in_layer"] = 3;
            def["components"]["SpriteRenderer"]["opacity"] = 0.20f;
            def["components"]["SpriteRenderer"]["color"] = rgba(r, g, b, 255);
            def["components"]["SpriteRenderer"]["pixels_per_unit"] = 1.0f;
            def["components"]["SpriteRenderer"]["flip_x"] = false;
            def["components"]["SpriteRenderer"]["flip_y"] = false;
            def["drift_x"] = 20.0f + i * 6.0f;
            def["drift_y"] = 7.0f + (i % 2) * 7.0f;
            def["drift_speed"] = 0.14f + i * 0.035f;
            def["drift_phase"] = (float)i * 1.19f;
            def["base_opacity"] = 0.20f;
            def["components"]["ScriptComponent"] = Entity::object();
            def["components"]["ScriptComponent"]["scripts"] = str_arr("abyss_ambient_drift");
            def["components"]["ScriptComponent"]["field_overrides"] = Entity::object();
            entities().push_back(def);
        }
    }

    // ── Spawn a glowing ability shrine (trigger collider + script) ────────
    void SpawnUnlockShrine(float x, float y,
                           string ability,
                           string display) {
        Entity def;
        def["active"]   = true;
        def["children"] = Entity::array();
        def["name"]     = "Shrine_" + ability;
        def["ability_name"]  = ability;
        def["display_name"]  = display;
        def["team"]     = 0;
        def["components"] = Entity::object();

        def["components"]["Transform"]            = Entity::object();
        def["components"]["Transform"]["x"]       = x;
        def["components"]["Transform"]["y"]       = y;
        def["components"]["Transform"]["rotation"] = 0.0;
        def["components"]["Transform"]["scale_x"] = 1.5;
        def["components"]["Transform"]["scale_y"] = 1.5;

        def["components"]["BoxCollider2D"]              = Entity::object();
        def["components"]["BoxCollider2D"]["width"]     = 40.0;
        def["components"]["BoxCollider2D"]["height"]    = 48.0;
        def["components"]["BoxCollider2D"]["is_trigger"] = true;
        def["components"]["BoxCollider2D"]["offset_x"]  = 0;
        def["components"]["BoxCollider2D"]["offset_y"]  = 0;

        def["components"]["Light2D"]            = Entity::object();
        def["components"]["Light2D"]["color"]   = rgba(255, 220, 100, 255);
        def["components"]["Light2D"]["intensity"] = 1.5;
        def["components"]["Light2D"]["radius"]  = 160.0;

        def["components"]["SpriteRenderer"]                  = Entity::object();
        def["components"]["SpriteRenderer"]["texture"]       = "lantern_abyss.png";
        def["components"]["SpriteRenderer"]["layer"]         = 2;
        def["components"]["SpriteRenderer"]["order_in_layer"] = 15;
        def["components"]["SpriteRenderer"]["opacity"]       = 1.0;
        def["components"]["SpriteRenderer"]["color"]         = rgba(255, 255, 200, 255);
        def["components"]["SpriteRenderer"]["pixels_per_unit"] = 1.0;
        def["components"]["SpriteRenderer"]["flip_x"]        = false;
        def["components"]["SpriteRenderer"]["flip_y"]        = false;

        def["components"]["ScriptComponent"]          = Entity::object();
        def["components"]["ScriptComponent"]["scripts"] = str_arr("abyss_ability_unlock");
        def["components"]["ScriptComponent"]["field_overrides"] = Entity::object();

        def["id"] = 10000 + (int)entities().size();
        entities().push_back(def);

        // Welcoming burst so the player spots the shrine immediately
        AbyssFx::SpawnBurst(this, x, y, 180.0f, 0.3f, 80.0f, 360.0f,
                            10.0f, 0.0f, AbyssFx::Color{255, 220, 100, 255},
                            AbyssFx::Color{200, 180, 80, 0}, 0.15f);
    }

    // ── Spawn a purple gate barrier (solid collider + trigger + script) ───
    void SpawnGate(float x, float y, string required) {
        Entity def;
        def["active"]   = true;
        def["children"] = Entity::array();
        def["name"]     = "Gate_" + required;
        def["required_ability"] = required;
        def["fade_duration"]    = 0.6;
        def["team"]     = 0;
        def["components"] = Entity::object();

        def["components"]["Transform"]             = Entity::object();
        def["components"]["Transform"]["x"]        = x;
        def["components"]["Transform"]["y"]        = y;
        def["components"]["Transform"]["rotation"] = 0.0;
        def["components"]["Transform"]["scale_x"]  = 1.0;
        def["components"]["Transform"]["scale_y"]  = 1.0;

        // Solid collider so the player is physically blocked
        def["components"]["BoxCollider2D"]               = Entity::object();
        def["components"]["BoxCollider2D"]["width"]      = 50.0;
        def["components"]["BoxCollider2D"]["height"]     = 80.0;
        def["components"]["BoxCollider2D"]["is_trigger"] = true; // script handles flash; physics layer blocks in AbyssAbilityGate
        def["components"]["BoxCollider2D"]["offset_x"]  = 0;
        def["components"]["BoxCollider2D"]["offset_y"]  = 0;

        def["components"]["SpriteRenderer"]                    = Entity::object();
        def["components"]["SpriteRenderer"]["texture"]         = "portal_abyss.png";
        def["components"]["SpriteRenderer"]["layer"]           = 2;
        def["components"]["SpriteRenderer"]["order_in_layer"]  = 14;
        def["components"]["SpriteRenderer"]["opacity"]         = 0.85f;
        def["components"]["SpriteRenderer"]["color"]           = rgba(180, 120, 255, 255);
        def["components"]["SpriteRenderer"]["pixels_per_unit"] = 1.0;
        def["components"]["SpriteRenderer"]["flip_x"]          = false;
        def["components"]["SpriteRenderer"]["flip_y"]          = false;

        def["components"]["Light2D"]              = Entity::object();
        def["components"]["Light2D"]["color"]     = rgba(180, 120, 255, 255);
        def["components"]["Light2D"]["intensity"] = 1.0;
        def["components"]["Light2D"]["radius"]    = 100.0;

        def["components"]["ScriptComponent"]            = Entity::object();
        def["components"]["ScriptComponent"]["scripts"] = str_arr("abyss_ability_gate");
        def["components"]["ScriptComponent"]["field_overrides"] = Entity::object();

        def["id"] = 20000 + (int)entities().size();
        entities().push_back(def);
    }
};
