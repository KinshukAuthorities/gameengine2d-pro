#include "../../../engine_cpp/script_system.hpp"
#include "../../../engine_cpp/unity2d_script_api.hpp"
#include "abyss_shared.hpp"
#include "abyss_fx.hpp"
#include <string>

// Reusable, saved world pickup.  It supports permanent stat vessels, Arc
// cells, lore and equipped relics while keeping collection ownership safe:
// the item never keeps a raw player pointer and deactivates itself if the
// profile says it was already taken.
class AbyssPickup : public MonoBehaviour {
public:
    string pickup_id = "home_heart_vessel";
    string display_name = "Heart Vessel";
    string description = "Maximum health increased";
    string pickup_kind = "heart";
    string relic_slot;
    string relic_name;
    int amount = 1;

    void Awake() override {
        pickup_id = entity ? entity.Value("pickup_id", pickup_id) : pickup_id;
        display_name = entity ? entity.Value("display_name", display_name) : display_name;
        description = entity ? entity.Value("description", description) : description;
        pickup_kind = entity ? entity.Value("pickup_kind", pickup_kind) : pickup_kind;
        relic_slot = entity ? entity.Value("relic_slot", relic_slot) : relic_slot;
        relic_name = entity ? entity.Value("relic_name", relic_name) : relic_name;
        amount = entity ? entity.Value("amount", amount) : amount;
        EXPOSE_FIELD(pickup_id);
        EXPOSE_FIELD(display_name);
        EXPOSE_FIELD(description);
        EXPOSE_FIELD(pickup_kind);
        EXPOSE_FIELD(relic_slot);
        EXPOSE_FIELD(relic_name);
        EXPOSE_FIELD(amount);
        base_y = Transform().Y();
        base_y_captured = true;
        if (AbyssGame::PickupCollected(pickup_id) && entity) entity["active"] = false;
    }

    void Update(float dt) override {
        bob_time += dt;
        Transform().SetPosition({Transform().X(), base_y + Sin(bob_time * 2.4f) * 4.0f});
        if (auto light = GetComponent("Light2D"))
            light.SetValue("intensity", 0.68f + Sin(bob_time * 3.4f) * 0.22f);
        if (near_player) {
            ShowPrompt();
            // Trigger callbacks are physics-rate, whereas GetKeyDown is
            // frame-rate.  Reading the input here makes an F press reliable
            // even when the player enters a pickup between two physics ticks.
            if (Input::GetKeyDown(Key::F)) Collect();
        }
    }

    void OnTriggerEnter2D(EntityRef other) override { Touch(other); }
    void OnTriggerStay2D(EntityRef other) override { Touch(other); }
    void OnTriggerExit2D(EntityRef other) override {
        if (other && other.Value("team", 0) == 1) near_player = false;
    }

private:
    bool near_player = false;
    bool collected = false;
    bool base_y_captured = false;
    float base_y = 0.0f;
    float bob_time = 0.0f;

    void Touch(EntityRef other) {
        if (!other || other.Value("team", 0) != 1 || collected) return;
        near_player = true;
    }

    void ShowPrompt() {
        auto hint = Find("HudHint");
        if (hint && hint.Contains("components") && hint["components"].contains("UIText"))
            hint["components"]["UIText"]["text"] = "F  •  collect " + display_name;
    }

    void Collect() {
        if (collected) return;
        collected = true;
        AbyssGame::MarkPickupCollected(pickup_id);
        if (pickup_kind == "heart") AbyssGame::AddUpgrade("heart", amount);
        else if (pickup_kind == "focus") AbyssGame::AddUpgrade("focus", amount);
        else if (pickup_kind == "arc") AbyssGame::AddUpgrade("arc", amount);
        else if (pickup_kind == "relic" && !relic_slot.empty())
            AbyssGame::EquipRelic(relic_slot, relic_name.empty() ? display_name : relic_name);
        else if (pickup_kind == "lore")
            PlayerPrefs::set_int(AbyssGame::ProfileKey("lore_" + pickup_id), 1);

        AbyssGame::AddFocus(1.0f);
        AbyssFx::Explosion(this, Transform().X(), Transform().Y(),
                           AbyssFx::Color{255, 224, 120, 255}, 0.85f);
        auto room = Find("HudRoom");
        if (room) {
            room["_popup_text"] = "RELIC ACQUIRED  •  " + display_name + "\n" + description;
            room["_popup_timer"] = 3.8f;
        }
        auto hint = Find("HudHint");
        if (hint && hint.Contains("components") && hint["components"].contains("UIText"))
            hint["components"]["UIText"]["text"] = "Collected: " + display_name;
        entity["active"] = false;
    }
};
