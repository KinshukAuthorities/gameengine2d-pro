#include "../../../engine_cpp/script_system.hpp"
#include <cmath>

class AbyssSeal : public MonoBehaviour {
public:
    void Update(float /*dt*/) override {
        bool beaten = PlayerPrefs::get_int("abyss_boss_defeated", 0) != 0;
        auto sr = GetComponent("SpriteRenderer");
        if (sr) sr.SetValue("opacity", beaten ? 0.15f : 1.0f);

        auto light = GetComponent("Light2D");
        if (light) light.SetValue("intensity", beaten ? 0.3f : 1.0f);

        if (beaten) {
            Destroy();
            return;
        }

        Transform().Rotate(Sin((float)Time::elapsed_time * 1.8f) * 0.8f);
    }
};

