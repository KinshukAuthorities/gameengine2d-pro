#include "../../../engine_cpp/script_system.hpp"
#include "../../../engine_cpp/net/replication_rpc.hpp"
#include "../../../engine_cpp/net/replication.hpp"
#include "abyss_fx.hpp"
#include <cmath>

// Ground enemy with readable states.  The old implementation ran toward the
// player forever and occasionally hopped on contact, which made its attack
// impossible to read.  This controller always exposes a wind-up, one lunge,
// and a recovery window before it can pressure again.
class AbyssCrawler : public MonoBehaviour {
public:
    void Awake() override {
        hp = entity ? entity.Value("hp", 3) : 3;
        if (entity) entity["hp"] = hp;
        damage = entity ? entity.Value("damage", 1) : 1;
        patrol_range = entity ? entity.Value("patrol_range", 180.0f) : 180.0f;
        speed = entity ? entity.Value("speed", 95.0f) : 95.0f;
        alert_range = entity ? entity.Value("alert_range", 560.0f) : 560.0f;
        spawn_x = Transform().X();
        left_bound = entity ? entity.Value("patrol_left", spawn_x - patrol_range) : spawn_x - patrol_range;
        right_bound = entity ? entity.Value("patrol_right", spawn_x + patrol_range) : spawn_x + patrol_range;
        state = State::Patrol;
        EXPOSE_FIELD(hp);
        EXPOSE_FIELD(speed);
        EXPOSE_FIELD(patrol_range);
        EventBus::instance().subscribe("net_health", [this](EntityRef data, EntityRef target) {
            if (target && target == entity) {
                hp = (int)data.value("current_health", (float)hp);
                target["hp"] = hp;
            }
        });
    }

    void Start() override {
        if (entity && entity.Contains("components") && entity["components"].contains("Rigidbody2D")) {
            entity["components"]["Rigidbody2D"]["allow_sleep"] = false;
            // Crawlers are ground enemies.  Use the same script-owned
            // gravity model as the player: mixed engine/script gravity made
            // them drift in mid-air on one map and sink through floors on
            // another, depending on the physics substep count.
            const bool airborne_variant = entity.Value("airborne_variant", false);
            entity["components"]["Rigidbody2D"]["gravity_scale"] = 0.0f;
            entity["components"]["Rigidbody2D"]["_abyss_ground_enemy"] = !airborne_variant;
            entity["components"]["Rigidbody2D"]["continuous_collision"] = true;
        }
    }

    void Update(float dt) override {
        if (entity) hp = entity.Value("hp", hp);
        damage_lock = Max(0.0f, damage_lock - dt);
        state_time = Max(0.0f, state_time - dt);
        flash_time = Max(0.0f, flash_time - dt);
        if (hp <= 0) { Die(); return; }

        auto player = Find("AbyssPlayer");
        const float dx = player ? GetX(player, Transform().X()) - Transform().X() : 99999.0f;
        const float dy = player ? GetY(player, Transform().Y()) - Transform().Y() : 0.0f;
        if (Abs(dx) > 2.0f) facing = dx < 0.0f ? -1.0f : 1.0f;

        if (!(Network::IsClient() && !Network::IsHost())) RunBrain(dx, dy, dt);
        ApplyVisuals();
    }

    void OnTriggerEnter2D(EntityRef other) override { TakeDamage(other); }
    void OnCollisionEnter2D(EntityRef other) override { TakeDamage(other); }

private:
    enum class State { Patrol, Position, Telegraph, Lunge, Recover, Stagger };
    State state = State::Patrol;
    int hp = 3, damage = 1;
    float patrol_range = 180.0f, speed = 95.0f, alert_range = 560.0f;
    float spawn_x = 0.0f, left_bound = 0.0f, right_bound = 0.0f;
    float facing = 1.0f, state_time = 0.0f, attack_cd = 0.0f;
    float damage_lock = 0.0f, flash_time = 0.0f;
    bool dying = false;

    static float GetX(EntityRef e, float fallback) {
        return e && e.Contains("components") && e["components"].contains("Transform")
            ? e["components"]["Transform"].value("x", fallback) : fallback;
    }
    static float GetY(EntityRef e, float fallback) {
        return e && e.Contains("components") && e["components"].contains("Transform")
            ? e["components"]["Transform"].value("y", fallback) : fallback;
    }

    void SetState(State next, float duration) {
        state = next;
        state_time = duration;
        if (entity) {
            const char* label = next == State::Patrol ? "Patrol" : next == State::Position ? "Position" :
                                next == State::Telegraph ? "Telegraph" : next == State::Lunge ? "Lunge" :
                                next == State::Recover ? "Recover" : "Stagger";
            entity["enemy_state"] = label;
        }
    }

