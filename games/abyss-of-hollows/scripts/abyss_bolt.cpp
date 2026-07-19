#include <cmath>
#include <algorithm>
#include "../../../engine_cpp/script_system.hpp"
#include "../../../engine_cpp/net/replication_rpc.hpp"
#include "../../../engine_cpp/net/replication.hpp"
#include "abyss_fx.hpp"

class AbyssBolt : public MonoBehaviour {
public:
    void Awake() override {
        life = entity ? entity.Value("life_time", 1.1f) : 1.1f;
        speed = entity ? entity.Value("speed", 980.0f) : 980.0f;
        damage = entity ? entity.Value("damage", 1) : 1;
        team = entity ? entity.Value("team", 1) : 1;
        pierces = entity ? entity.Value("pierces", 0) : 0;
        // Arc is a gameplay projectile, not a decorative Animator.  Prime the
        // body here so a freshly-instantiated clone cannot spend its first
        // frame as a static sprite while the physics step catches up.
        if (entity && entity.Contains("components") && entity["components"].contains("Rigidbody2D")) {
            auto& rb = entity["components"]["Rigidbody2D"];
            rb["gravity_scale"] = 0.0f;
            rb["is_kinematic"] = true;
            rb["continuous_collision"] = true;
            rb["allow_sleep"] = false;
        }
        if (auto sr = GetComponent("SpriteRenderer")) {
            // bolt_abyss.png is a four-frame 16px strip.  A clone must never
            // show the whole strip as one large blue rectangle.
            sr.SetValue("use_source_rect", true);
            sr.SetValue("source_x", 0);
            sr.SetValue("source_y", 0);
            sr.SetValue("source_w", 16);
            sr.SetValue("source_h", 16);
        }
        EXPOSE_FIELD(speed);
        EXPOSE_FIELD(damage);
    }

    void Update(float dt) override {
        life -= dt;
        if (life <= 0.0f) {
            // Host despawns the bolt authoritatively.
            uint32_t net_id = entity ? Replication::NetIdOf(entity) : 0;
            if (net_id != 0 && Network::IsHost()) {
                Replication::Despawn(entities(), net_id, "expired");
            } else {
                Destroy();
            }
            return;
        }

        float dx = entity ? entity.Value("dir_x", 1.0f) : 1.0f;
        float dy = entity ? entity.Value("dir_y", 0.0f) : 0.0f;
        float len = Sqrt(dx * dx + dy * dy);
        if (len < 0.001f) { dx = 1.0f; dy = 0.0f; len = 1.0f; }
        dx /= len; dy /= len;

        // A prefabricated bolt can be despawned during a scene transition
        // before its rigidbody wrapper is resolved.  Never dereference a
        // missing component in that one-frame teardown window.
        // Move directly as well as publishing velocity.  Newly-created
        // scripted prefabs are registered on the next script pass; relying
        // only on the following physics step made Arc appear as a permanent
        // blue object when that pass was delayed by an editor frame.  Direct
        // movement makes the visual and hit sweep deterministic.  Arc's own
        // narrow overlap guard below resolves high-speed targets, so keeping
        // a second physics velocity here would only move the bolt twice.
        const float step = Min(dt, 1.0f / 30.0f);
        Transform().SetPosition({Transform().X() + dx * speed * step,
                                 Transform().Y() + dy * speed * step});
        if (auto rb = Rigidbody()) rb.SetVelocity({0.0f, 0.0f});
        else { Destroy(); return; }
        Transform().Rotate((team == 1 ? 400.0f : -320.0f) * dt);

        float s = 1.0f + 0.08f * Sin((float)Time::elapsed_time * 14.0f);
        Transform().SetScale({s, s});

        auto sr = GetComponent("SpriteRenderer");
        if (sr) sr.SetValue("opacity", 0.78f + 0.22f * Sin((float)Time::elapsed_time * 18.0f));

        AbyssFx::Color trail_c = team == 1
            ? AbyssFx::Color{160, 220, 255, 150}
            : AbyssFx::Color{255, 140, 140, 150};
        float px = Transform().X(), py = Transform().Y();
        AbyssFx::TrailSegment(px - dx * speed * dt, py - dy * speed * dt, px, py, trail_c, 0.1f, 2);

        // The regular trigger path below remains the authoritative collision
        // path.  This short overlap guard closes a high-speed tunnelling gap
        // against small enemies, so Arc always has a visible gameplay effect
        // even between two 120 Hz physics substeps.
        if (team == 1) {
            _tunnel_check_frames = (_tunnel_check_frames + 1) % 3;
            if (_tunnel_check_frames != 0) goto after_tunnel_check;
            for (auto& other : entities()) {
                if (other.value("_destroyed", false) || !other.value("active", true)) continue;
                if (other.value("team", 0) != 2 || !other.contains("hp") ||
                    !other.contains("components") || !other["components"].contains("Transform")) continue;
                const auto& ot = other["components"]["Transform"];
                const float ox = ot.value("x", px);
                const float oy = ot.value("y", py);
                if (Abs(ox - px) > 38.0f || Abs(oy - py) > 42.0f) continue;
                const int applied = Max(1, damage);
                other["hp"] = Max(0, other.value("hp", 1) - applied);
                other["_abyss_arc_hit"] = other.value("_abyss_arc_hit", 0) + 1;
                AbyssFx::HitSpark(this, px, py, AbyssFx::Color{190, 240, 255, 255}, 1.0f);
                if (pierces > 0) {
                    --pierces;
                    if (entity) entity["pierces"] = pierces;
                } else {
                    Destroy();
                }
                return;
            }
            after_tunnel_check:;
        }
    }

    void OnTriggerEnter2D(EntityRef other) override {
        if (!other) return;
        if (other.Contains("team") && other.Value("team", 0) == team) return;
        const bool damageable = other.Contains("team") && other.Contains("hp")
            && other.Value("team", 0) != team;
        const bool terrain = other.Contains("components") && other["components"].contains("Tilemap");
        bool hazard = false;
        if (other.Contains("tags")) {
            for (const auto& tag : other["tags"])
                if (tag.is_string() && tag.get<string>() == "Hazard") { hazard = true; break; }
        }
        // Ignore authored room/checkpoint/portal volumes. They are gameplay
        // metadata, not bullet walls; only terrain, hazards, and real enemy
        // bodies may consume an Arc bolt.
        if (!damageable && !terrain && !hazard) return;
        AbyssFx::EnergyPuff(this, Transform().X(), Transform().Y(),
                             team == 1 ? AbyssFx::Color{170, 225, 255, 255} : AbyssFx::Color{255, 150, 150, 255});

        // A charged Arc Bolt can carry through a short line of targets.  It
        // is still finite and expires normally, so no projectile can linger.
        if (pierces > 0 && damageable) {
            --pierces;
            if (entity) entity["pierces"] = pierces;
            return;
        }
        // Host authoritatively despawns the bolt on impact.
        uint32_t net_id = entity ? Replication::NetIdOf(entity) : 0;
        if (net_id != 0 && Network::IsHost()) {
            Replication::Despawn(entities(), net_id, "hit");
        } else {
            Destroy();
        }
    }

private:
    float life = 0.0f;
    float speed = 0.0f;
    int damage = 0;
    int team = 0;
    int pierces = 0;
    int _tunnel_check_frames = 0;

};
