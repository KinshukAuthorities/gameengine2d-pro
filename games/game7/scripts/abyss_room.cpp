#include "../../../engine_cpp/script_system.hpp"
#include "../../../engine_cpp/unity2d_script_api.hpp"
#include "abyss_shared.hpp"
#include "abyss_fx.hpp"
#include <string>

// A room volume is deliberately a small, self-contained gameplay object.
// Region scenes can have four or more of these without depending on a
// teleporter: entering a volume reveals it on the world map and updates the
// HUD title/objective.  The profile key is stable across scene edits.
class AbyssRoom : public MonoBehaviour {
public:
    string room_id = "home_lantern_refuge";
    string title = "Lantern Refuge";
    string objective = "Follow the lanterns";

    void Awake() override {
        room_id = entity ? entity.Value("room_id", room_id) : room_id;
        title = entity ? entity.Value("room_title", title) : title;
        objective = entity ? entity.Value("room_objective", objective) : objective;
        EXPOSE_FIELD(room_id);
        EXPOSE_FIELD(title);
        EXPOSE_FIELD(objective);
    }

    void OnTriggerEnter2D(EntityRef other) override { Enter(other); }
    void OnTriggerStay2D(EntityRef other) override { Enter(other); }

private:
    bool shown = false;

    void Enter(EntityRef other) {
        if (!other || other.Value("team", 0) != 1) return;
        AbyssGame::SetCurrentRoom(room_id);
        if (shown) return;
        shown = true;

        auto hud = Find("HudRoom");
        if (hud) {
            hud["_popup_text"] = title + "  —  " + objective;
            hud["_popup_timer"] = 3.2f;
        }
        AbyssFx::SpawnBurst(this, Transform().X(), Transform().Y(),
                            32.0f, 0.45f, 55.0f, 120.0f,
                            4.0f, 0.0f, AbyssFx::Color{165, 228, 240, 150},
                            AbyssFx::Color{120, 180, 220, 0}, 0.12f);
    }
};
