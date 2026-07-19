#include "../../../engine_cpp/script_system.hpp"
#include "abyss_shared.hpp"
#include "abyss_fx.hpp"
#include <string>

// A room-local encounter director.  It uses entity IDs/JSON markers rather
// than retaining pointers to spawned enemies, so despawning during a hit,
// scene transition or reload cannot leave an invalid callback behind.
class AbyssEncounterController : public MonoBehaviour {
public:
    string room_id = "sanctum_warden_arena";
    bool boss_only = false;
    int waves = 2;
    float spawn_x = 0.0f;
    float spawn_y = 0.0f;

    void Awake() override {
        room_id = entity ? entity.Value("encounter_id", entity.Value("room_id", room_id)) : room_id;
        boss_only = entity ? entity.Value("boss_only", boss_only) : boss_only;
        waves = entity ? Max(1, entity.Value("waves", waves)) : waves;
        spawn_x = entity ? entity.Value("encounter_spawn_x", Transform().X()) : Transform().X();
        spawn_y = entity ? entity.Value("encounter_spawn_y", Transform().Y()) : Transform().Y();
        EXPOSE_FIELD(room_id);
        EXPOSE_FIELD(boss_only);
        EXPOSE_FIELD(waves);
    }

    void Start() override {
        if (AbyssGame::RoomCleared(room_id)) completed = true;
    }

    void OnTriggerEnter2D(EntityRef other) override { Enter(other); }
    void OnTriggerStay2D(EntityRef other) override { Enter(other); }

    void Update(float dt) override {
        if (completed) return;
        scan_timer = Max(0.0f, scan_timer - dt);
        if (scan_timer > 0.0f) return;
        scan_timer = 0.18f;

        if (boss_only) {
            if (BossAlive() || PlayerPrefs::get_int("abyss_boss_defeated", 0) == 0) return;
            Complete("Sanctum cleared  •  Focus restored  •  return portal awakened");
            return;
        }
        if (!player_entered || !started) return;
        if (CountOwnedEnemies() > 0) return;
        if (wave_index < waves) {
            StartWave();
            return;
        }
        Complete("Room cleared  •  Focus restored  •  reward discovered");
    }

private:
    bool player_entered = false;
    bool started = false;
    bool completed = false;
    int wave_index = 0;
    float scan_timer = 0.0f;

    void Enter(EntityRef other) {
        if (!other || other.Value("team", 0) != 1 || completed) return;
        player_entered = true;
        if (!started && !boss_only) {
            started = true;
            ShowStatus("Encounter sealed  •  read the enemy tells");
        }
    }

    bool BossAlive() const {
        for (const auto& e : entities()) {
            if (!e.value("active", true)) continue;
            if (e.value("name", string()) == "AbyssBoss" && e.value("hp", 0) > 0) return true;
        }
        return false;
    }

    int CountOwnedEnemies() const {
        int count = 0;
        for (const auto& e : entities()) {
            if (!e.value("active", true)) continue;
            if (e.value("abyss_encounter_owner", string()) != room_id) continue;
            if (e.value("hp", 0) > 0) ++count;
        }
        return count;
    }

    void StartWave() {
        auto crawler_template = Find("AbyssCrawlerTemplate");
        if (!crawler_template) {
            // Do not leave a room permanently sealed if a project variant
            // intentionally omits this sample template.
            completed = true;
            ShowStatus("Encounter skipped: crawler template is unavailable.");
            return;
        }
        ++wave_index;
        const int count = 1 + wave_index;
        const float spacing = 92.0f;
        for (int i = 0; i < count; ++i) {
            const float offset = (i - (count - 1) * 0.5f) * spacing;
            EntityRef enemy = Instantiate(crawler_template, spawn_x + offset, spawn_y);
            if (!enemy) continue;
            enemy["active"] = true;
            enemy["name"] = "RoomCrawler_" + room_id + "_" + ToStr(wave_index) + "_" + ToStr(i);
            enemy["team"] = 2;
            enemy["hp"] = 2 + wave_index;
            enemy["damage"] = 1;
            enemy["speed"] = 96.0f + wave_index * 14.0f;
            enemy["patrol_range"] = 220.0f;
            enemy["patrol_left"] = spawn_x - 520.0f;
            enemy["patrol_right"] = spawn_x + 520.0f;
            enemy["abyss_encounter_owner"] = room_id;
            AbyssFx::EnergyPuff(this, spawn_x + offset, spawn_y, AbyssFx::Color{255, 180, 110, 255});
        }
        ShowStatus("Wave " + ToStr(wave_index) + " / " + ToStr(waves) + "  •  clear the room");
    }

    void Complete(const string& message) {
        if (completed) return;
        completed = true;
        AbyssGame::MarkRoomCleared(room_id);
        AbyssGame::AddFocus(2.0f);
        PlayerPrefs::set_int("abyss_reward_" + room_id, 1);
        AbyssFx::Explosion(this, Transform().X(), Transform().Y(), AbyssFx::Color{255, 220, 125, 255}, 1.20f);
        ShowStatus(message);
    }

    void ShowStatus(const string& text) {
        auto hud = Find("HudRoom");
        if (!hud || !hud.Contains("components") || !hud["components"].contains("UIText")) return;
        hud["_popup_text"] = text;
        hud["_popup_timer"] = 3.2f;
    }
};
