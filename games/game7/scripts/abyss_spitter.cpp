#include "../../../engine_cpp/script_system.hpp"
#include "../../../engine_cpp/net/replication_rpc.hpp"
#include "../../../engine_cpp/net/replication.hpp"
#include "abyss_fx.hpp"
#include <cmath>

// Ranged enemy: it keeps a readable distance, plants its feet for a visible
// charge, then fires a single parryable shard.  It no longer attacks with no
// recovery or simply drifts forever.
class AbyssSpitter : public MonoBehaviour {
public:
    void Awake() override {
        hp = entity ? entity.Value("hp", 4) : 4;
        if (entity) entity["hp"] = hp;
        fire_range = entity ? entity.Value("fire_range", 980.0f) : 980.0f;
        if (auto tpl = Find("AbyssShardTemplate")) shard_template_id = tpl.Value("id", -1);
        EXPOSE_FIELD(hp);
        EXPOSE_FIELD(fire_range);
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
            // Spitters are floor-mounted ranged enemies, not floating
            // turrets. Their controller owns gravity just like AbyssPlayer.
            entity["components"]["Rigidbody2D"]["gravity_scale"] = 0.0f;
            entity["components"]["Rigidbody2D"]["continuous_collision"] = true;
        }
    }

    void Update(float dt) override {
        if (entity) hp = entity.Value("hp", hp);
        hurt_lock = Max(0.0f, hurt_lock - dt);
        cooldown = Max(0.0f, cooldown - dt);
        state_time = Max(0.0f, state_time - dt);
        if (hp <= 0) { Die(); return; }

        auto player = Find("AbyssPlayer");
        const float dx = player ? GetX(player, Transform().X()) - Transform().X() : 99999.0f;
        const float dy = player ? GetY(player, Transform().Y()) - Transform().Y() : 0.0f;
        if (Abs(dx) > 1.0f) facing = dx < 0.0f ? -1.0f : 1.0f;
        auto shard = FindById(shard_template_id);
        if (!shard) { shard = Find("AbyssShardTemplate"); if (shard) shard_template_id = shard.Value("id", -1); }

        if (!(Network::IsClient() && !Network::IsHost())) RunBrain(dx, dy, shard, dt);
        ApplyVisuals();
    }

    void OnTriggerEnter2D(EntityRef other) override { TakeDamage(other); }
    void OnCollisionEnter2D(EntityRef other) override { TakeDamage(other); }

