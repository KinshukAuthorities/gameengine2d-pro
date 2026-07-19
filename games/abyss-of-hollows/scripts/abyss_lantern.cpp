#include "../../../engine_cpp/script_system.hpp"
#include <cmath>

class AbyssLantern : public MonoBehaviour {
public:
    void Update(float /*dt*/) override {
        pulse += (float)Time::delta_time;
        auto light = GetComponent("Light2D");
        if (light) light.SetValue("intensity", 0.95f + 0.25f * Sin(pulse * 2.6f));

        auto sr = GetComponent("SpriteRenderer");
        if (sr) sr.SetValue("opacity", 0.84f + 0.16f * Sin(pulse * 3.5f));
        Transform().Rotate(Sin(pulse * 1.4f) * 1.6f);
    }

private:
    float pulse = 0.0f;
};

