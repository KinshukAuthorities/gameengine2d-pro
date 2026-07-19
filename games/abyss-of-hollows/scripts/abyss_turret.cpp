#include "../../../engine_cpp/script_system.hpp"
#include "../../../engine_cpp/net/replication_rpc.hpp"
#include "../../../engine_cpp/net/replication.hpp"
#include "abyss_fx.hpp"
#include <cmath>
#include <algorithm>

class AbyssTurret : public MonoBehaviour {
public:
    void Awake() override {
        hp = entity ? entity.Value("hp", 3) : 3;
        if (entity) entity["hp"] = hp;
        damage = entity ? entity.Value("damage", 1) : 1;
        fire_cd = entity ? entity.Value("fire_cd", 0.9f) : 0.9f;
        fire_range = entity ? entity.Value("fire_range", 760.0f) : 760.0f;
        bolt_template_id = -1;
        auto tpl = Find("AbyssShardTemplate");
        bolt_template_id = tpl ? tpl.Value("id", -1) : -1;
        auto p = Find("AbyssPlayer");
        player_id = p ? p.Value("id", -1) : -1;
        base_rotation = Transform().Rotation();
        EXPOSE_FIELD(fire_range);

        EventBus::instance().subscribe("net_health", [this](EntityRef data, EntityRef target) {
            if (!target || target != entity) return;
            hp = (int)data.value("current_health", (float)hp);
            target["hp"] = hp;
        });
    }

    void Start() override {
        // FIX (frozen enemies): disable physics engine gravity and sleep to prevent
        // interference with script-driven movement (same fix as AbyssPlayer)
        if (entity && entity.Contains("components") && entity["components"].contains("Rigidbody2D")) {
            entity["components"]["Rigidbody2D"]["allow_sleep"] = false;
            // Turrets are intentionally anchored.  Leaving their authored
            // default gravity active made them fall away from their socket in
            // longer encounters, then appear to be frozen or untouchable.
            entity["components"]["Rigidbody2D"]["gravity_scale"] = 0.0f;
            entity["components"]["Rigidbody2D"]["continuous_collision"] = true;
        }
    }

    void Update(float dt) override {
        if (entity) hp = entity.Value("hp", hp);
        if (hp <= 0) { Destroy(); return; }
        fire_cd = Max(0.0f, fire_cd - dt);
        telegraph_timer = Max(0.0f, telegraph_timer - dt);
        hurt_lock = Max(0.0f, hurt_lock - dt);

        auto player = FindById(player_id);
        if (!player) {
            player = Find("AbyssPlayer");
            if (player) player_id = player.Value("id", -1);
        }
        auto bolt_template = FindById(bolt_template_id);
        if (!bolt_template) {
            bolt_template = Find("AbyssShardTemplate");
            if (bolt_template) bolt_template_id = bolt_template.Value("id", -1);
        }

        float px = player ? GetX(player, Transform().X()) : Transform().X();
        float py = player ? GetY(player, Transform().Y()) : Transform().Y();
        float dx = px - Transform().X();
        float dy = py - Transform().Y();

        bool host = Network::IsHost() || !Network::IsClient();

        if (host && Abs(dx) < fire_range && py > Transform().Y() - 220.0f && bolt_template) {
            if (fire_cd <= 0.0f && !pending_shot) {
                pending_shot = true;
                pending_dx = dx;
                pending_dy = dy;
                telegraph_timer = 0.26f;
                AbyssFx::EnergyPuff(this, Transform().X(), Transform().Y() - 18.0f, AbyssFx::Color{255, 215, 120, 255});
            }
            if (pending_shot && telegraph_timer <= 0.0f) {
                Fire(bolt_template, pending_dx, pending_dy);
                pending_shot = false;
                fire_cd = 1.1f;
            }
        }

        auto sr = GetComponent("SpriteRenderer");
        if (sr) {
            sr.SetValue("flip_x", dx < 0.0f);
            sr.SetValue("color", telegraph_timer > 0.0f ? vector<int>{255, 215, 120, 255} : vector<int>{255, 255, 255, 255});
        }
        // Rotate() compounded the bobbing amount every frame, so a turret
        // slowly spun forever.  Set an offset from its authored angle instead.
        Transform().SetRotation(base_rotation +
            Sin((float)Time::elapsed_time * 1.3f + Transform().X() * 0.01f) * 1.5f);
    }

    void OnTriggerEnter2D(EntityRef other) override { TakeDamage(other); }
    void OnCollisionEnter2D(EntityRef other) override { TakeDamage(other); }

private:
    int hp = 0;
    int damage = 0;
    float fire_cd = 0.0f;
    float fire_range = 0.0f;
    int bolt_template_id = -1;
    int player_id = -1;
    float telegraph_timer = 0.0f;
    float pending_dx = 0.0f;
    float pending_dy = 0.0f;
    bool pending_shot = false;
    float hurt_lock = 0.0f;
    float base_rotation = 0.0f;

    EntityRef FindById(int id) {
        if (id < 0) return EntityRef();
        for (EntityRef e : entities())
            if (e.value("id", 0) == id) return e;
        return EntityRef();
    }

    static float GetX(EntityRef e, float def) {
        if (!e || !e.Contains("components") || !e["components"].contains("Transform")) return def;
        return e["components"]["Transform"].value("x", def);
    }
    static float GetY(EntityRef e, float def) {
        if (!e || !e.Contains("components") || !e["components"].contains("Transform")) return def;
        return e["components"]["Transform"].value("y", def);
    }

    void Fire(EntityRef bolt_template, float dx, float dy) {
        float len = Sqrt(dx * dx + dy * dy);
        if (len < 0.001f) { dx = -1.0f; dy = 0.0f; len = 1.0f; }
        dx /= len; dy /= len;
        dy = Max(0.35f, dy);

        EntityRef b;
        {
            string prefab = bolt_template.Value("prefab_source", string());
            b = prefab.empty()
                ? Instantiate(bolt_template, Transform().X(), Transform().Y() - 18.0f)
                : Replication::Spawn(entities(), asset_dir_,
                                     prefab, Transform().X(), Transform().Y() - 18.0f);
        }
        if (!b) return;
        b["active"] = true;
        b["team"] = 2;
        b["damage"] = 1;
        b["dir_x"] = dx * 0.35f;
        b["dir_y"] = dy;
        b["speed"] = 520.0f;
        b["life_time"] = 1.5f;
        b["parryable"] = true;
        AbyssFx::EnergyPuff(this, Transform().X(), Transform().Y() - 18.0f, AbyssFx::Color{160, 90, 255, 255});
    }

    void TakeDamage(EntityRef other) {
        if (!other || hurt_lock > 0.0f) return;
        if (!other.Contains("team") || !other.Contains("damage")) return;
        if (other.Value("team", 0) == 2) return;
        hurt_lock = 0.10f;

        uint32_t my_net_id = entity ? Replication::NetIdOf(entity) : 0;
        int dmg = Max(1, other.Value("damage", 1));
        if (my_net_id != 0) {
            Replication::RequestDamage(my_net_id, (float)dmg,
                                        Network::LocalPeerId(),
                                        Transform().X(), Transform().Y());
        } else {
            hp -= dmg;
        }
        if (entity) entity["hp"] = hp;

        AbyssFx::HitSpark(this, Transform().X(), Transform().Y(), AbyssFx::Color{235, 245, 255, 255}, 0.8f);
        if (hp <= 0) {
            AbyssFx::Explosion(this, Transform().X(), Transform().Y(), AbyssFx::Color{160, 90, 255, 255}, 1.0f);
            Destroy();
        }
    }

    string asset_dir_;

};