private:
    enum class State { Position, Charge, Recover, Stagger };
    State state = State::Position;
    int hp = 4, shard_template_id = -1;
    float fire_range = 980.0f, cooldown = 0.45f, state_time = 0.0f;
    float facing = 1.0f, hurt_lock = 0.0f, flash_time = 0.0f;
    float aim_x = 1.0f, aim_y = 0.0f;
    bool dying = false;
    string asset_dir_;

    EntityRef FindById(int id) {
        if (id < 0) return EntityRef();
        for (EntityRef e : entities()) if (e.value("id", 0) == id) return e;
        return EntityRef();
    }
    static float GetX(EntityRef e, float fallback) {
        return e && e.Contains("components") && e["components"].contains("Transform") ? e["components"]["Transform"].value("x", fallback) : fallback;
    }
    static float GetY(EntityRef e, float fallback) {
        return e && e.Contains("components") && e["components"].contains("Transform") ? e["components"]["Transform"].value("y", fallback) : fallback;
    }
    void SetState(State next, float duration) { state = next; state_time = duration; }

    void RunBrain(float dx, float dy, EntityRef shard, float dt) {
        auto rb = Rigidbody();
        if (!rb) return;
        float vx = rb.VX();
        const float distance = Sqrt(dx * dx + dy * dy);
        switch (state) {
            case State::Position: {
                float desired = 0.0f;
                if (distance < 240.0f) desired = -facing * 140.0f;
                else if (distance > 440.0f && distance < fire_range) desired = facing * 105.0f;
                vx += (desired - vx) * Min(1.0f, dt * 6.0f);
                if (distance < fire_range && Abs(dy) < 300.0f && cooldown <= 0.0f && shard) {
                    aim_x = dx; aim_y = dy;
                    SetState(State::Charge, 0.42f);
                    AbyssFx::SpawnBurst(this, Transform().X() + facing * 13.0f, Transform().Y() - 12.0f,
                                        55.0f, 0.30f, 70.0f, 360.0f, 6.0f, 0.0f,
                                        AbyssFx::Color{135,255,175,230}, AbyssFx::Color{70,180,115,0}, 0.11f);
                }
                break;
            }
            case State::Charge:
                vx *= 0.25f;
                if (state_time <= 0.0f) {
                    Fire(shard, aim_x, aim_y);
                    cooldown = 1.35f;
                    SetState(State::Recover, 0.30f);
                }
                break;
            case State::Recover:
                vx *= 0.65f;
                if (state_time <= 0.0f) SetState(State::Position, 0.0f);
                break;
            case State::Stagger:
                vx *= 0.42f;
                if (state_time <= 0.0f) SetState(State::Position, 0.0f);
                break;
        }
        const auto ground = phys::query_ground_info(entity);
        float vy = rb.VY();
        vy = ground.grounded ? Clamp(vy, 0.0f, 80.0f) : Min(vy + 1210.0f * dt, 900.0f);
        rb.SetVelocity({vx, vy});
    }

    void Fire(EntityRef shard, float dx, float dy) {
        if (!shard) return;
        const float len = Max(1.0f, Sqrt(dx * dx + dy * dy));
        dx /= len; dy /= len;
        EntityRef bolt;
        const string prefab = shard.Value("prefab_source", string());
        bolt = prefab.empty() ? Instantiate(shard, Transform().X() + facing * 14.0f, Transform().Y() - 9.0f)
                              : Replication::Spawn(entities(), asset_dir_, prefab, Transform().X() + facing * 14.0f, Transform().Y() - 9.0f);
        if (!bolt) return;
        bolt["active"] = true; bolt["team"] = 2; bolt["damage"] = 1;
        bolt["dir_x"] = dx; bolt["dir_y"] = dy; bolt["speed"] = 650.0f;
        bolt["life_time"] = 1.45f; bolt["parryable"] = true;
        AbyssFx::EnergyPuff(this, Transform().X(), Transform().Y(), AbyssFx::Color{120,220,140,255});
    }

    void ApplyVisuals() {
        auto sprite = GetComponent("SpriteRenderer");
        if (!sprite) return;
        // spitter_abyss.png also contains four 64x64 frames.  Always choose
        // one source rectangle so a combatant is a single readable sprite.
        int frame = 0;
        if (state == State::Charge) frame = 2 + ((int)(Time::elapsed_time * 16.0f) & 1);
        else if (state == State::Recover || state == State::Stagger) frame = 2;
        else frame = ((int)(Time::elapsed_time * 6.0f) & 1);
        sprite.SetValue("use_source_rect", true);
        sprite.SetValue("source_x", frame * 64);
        sprite.SetValue("source_y", 0);
        sprite.SetValue("source_w", 64);
        sprite.SetValue("source_h", 64);
        sprite.SetValue("flip_x", facing < 0.0f);
        if (state == State::Charge) {
            int pulse = 150 + (int)(105.0f * (0.5f + 0.5f * Sin((float)Time::elapsed_time * 25.0f)));
            sprite.SetValue("color", vector<int>{pulse,255,145,255});
        } else if (flash_time > 0.0f) sprite.SetValue("color", vector<int>{255,235,235,255});
        else sprite.SetValue("color", vector<int>{255,255,255,255});
        flash_time = Max(0.0f, flash_time - (float)Time::delta_time);
    }

    void TakeDamage(EntityRef other) {
        if (!other || hurt_lock > 0.0f || other.Value("team", 0) == 2 || !other.Contains("damage")) return;
        hurt_lock = 0.10f;
        const int dmg = Max(1, other.Value("damage", 1));
        const uint32_t id = entity ? Replication::NetIdOf(entity) : 0;
        if (id != 0) Replication::RequestDamage(id, (float)dmg, Network::LocalPeerId(), Transform().X(), Transform().Y());
        else hp -= dmg;
        if (entity) entity["hp"] = hp;
        flash_time = 0.10f; SetState(State::Stagger, 0.18f);
        if (auto rb = Rigidbody()) rb.AddImpulse({-facing * 120.0f, -60.0f});
        AbyssFx::HitSpark(this, Transform().X(), Transform().Y(), AbyssFx::Color{235,245,255,255}, 0.9f);
    }

    void Die() {
        if (dying) return;
        dying = true;
        AbyssFx::Explosion(this, Transform().X(), Transform().Y(), AbyssFx::Color{120,220,140,255}, 1.0f);
        Destroy();
    }
};
