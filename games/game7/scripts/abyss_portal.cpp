#include "../../../engine_cpp/script_system.hpp"
#include "../../../engine_cpp/net/replication_rpc.hpp"
#include "../../../engine_cpp/net/replication_props.hpp"
#include "../../../engine_cpp/net/replication.hpp"
#include "abyss_shared.hpp"
#include <cmath>

class AbyssPortal : public MonoBehaviour {
public:
    void Awake() override {
        target_scene = entity ? entity.Value("target_scene", string()) : string();
        target_spawn_name = entity ? entity.Value("target_spawn_name", string()) : string();
        target_spawn_x = entity ? entity.Value("target_spawn_x", 0.0f) : 0.0f;
        target_spawn_y = entity ? entity.Value("target_spawn_y", 0.0f) : 0.0f;
        require_boss_defeated = entity ? entity.Value("require_boss_defeated", 0) != 0 : false;
        EXPOSE_FIELD(target_scene);
        EXPOSE_FIELD(target_spawn_name);
        EXPOSE_FIELD(target_spawn_x);
        EXPOSE_FIELD(target_spawn_y);

        // Listen for the host-broadcast scene-transition world event.
        EventBus::instance().subscribe("world:scene_transition", [this](EntityRef data, EntityRef) {
            // Only clients act on this; the host already did the transition.
            if (Network::IsHost()) return;
            string scene = data.value("scene", string());
            string spawn_name = data.value("spawn_name", string());
            float sx = data.value("spawn_x", 0.0f);
            float sy = data.value("spawn_y", 0.0f);
            if (!spawn_name.empty() || sx != 0.0f || sy != 0.0f)
                AbyssGame::SetSpawn(sx, sy, spawn_name.empty() ? "Unknown Reach" : spawn_name, scene);
            if (!scene.empty()) {
                AbyssGame::SetGameplayEnabled(scene != "scene.json");
                SceneManager::LoadScene(scene);
            }
        });
    }

    void Update(float /*dt*/) override {
        pulse += (float)Time::delta_time;
        auto sr = GetComponent("SpriteRenderer");
        if (sr) sr.SetValue("opacity", 0.88f + 0.12f * Sin(pulse * 3.0f));
        auto light = GetComponent("Light2D");
        if (light) light.SetValue("intensity", 1.0f + 0.25f * Sin(pulse * 2.4f));
        Transform().Rotate(Sin(pulse * 1.1f) * 1.5f);
    }

    void OnTriggerEnter2D(EntityRef other) override {
        if (!other || !other.Contains("team") || other.Value("team", 0) != 1) return;
        if (require_boss_defeated && PlayerPrefs::get_int("abyss_boss_defeated", 0) == 0) return;

        if (!target_scene.empty()) {
            if (!target_spawn_name.empty() || target_spawn_x != 0.0f || target_spawn_y != 0.0f)
                AbyssGame::SetSpawn(target_spawn_x, target_spawn_y,
                                    target_spawn_name.empty() ? "Unknown Reach" : target_spawn_name,
                                    target_scene);
            if (target_scene != "scene.json") {
                PlayerPrefs::set_string("abyss_last_scene_path", target_scene);
            }
            AbyssGame::SetGameplayEnabled(target_scene != "scene.json");

            // Broadcast transition to all peers before loading locally.
            Entity payload = Entity::object();
            payload["scene"] = target_scene;
            payload["spawn_name"] = target_spawn_name;
            payload["spawn_x"] = target_spawn_x;
            payload["spawn_y"] = target_spawn_y;
            Replication::FireWorldEvent("scene_transition", payload);

            SceneManager::LoadScene(target_scene);
            return;
        }

        if (PlayerPrefs::get_int("abyss_boss_defeated", 0) == 0) return;
        PlayerPrefs::set_int("abyss_victory", 1);
        // Broadcast victory to all peers.
        Replication::FireWorldEvent("victory", Entity::object());
        Debug::log("The abyss has been cleared.");
    }

private:
    float pulse = 0.0f;
    string target_scene;
    string target_spawn_name;
    float target_spawn_x = 0.0f;
    float target_spawn_y = 0.0f;
    bool require_boss_defeated = false;
};
