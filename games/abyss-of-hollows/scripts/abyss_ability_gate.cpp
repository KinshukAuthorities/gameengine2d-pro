#include "../../../engine_cpp/script_system.hpp"
#include "../../../engine_cpp/unity2d_script_api.hpp"
#include "abyss_shared.hpp"
#include "abyss_fx.hpp"
#include <string>
#include <cmath>

class AbyssAbilityGate : public MonoBehaviour {
public:
    string required_ability = "dash";
    float fade_duration = 0.6f;
    bool opened = false;
    float fade_timer = 0.0f;

    void Awake() override {
        required_ability = entity.Value("required_ability", string("dash"));
        fade_duration = entity.Value("fade_duration", 0.6f);
    }

    float pulse_timer = 0.0f;
    float locked_flash_timer = 0.0f;

    void Update(float dt) override {
        CheckAutoOpen(); // open silently if ability was granted since last trigger
        pulse_timer += dt;
        locked_flash_timer = Max(0.0f, locked_flash_timer - dt);

        if (opened) {
            fade_timer -= dt;
            float t = Max(0.0f, fade_timer / fade_duration);
            auto sr = GetComponent("SpriteRenderer");
            if (sr) {
                sr.SetValue("opacity", t);
            }
            auto col = GetComponent("BoxCollider2D");
            if (col) {
                col.SetValue("enabled", t > 0.0f);
            }
            if (fade_timer <= 0.0f) {
                entity["active"] = false;
            }
            return;
        }

        auto sr = GetComponent("SpriteRenderer");
        if (sr) {
            float base_op = 0.80f + 0.10f * Sin(pulse_timer * 2.0f);
            if (locked_flash_timer > 0.0f) {
                sr.SetValue("opacity", 1.0f);
                float flash = Sin(locked_flash_timer * 30.0f) * 0.5f + 0.5f;
                sr.SetValue("color", Entity::array());
                auto c = sr.raw()["color"];
                c.push_back((int)(180 + 75 * flash));
                c.push_back((int)(120 - 80 * flash));
                c.push_back((int)(255 - 120 * flash));
                c.push_back(255);
            } else {
                sr.SetValue("opacity", base_op);
            }
        }
        auto light = GetComponent("Light2D");
        if (light) {
            light.SetValue("intensity", 0.8f + 0.2f * Sin(pulse_timer * 2.0f));
        }
    }

    void OnTriggerEnter2D(EntityRef other) override {
        if (opened) return;
        if (!other.Contains("team")) return;
        if (other.Value("team", 0) != 1) return;

        if (AbyssGame::HasAbility(required_ability)) {
            opened = true;
            fade_timer = fade_duration;
            // Also disable the solid collider so nothing else bumps it during fade
            auto col = GetComponent("BoxCollider2D");
            if (col) col.SetValue("enabled", false);
            AbyssFx::SpawnBurst(this, Transform().X(), Transform().Y(),
                                120.0f, 0.3f, 100.0f, 360.0f,
                                15.0f, 0.0f, AbyssFx::Color{200, 220, 255, 255}, AbyssFx::Color{100, 140, 255, 0}, 0.2f);
        } else {
            locked_flash_timer = 0.4f;
            AbyssFx::SpawnBurst(this, Transform().X(), Transform().Y(),
                                40.0f, 0.2f, 30.0f, 60.0f,
                                5.0f, 0.0f, AbyssFx::Color{255, 100, 100, 200}, AbyssFx::Color{180, 60, 60, 0}, 0.1f);
        }
    }

    // Also check every frame whether the ability was just unlocked (e.g.
    // if the player picked up the shrine in the same scene and then walks
    // back to the gate without re-triggering it).
    void CheckAutoOpen() {
        if (opened) return;
        if (AbyssGame::HasAbility(required_ability)) {
            opened = true;
            fade_timer = fade_duration;
            auto col = GetComponent("BoxCollider2D");
            if (col) col.SetValue("enabled", false);
            AbyssFx::SpawnBurst(this, Transform().X(), Transform().Y(),
                                120.0f, 0.3f, 100.0f, 360.0f,
                                15.0f, 0.0f, AbyssFx::Color{200, 220, 255, 255}, AbyssFx::Color{100, 140, 255, 0}, 0.2f);
        }
    }
};
