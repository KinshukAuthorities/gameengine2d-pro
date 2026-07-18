#include "../../../engine_cpp/script_system.hpp"
#include "../../../engine_cpp/net/replication_rpc.hpp"
#include "../../../engine_cpp/net/replication_props.hpp"
#include "abyss_shared.hpp"
#include "abyss_fx.hpp"
#include <string>
#include <cmath>

class AbyssCheckpoint : public MonoBehaviour {
public:
    void Awake() override {
        activated = false;
        checkpoint_name = entity ? entity.Value("checkpoint_name", string("Abyss Checkpoint")) : string("Abyss Checkpoint");
        EXPOSE_FIELD(checkpoint_name);

        // Sync activation state when received from host.
        EventBus::instance().subscribe("net_prop_changed", [this](EntityRef data, EntityRef target) {
            if (!target || target != entity) return;
            string field = data.value("field", string());
            if (field != "activated") return;
            bool new_val = (bool)data["value"];
            if (!activated && new_val) {
                activated = true;
                // Play activation burst locally on all clients.
                AbyssFx::SpawnBurst(this, Transform().X(), Transform().Y() - 4.0f,
                                     80.0f, 0.6f, 90.0f, 360.0f,
                                     9.0f, 0.0f, AbyssFx::Color{160, 255, 200, 230}, AbyssFx::Color{120, 220, 170, 0}, 0.12f);
            } else {
                activated = new_val;
            }
        });
    }

    void Update(float dt) override {
        pulse += dt;
        auto sr = GetComponent("SpriteRenderer");
        if (sr) sr.SetValue("opacity", activated ? 1.0f : (0.75f + 0.2f * Sin(pulse * 4.0f)));

        auto light = GetComponent("Light2D");
        if (light) light.SetValue("intensity", activated ? 1.25f : 0.9f + 0.2f * Sin(pulse * 3.0f));
    }

    void OnTriggerEnter2D(EntityRef other) override { Touch(other); }
    void OnCollisionEnter2D(EntityRef other) override { Touch(other); }

private:
    bool activated = false;
    float pulse = 0.0f;
    string checkpoint_name;

    void Touch(EntityRef other) {
        if (!other) return;
        if (!other.Contains("team") || other.Value("team", 0) != 1) return;
        if (activated) return; // already active, skip re-broadcast

        activated = true;
        AbyssGame::SetSpawn(Transform().X(), Transform().Y() - 4.0f, checkpoint_name);
        PlayerPrefs::set_int("abyss_checkpoint_" + checkpoint_name, 1);

        AbyssFx::SpawnBurst(this, Transform().X(), Transform().Y() - 4.0f,
                             80.0f, 0.6f, 90.0f, 360.0f,
                             9.0f, 0.0f, AbyssFx::Color{160, 255, 200, 230}, AbyssFx::Color{120, 220, 170, 0}, 0.12f);

        // Broadcast "activated" so all peers see the checkpoint light up.
        if (entity) {
            Replication::Replicate(entity, "activated", true);
        }
    }
};
