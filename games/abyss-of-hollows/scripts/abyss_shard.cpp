#include "../../../engine_cpp/script_system.hpp"
#include "../../../engine_cpp/net/replication.hpp"
#include "abyss_fx.hpp"
#include <cmath>

// Hostile crystal projectile used by spitters, turrets, and the Sanctum boss.
// This used to be referenced by every scene as `abyss_shard` without an
// implementation, so cloned prefabs stayed alive forever and stacked in the
// arena.  Keep all state local to the spawned entity and resolve an impact at
// most once, even if both trigger and collision callbacks arrive this frame.
class AbyssShard : public MonoBehaviour {
public:
    void Awake() override {
        life = entity ? entity.Value("life_time", 1.25f) : 1.25f;
        speed = entity ? entity.Value("speed", 680.0f) : 680.0f;
        team = entity ? entity.Value("team", 2) : 2;
        dir_x = entity ? entity.Value("dir_x", -1.0f) : -1.0f;
        dir_y = entity ? entity.Value("dir_y", 0.0f) : 0.0f;
        const float len = Sqrt(dir_x * dir_x + dir_y * dir_y);
        if (len > 0.001f) { dir_x /= len; dir_y /= len; }
        else { dir_x = -1.0f; dir_y = 0.0f; }
        if (entity && entity.Contains("components") && entity["components"].contains("Rigidbody2D")) {
            entity["components"]["Rigidbody2D"]["gravity_scale"] = 0.0f;
            entity["components"]["Rigidbody2D"]["allow_sleep"] = false;
            entity["components"]["Rigidbody2D"]["continuous_collision"] = true;
        }
    }

    void Update(float dt) override {
        if (resolved) return;
        life -= dt;
        if (life <= 0.0f) { Resolve(false); return; }

        // Parry changes team/direction on the live entity; do not keep a
        // stale Awake-time copy or a reflected shard would still hurt player.
        team = entity ? entity.Value("team", team) : team;
        dir_x = entity ? entity.Value("dir_x", dir_x) : dir_x;
        dir_y = entity ? entity.Value("dir_y", dir_y) : dir_y;
        const float live_len = Sqrt(dir_x * dir_x + dir_y * dir_y);
        if (live_len > 0.001f) { dir_x /= live_len; dir_y /= live_len; }

        auto rb = Rigidbody();
        if (rb) rb.SetVelocity({dir_x * speed, dir_y * speed});
        Transform().Rotate(420.0f * dt * (dir_x >= 0.0f ? 1.0f : -1.0f));

        const float pulse = 1.0f + 0.10f * Sin((float)Time::elapsed_time * 15.0f);
        Transform().SetScale({pulse, pulse});
        if (auto sr = GetComponent("SpriteRenderer"))
            sr.SetValue("opacity", 0.72f + 0.28f * Sin((float)Time::elapsed_time * 18.0f));
    }

    void OnTriggerEnter2D(EntityRef other) override { Impact(other); }
    void OnCollisionEnter2D(EntityRef other) override { Impact(other); }

private:
    float life = 1.25f;
    float speed = 680.0f;
    float dir_x = -1.0f;
    float dir_y = 0.0f;
    int team = 2;
    bool resolved = false;

    void Impact(EntityRef other) {
        if (resolved || !other) return;
        team = entity ? entity.Value("team", team) : team;
        if (other.Contains("team") && other.Value("team", 0) == team) return;
        const bool damageable = other.Contains("team") && other.Contains("hp")
            && other.Value("team", 0) != team;
        const bool terrain = other.Contains("components") && other["components"].contains("Tilemap");
        bool hazard = false;
        if (other.Contains("tags")) {
            for (const auto& tag : other["tags"])
                if (tag.is_string() && tag.get<string>() == "Hazard") { hazard = true; break; }
        }
        // Metadata triggers such as campaign rooms/checkpoints must not eat
        // a projectile. Only a true body, hazard, or terrain resolves it.
        if (!damageable && !terrain && !hazard) return;
        Resolve(true);
    }

    void Resolve(bool impact) {
        if (resolved) return;
        resolved = true;
        if (impact) {
            AbyssFx::EnergyPuff(this, Transform().X(), Transform().Y(),
                                 AbyssFx::Color{255, 125, 165, 255});
        }
        uint32_t net_id = entity ? Replication::NetIdOf(entity) : 0;
        if (net_id != 0 && Network::IsHost()) Replication::Despawn(entities(), net_id, impact ? "impact" : "expired");
        else Destroy();
    }
};
