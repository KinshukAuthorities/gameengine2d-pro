#include "../../../engine_cpp/script_system.hpp"
#include <cmath>

class AbyssSeal : public MonoBehaviour {
public:
    void Start() override {
        _beaten = PlayerPrefs::get_int("abyss_boss_defeated", 0) != 0;
    }

    void Update(float /*dt*/) override {
        if (_beaten) {
            auto sr = GetComponent("SpriteRenderer");
            if (sr) sr.SetValue("opacity", 0.15f);
            auto light = GetComponent("Light2D");
            if (light) light.SetValue("intensity", 0.3f);
            Destroy();
            return;
        }

        Transform().Rotate(Sin((float)Time::elapsed_time * 1.8f) * 0.8f);
    }

private:
    bool _beaten = false;
};

