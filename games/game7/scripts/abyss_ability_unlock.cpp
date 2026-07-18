#include "../../../engine_cpp/script_system.hpp"
#include "../../../engine_cpp/unity2d_script_api.hpp"
#include "abyss_shared.hpp"
#include "abyss_fx.hpp"
#include <chrono>
#include <string>
#include <cmath>

class AbyssAbilityUnlock : public MonoBehaviour {
public:
    string ability_name = "dash";
    string display_name = "Dash";
    bool collected = false;
    bool freeze_active = false;
    bool player_near = false;
    float pulse_timer = 0.0f;
    float vanish_timer = 0.0f;
    steady_clock::time_point freeze_start;

    bool UpdateWhilePaused() const override { return freeze_active; }
    void UpdateUnscaled(float /*raw_dt*/) override { Update(0.0f); }

    void Awake() override {
        ability_name = entity.Value("ability_name", string("dash"));
        display_name = entity.Value("display_name", string("Dash"));
    }

    void Update(float dt) override {
        if (freeze_active) {
            auto now = steady_clock::now();
            float elapsed = duration<float>(now - freeze_start).count();
            if (elapsed >= 3.0f) {
                Time::time_scale = 1.0f;
                freeze_active = false;
                auto room = Find("HudRoom");
                if (room) {
                    room->erase("_popup_text");
                    room["_popup_timer"] = 0.0f;
                    auto txt = room["components"]["UIText"];
                    txt["anchor_x"] = 0.0;
                    txt["anchor_y"] = 1.0;
                    txt["pivot_x"] = 0.0;
                    txt["pivot_y"] = 1.0;
                    txt["pos_x"] = 28.0;
                    txt["pos_y"] = -78.0;
                    txt["font_size"] = 16;
                    txt["align"] = "left";
                    txt["v_align"] = "middle";
                }
                vanish_timer = 0.0f;
            }
            return;
        }
        if (collected) {
            vanish_timer -= dt;
            float t = Max(0.0f, (vanish_timer + 0.3f) / 0.3f);
            auto sr = GetComponent("SpriteRenderer");
            if (sr) sr.SetValue("opacity", t);
            auto light = GetComponent("Light2D");
            if (light) light.SetValue("intensity", t * 1.5f);
            if (vanish_timer <= -0.3f) {
                entity["active"] = false;
            }
            return;
        }
        pulse_timer += dt;
        auto sr = GetComponent("SpriteRenderer");
        if (sr) {
            float glow = 0.80f + 0.20f * Sin(pulse_timer * 3.0f);
            sr.SetValue("opacity", glow);
        }
        auto light = GetComponent("Light2D");
        if (light) {
            light.SetValue("intensity", 1.2f + 0.5f * Sin(pulse_timer * 2.5f));
        }
    }

    void OnTriggerEnter2D(EntityRef other) override { TryCollect(other); }
    void OnTriggerStay2D(EntityRef other) override { TryCollect(other); }
    void OnTriggerExit2D(EntityRef other) override {
        if (other && other.Contains("team") && other.Value("team", 0) == 1) player_near = false;
    }

    void TryCollect(EntityRef other) {
        if (collected) return;
        if (!other.Contains("team")) return;
        if (other.Value("team", 0) != 1) return;

        player_near = true;
        if (!Input::GetKeyDown(Key::F)) {
            auto hint = Find("HudHint");
            if (hint && hint.Contains("components") && hint["components"].contains("UIText"))
                hint["components"]["UIText"]["text"] = "F  •  claim " + display_name;
            return;
        }

        collected = true;
        freeze_active = true;
        freeze_start = steady_clock::now();
        Time::time_scale = 0.0f;
        AbyssGame::GrantAbility(ability_name);

        AbyssFx::Explosion(this, Transform().X(), Transform().Y(),
                           AbyssFx::Color{255, 220, 100, 255}, 1.2f);
        AbyssFx::SpawnBurst(this, Transform().X(), Transform().Y(),
                            0.0f, 0.4f, 120.0f, 360.0f,
                            20.0f, 0.0f, AbyssFx::Color{255, 230, 150, 255},
                            AbyssFx::Color{255, 200, 80, 0}, 0.25f);

        auto room = Find("HudRoom");
        if (room) {
            auto txt = room["components"]["UIText"];
            txt["anchor_x"] = 0.5;
            txt["anchor_y"] = 0.5;
            txt["pivot_x"] = 0.5;
            txt["pivot_y"] = 0.5;
            txt["pos_x"] = 0.0;
            txt["pos_y"] = 0.0;
            txt["font_size"] = 28;
            txt["align"] = "center";
            txt["v_align"] = "middle";
            auto col = Entity::array();
            col.push_back(255);
            col.push_back(220);
            col.push_back(100);
            col.push_back(255);
            txt["color"] = col;
            txt["text"] = "New Ability: " + display_name + "!";
            room["_popup_text"] = "New Ability: " + display_name + "!";
            room["_popup_timer"] = 3.0f;
        }
    }
};
