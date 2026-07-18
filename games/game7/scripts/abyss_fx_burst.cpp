#pragma once

// AbyssFxBurst — standalone self-destructing FX burst behaviour.
// Split out from abyss_fx.hpp so it registers/hot-reloads on its own.
//
// Tracks its own age purely from the per-frame dt passed into Update() —
// NOT from Time::elapsed_time/Time::frame_count. Those are process-wide
// globals that a hot-reloaded script DLL does not reliably share with the
// host editor, so anything gating destruction on "now >= some deadline"
// read from Time:: could compare against a clock that never advances and
// never fire. dt is always passed in fresh every call regardless of that,
// so counting locally guarantees this entity dies on schedule.

#include "../../../engine_cpp/script_system.hpp"

class AbyssFxBurst : public MonoBehaviour {
public:
    void Awake() override {
        float burst_life = entity ? entity->value("fx_burst_life", 0.1f) : 0.1f;
        float particle_lifetime = 0.5f;
        auto em = GetComponent("ParticleEmitter");
        if (em) particle_lifetime = em.value("lifetime", 0.5f);
        stop_in = burst_life;
        die_in = Min(0.7f, burst_life + particle_lifetime + 0.05f);
        age = 0.0f;
        emission_stopped = false;
    }

    void Update(float dt) override {
        age += dt;
        if (!emission_stopped && age >= stop_in) {
            emission_stopped = true;
            auto em = GetComponent("ParticleEmitter");
            if (em) em.SetValue("emitting", false);
        }
        if (age >= die_in) {
            Destroy();
        }
    }

private:
    float age = 0.0f;
    float stop_in = 0.0f;
    float die_in = 0.0f;
    bool emission_stopped = false;
};
