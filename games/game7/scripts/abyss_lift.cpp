#include "../../../engine_cpp/script_system.hpp"
#include <cmath>

class AbyssLift : public MonoBehaviour {
public:
    void Awake() override {
        base = Transform().Position();
        amplitude = entity ? entity->value("amplitude", 110.0f) : 110.0f;
        speed = entity ? entity->value("speed", 1.0f) : 1.0f;
    }

    void Update(float /*dt*/) override {
        phase += speed * (float)Time::delta_time;
        Vector2 pos = base;
        pos.y += sin(phase) * amplitude;
        Transform().SetPosition(pos);
        auto sr = GetComponent("SpriteRenderer");
        if (sr) sr.set("opacity", 0.86f + 0.12f * sin(phase * 2.0f));
    }

private:
    Vector2 base{};
    float amplitude = 0.0f;
    float speed = 1.0f;
    float phase = 0.0f;
};