    void RunBrain(float dx, float dy, float dt) {
        auto rb = Rigidbody();
        if (!rb) return;
        attack_cd = Max(0.0f, attack_cd - dt);
        float vx = rb.VX();
        const bool grounded = rb.IsGrounded();
        const bool target_visible = Abs(dx) < alert_range && Abs(dy) < 310.0f;

        switch (state) {
            case State::Patrol:
                vx = facing * speed * 0.60f;
                if (Transform().X() <= left_bound) facing = 1.0f;
                if (Transform().X() >= right_bound) facing = -1.0f;
                if (target_visible) SetState(State::Position, 0.0f);
                break;
            case State::Position: {
                const float desired = Abs(dx) > 180.0f ? facing * speed * 1.20f : 0.0f;
                vx += (desired - vx) * Min(1.0f, dt * 8.0f);
                if (!target_visible) {
                    SetState(State::Patrol, 0.0f);
                } else {
                    const bool attack_height = Abs(dy) < 118.0f;
                    // Let a crawler attack while the solver is resolving its
                    // first floor contact too.  `IsGrounded()` can be false
                    // for one frame on a slope/ledge even though the target
                    // is right beside it; a near-zero vertical speed is an
                    // equally safe grounding signal for this lunge.
                    const bool stable_enough = grounded || Abs(rb.VY()) < 34.0f;
                    if (stable_enough && attack_height && Abs(dx) < 190.0f && attack_cd <= 0.0f) {
                        SetState(State::Telegraph, 0.36f);
                        AbyssFx::SpawnBurst(this, Transform().X() + facing * 14.0f, Transform().Y() - 6.0f,
                                            46.0f, 0.25f, 85.0f, 45.0f, 6.0f, 0.0f,
                                            AbyssFx::Color{255, 205, 115, 210}, AbyssFx::Color{255, 100, 100, 0}, 0.08f);
                    }
                }
                break;
            }
            case State::Telegraph:
                vx *= 0.45f;
                if (state_time <= 0.0f) {
                    SetState(State::Lunge, 0.22f);
                    attack_cd = 1.10f;
                    rb.SetVelocity({facing * (speed * 3.65f), 0.0f});
                    AbyssFx::Dust(this, Transform().X(), Transform().Y() + 12.0f, 0.9f);
                }
                break;
            case State::Lunge:
                vx = facing * speed * 3.65f;
                if (state_time <= 0.0f) SetState(State::Recover, 0.42f);
                break;
            case State::Recover:
                vx *= 0.72f;
                if (state_time <= 0.0f) SetState(State::Position, 0.0f);
                break;
            case State::Stagger:
                vx *= 0.40f;
                if (state_time <= 0.0f) SetState(State::Position, 0.0f);
                break;
        }
        float vy = rb.VY();
        const bool airborne_variant = entity ? entity.Value("airborne_variant", false) : false;
        if (!airborne_variant) {
            // Down is positive in the showcase's screen-space coordinates. Keep a
            // small downward contact velocity on a floor, and a bounded fall
            // speed otherwise; this is the missing gravity contract behind
            // crawling enemies appearing to hover over their platforms.
            vy = grounded ? Clamp(vy, 0.0f, 80.0f) : Min(vy + 1210.0f * dt, 900.0f);
        }
        rb.SetVelocity({vx, vy});
    }

    void ApplyVisuals() {
        auto sprite = GetComponent("SpriteRenderer");
        if (!sprite) return;
        // crawler_abyss.png is a four-frame 64x64 strip.  Leaving the
        // source rectangle disabled renders every frame side-by-side, which
        // looked like four enemies glued together during combat.
        int frame = 0;
        if (state == State::Telegraph) frame = 2 + ((int)(Time::elapsed_time * 14.0f) & 1);
        else if (state == State::Lunge) frame = 3;
        else if (state == State::Recover || state == State::Stagger) frame = 2;
        else frame = ((int)(Time::elapsed_time * 7.0f) & 1);
        sprite.SetValue("use_source_rect", true);
        sprite.SetValue("source_x", frame * 64);
        sprite.SetValue("source_y", 0);
        sprite.SetValue("source_w", 64);
        sprite.SetValue("source_h", 64);
        sprite.SetValue("flip_x", facing < 0.0f);
        if (state == State::Telegraph) {
            const int glow = 155 + (int)(100.0f * (0.5f + 0.5f * Sin((float)Time::elapsed_time * 24.0f)));
            sprite.SetValue("color", vector<int>{255, glow, 110, 255});
        } else if (flash_time > 0.0f) {
            sprite.SetValue("color", vector<int>{255, 235, 235, 255});
        } else {
            sprite.SetValue("color", vector<int>{255, 255, 255, 255});
        }
    }

    void TakeDamage(EntityRef other) {
        if (!other || damage_lock > 0.0f || other.Value("team", 0) == 2) return;
        if (!other.Contains("damage")) return;
        damage_lock = 0.10f;
        const int dmg = Max(1, other.Value("damage", 1));
        const uint32_t net_id = entity ? Replication::NetIdOf(entity) : 0;
        if (net_id != 0) Replication::RequestDamage(net_id, (float)dmg, Network::LocalPeerId(), Transform().X(), Transform().Y());
        else hp -= dmg;
        if (entity) entity["hp"] = hp;
        flash_time = 0.10f;
        SetState(State::Stagger, 0.16f);
        if (auto rb = Rigidbody()) rb.AddImpulse({-facing * 125.0f, -75.0f});
        AbyssFx::HitSpark(this, Transform().X(), Transform().Y(), AbyssFx::Color{235,245,255,255}, 0.9f);
    }

    void Die() {
        if (dying) return;
        dying = true;
        AbyssFx::Explosion(this, Transform().X(), Transform().Y(), AbyssFx::Color{200,90,110,255}, 1.0f);
        Destroy();
    }
};
