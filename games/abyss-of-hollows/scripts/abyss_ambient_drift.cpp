#include "../../../engine_cpp/script_system.hpp"
#include <cmath>

// Lightweight motion for world fog, drifting leaves and glow motes.  It owns
// no spawned entities and reads its initial transform once, so it cannot keep
// a stale scene reference after a transition or hot reload.
class AbyssAmbientDrift : public MonoBehaviour {
public:
    float drift_x = 18.0f;
    float drift_y = 8.0f;
    float speed = 0.22f;
    float phase = 0.0f;
    float base_opacity = 0.24f;

    void Awake() override {
        drift_x = entity ? entity.Value("drift_x", drift_x) : drift_x;
        drift_y = entity ? entity.Value("drift_y", drift_y) : drift_y;
        speed = entity ? entity.Value("drift_speed", speed) : speed;
        phase = entity ? entity.Value("drift_phase", phase) : phase;
        base_opacity = entity ? entity.Value("base_opacity", base_opacity) : base_opacity;
        EXPOSE_FIELD(drift_x);
        EXPOSE_FIELD(drift_y);
        EXPOSE_FIELD(speed);
        base_x = Transform().X();
        base_y = Transform().Y();
    }

    void Update(float /*dt*/) override {
        const float t = (float)Time::elapsed_time * speed + phase;
        Transform().SetPosition({base_x + Sin(t) * drift_x,
                                 base_y + Cos(t * 1.31f) * drift_y});
        if (auto sprite = GetComponent("SpriteRenderer"))
            sprite.SetValue("opacity", base_opacity + 0.055f * (0.5f + 0.5f * Sin(t * 2.2f)));
        if (auto light = GetComponent("Light2D"))
            light.SetValue("intensity", 0.58f + 0.14f * (0.5f + 0.5f * Sin(t * 2.7f)));
    }

private:
    float base_x = 0.0f;
    float base_y = 0.0f;
};
