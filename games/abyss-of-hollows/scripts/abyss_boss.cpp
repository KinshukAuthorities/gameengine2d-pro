#include <cmath>
#include <algorithm>
#include "../../../engine_cpp/script_system.hpp"
#include "../../../engine_cpp/net/replication_rpc.hpp"
#include "../../../engine_cpp/net/replication_props.hpp"
#include "../../../engine_cpp/net/replication.hpp"
#include "abyss_shared.hpp"
#include "abyss_fx.hpp"
class AbyssBoss : public MonoBehaviour {
public:
    void Awake() override {
        hp = entity ? entity.Value("hp", 96) : 96;
        if (entity) entity["hp"] = hp;
        team = entity ? entity.Value("team", 2) : 2;
        damage = entity ? entity.Value("damage", 2) : 2;
        attack_cd = 0.0f;
        summon_cd = 0.0f;
        phase = 0;
        last_phase = 0;
        phase_flash_timer = 0.0f;
        seed_phase = 0.0f;
        telegraph_timer = 0.0f;
        telegraph_pattern = 0;
        dash_timer = 0.0f;
        hit_lock = 0.0f;
        defeated = false;
        anim_time = 0.0f;
        base_rotation = Transform().Rotation();
        dash_trail_emitted = false;

        // boss_abyss.png is a six-frame horizontal strip.  Never leave its
        // renderer at the full texture: that was the source of the row of
        // duplicate-looking boss sprites in standalone builds.
        if (auto sprite = GetComponent("SpriteRenderer")) {
            sprite.SetValue("use_source_rect", true);
            sprite.SetValue("source_w", 64);
            sprite.SetValue("source_h", 64);
            sprite.SetValue("source_y", 0);
            sprite.SetValue("source_x", 0);
        }

        auto tpl = Find("AbyssShardTemplate");
        shard_template_id = tpl ? tpl.Value("id", -1) : -1;
        auto ctpl = Find("AbyssCrawlerTemplate");
        crawler_template_id = ctpl ? ctpl.Value("id", -1) : -1;
        auto p = Find("AbyssPlayer");
        player_id = p ? p.Value("id", -1) : -1;

        EXPOSE_FIELD(hp);

        // Listen for authoritative health sync (clients read hp from net_health).
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
            // The Warden is a hovering arena opponent. Leaving the authored
            // default gravity enabled makes its AI fight the solver and can
            // drop it through the arena after a long fight.
            entity["components"]["Rigidbody2D"]["gravity_scale"] = 0.0f;
            entity["components"]["Rigidbody2D"]["continuous_collision"] = true;
        }
        EnsureBossHud();
    }

    void Update(float dt) override {
        if (entity) hp = entity.Value("hp", hp);
        if (hp <= 0) { Defeat(); return; }

        anim_time += dt;

        attack_cd = Max(0.0f, attack_cd - dt);
        summon_cd = Max(0.0f, summon_cd - dt);
        hit_lock = Max(0.0f, hit_lock - dt);
        telegraph_timer = Max(0.0f, telegraph_timer - dt);
        dash_timer = Max(0.0f, dash_timer - dt);

        auto player = FindById(player_id);
        if (!player) {
            player = Find("AbyssPlayer");
            if (player) player_id = player.Value("id", -1);
        }

        auto shard_template = FindById(shard_template_id);
        if (!shard_template) {
            shard_template = Find("AbyssShardTemplate");
            if (shard_template) shard_template_id = shard_template.Value("id", -1);
        }
        auto crawler_template = FindById(crawler_template_id);
        if (!crawler_template) {
            crawler_template = Find("AbyssCrawlerTemplate");
            if (crawler_template) crawler_template_id = crawler_template.Value("id", -1);
        }

        float px = player ? GetX(player, Transform().X()) : Transform().X();
        float py = player ? GetY(player, Transform().Y()) : Transform().Y();
        float dx = px - Transform().X();
        float dy = py - Transform().Y();
        float dist = Sqrt(dx * dx + dy * dy);

        int new_phase = (hp > 70) ? 0 : (hp > 35 ? 1 : 2);

        if (new_phase != phase) {
            phase = new_phase;
            last_phase = phase;
            AbyssFx::Explosion(this, Transform().X(), Transform().Y(),
                                phase == 2 ? AbyssFx::Color{255, 90, 90, 255} : AbyssFx::Color{200, 120, 255, 255},
                                1.4f);
            phase_flash_timer = 0.35f;

            // Replicate phase change so all peers trigger effects.
            if (entity) {
                Replication::ReplicateGroup(entity, {{"phase", phase}, {"enrage", phase == 2}});
            }
        }
        phase_flash_timer = Max(0.0f, phase_flash_timer - dt);

        // Only the host runs AI logic (movement, attacks, summons).
        if (!Network::IsHost() && Network::IsClient()) {
            // Clients: just update visuals from replicated state.
            ApplyVisuals(dx, dt);
            return;
        }

        if (dash_timer > 0.0f) {
            if (auto rb = Rigidbody()) rb.SetVelocity({dash_dx * 520.0f, dash_dy * 420.0f});
            if (!dash_trail_emitted) {
                dash_trail_emitted = true;
                AbyssFx::SpawnBurst(this, Transform().X(), Transform().Y(),
                                    120.0f, 0.20f, 180.0f, 360.0f,
                                    8.0f, 0.0f, AbyssFx::Color{255, 120, 180, 255}, AbyssFx::Color{180, 80, 255, 0}, 0.06f);
            }
        } else if (telegraph_timer > 0.0f) {
            if (auto rb = Rigidbody()) rb.SetVelocity({0.0f, 0.0f});
        } else if (dist > 1200.0f) {
            float target_x = (Transform().X() < px) ? 90.0f : -90.0f;
            if (auto rb = Rigidbody()) rb.SetVelocity({target_x, 0.0f});
        } else {
            float hover_x = Clamp(dx * 0.55f, -160.0f, 160.0f);
            float hover_y = Clamp(dy * 0.22f, -90.0f, 90.0f);
            if (phase == 2) hover_y *= 1.25f;
            if (auto rb = Rigidbody()) rb.SetVelocity({hover_x, hover_y});
        }

        // Every attack gets a visible wind-up.  The old loop immediately
        // emitted a spread as soon as its cooldown expired, which made the
        // encounter feel random and hid the player's parry window.
        if (dist <= 960.0f && telegraph_timer <= 0.0f && dash_timer <= 0.0f && attack_cd <= 0.0f) {
            telegraph_pattern = ChoosePattern();
            dash_trail_emitted = false;
            telegraph_timer = phase == 0 ? 0.42f : (phase == 1 ? 0.34f : 0.27f);
            attack_cd = phase == 0 ? 1.28f : (phase == 1 ? 1.02f : 0.84f);
            AbyssFx::SpawnBurst(this, Transform().X(), Transform().Y(),
                                60.0f, telegraph_timer, 70.0f, 360.0f,
                                6.0f, 0.0f, AbyssFx::Color{255, 215, 130, 230}, AbyssFx::Color{255, 80, 150, 0}, 0.08f);
        }
        if (telegraph_pattern != 0 && telegraph_timer <= 0.0f && attack_cd > 0.0f && dash_timer <= 0.0f) {
            PerformPattern(shard_template, crawler_template, dx, dy);
            telegraph_pattern = 0;
        }

        ApplyVisuals(dx, dt);
    }

    void OnTriggerEnter2D(EntityRef other) override { Hurt(other); }
    void OnCollisionEnter2D(EntityRef other) override { Hurt(other); }

private:
    int hp = 0;
    int team = 2;
    int damage = 2;
    float attack_cd = 0.0f;
    float summon_cd = 0.0f;
    int phase = 0;
    int last_phase = 0;
    float phase_flash_timer = 0.0f;
    float seed_phase = 0.0f;
    float telegraph_timer = 0.0f;
    int telegraph_pattern = 0;
    float dash_timer = 0.0f;
    float dash_dx = 0.0f;
    float dash_dy = 0.0f;
    float hit_lock = 0.0f;
    float anim_time = 0.0f;
    float base_rotation = 0.0f;
    bool defeated = false;
    bool dash_trail_emitted = false;
    int shard_template_id = -1;
    int crawler_template_id = -1;
    int player_id = -1;

    Entity Rgba(int r, int g, int b, int a = 255) {
        Entity c = Entity::array();
        c.push_back(r); c.push_back(g); c.push_back(b); c.push_back(a);
        return c;
    }

    int NextHudId() {
        int next = 56000;
        for (const auto& e : entities()) next = Max(next, e.value("id", 0) + 1);
        return next;
    }

    void EnsureBossHud() {
        if (Find("BossHealthBar")) return;
        int next = NextHudId();
        Entity frame = Entity::object();
        frame["id"] = next++; frame["name"] = "BossHealthFrame"; frame["active"] = true; frame["children"] = Entity::array();
        frame["components"] = Entity::object(); frame["components"]["Transform"] = {{"x",0.0},{"y",0.0},{"rotation",0.0},{"scale_x",1.0},{"scale_y",1.0}};
        frame["components"]["UICanvas"] = Entity::object();
        frame["components"]["UIPanel"] = {{"anchor_x",0.5},{"anchor_y",0.0},{"pivot_x",0.5},{"pivot_y",0.0},{"pos_x",0.0},{"pos_y",34.0},{"width",470.0},{"height",52.0},{"color",Rgba(22,8,20,225)},{"border_color",Rgba(224,120,155,235)},{"border_width",2},{"responsive",true},{"responsive_fit",true},{"reference_width",1280},{"reference_height",720},{"min_scale",0.55},{"max_scale",1.35}};
        entities().push_back(frame);

        Entity bar = Entity::object();
        bar["id"] = next++; bar["name"] = "BossHealthBar"; bar["active"] = true; bar["children"] = Entity::array();
        bar["components"] = Entity::object(); bar["components"]["Transform"] = {{"x",0.0},{"y",0.0},{"rotation",0.0},{"scale_x",1.0},{"scale_y",1.0}};
        bar["components"]["UICanvas"] = Entity::object();
        bar["components"]["UIProgressBar"] = {{"anchor_x",0.5},{"anchor_y",0.0},{"pivot_x",0.5},{"pivot_y",0.0},{"pos_x",0.0},{"pos_y",58.0},{"width",432.0},{"height",14.0},{"min",0.0},{"max",100.0},{"value",(float)hp},{"bg_color",Rgba(42,12,31,255)},{"fill_color",Rgba(230,82,124,255)},{"responsive",true},{"responsive_fit",true},{"reference_width",1280},{"reference_height",720},{"min_scale",0.55},{"max_scale",1.35}};
        entities().push_back(bar);

        Entity title = Entity::object();
        title["id"] = next++; title["name"] = "BossHealthTitle"; title["active"] = true; title["children"] = Entity::array();
        title["components"] = Entity::object(); title["components"]["Transform"] = {{"x",0.0},{"y",0.0},{"rotation",0.0},{"scale_x",1.0},{"scale_y",1.0}};
        title["components"]["UICanvas"] = Entity::object();
        title["components"]["UIText"] = {{"anchor_x",0.5},{"anchor_y",0.0},{"pivot_x",0.5},{"pivot_y",0.0},{"pos_x",0.0},{"pos_y",36.0},{"width",430.0},{"height",20},{"text","THE HOLLOW WARDEN"},{"font_size",17},{"bold",true},{"shadow",true},{"align","center"},{"v_align","middle"},{"color",Rgba(255,220,230,255)},{"responsive",true},{"responsive_fit",true},{"reference_width",1280},{"reference_height",720},{"min_scale",0.55},{"max_scale",1.35}};
        entities().push_back(title);
    }

    void RefreshBossHud() {
        if (auto bar = Find("BossHealthBar")) {
            auto& p = bar["components"]["UIProgressBar"];
            p["max"] = (float)Max(1, entity ? entity.Value("max_hp", 100) : 100);
            p["value"] = (float)Max(0, hp);
        }
        if (auto title = Find("BossHealthTitle")) {
            const char* phase_name = phase == 0 ? "THE HOLLOW WARDEN" : phase == 1 ? "THE HOLLOW WARDEN  -  AWAKENED" : "THE HOLLOW WARDEN  -  LAST LIGHT";
            title["components"]["UIText"]["text"] = phase_name;
        }
    }

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

    void ApplyVisuals(float dx, float dt) {
        RefreshBossHud();
        if (phase == 2) {
            Transform().SetRotation(base_rotation + Sin((float)Time::elapsed_time * 2.0f) * 1.8f);
            auto sr = GetComponent("SpriteRenderer");
            if (sr) sr.SetValue("opacity", 0.92f + 0.08f * Sin((float)Time::elapsed_time * 5.0f));
        } else {
            Transform().SetRotation(base_rotation + Sin((float)Time::elapsed_time * 1.4f) * 0.8f);
        }

        auto spr = GetComponent("SpriteRenderer");
        if (spr) {
            // Idle/pressure/telegraph frames are selected explicitly from a
            // 6 x 64px strip.  The direct source rect remains stable even if
            // a scene has no Animator component.
            int frame = 0;
            if (dash_timer > 0.0f) frame = 4 + ((int)(anim_time * 18.0f) & 1);
            else if (telegraph_timer > 0.0f) frame = 2 + ((int)(anim_time * 13.0f) & 1);
            else frame = (int)(anim_time * (phase == 2 ? 9.0f : 6.0f)) & 1;
            spr.SetValue("use_source_rect", true);
            spr.SetValue("source_x", frame * 64);
            spr.SetValue("source_y", 0);
            spr.SetValue("source_w", 64);
            spr.SetValue("source_h", 64);
            spr.SetValue("flip_x", dx < 0.0f);
            if (telegraph_timer > 0.0f) {
                float flash = 0.5f + 0.5f * Sin((float)Time::elapsed_time * 28.0f);
                spr.SetValue("color", vector<int>{255, (int)(145 + 90 * flash), (int)(120 + 100 * flash), 255});
            } else if (phase_flash_timer > 0.0f) {
                float t = phase_flash_timer / 0.35f;
                int v = 255;
                int base = (int)Round((1.0f - t) * 255.0f);
                spr.SetValue("color", vector<int>{v, (int)Max(base, 120), (int)Max(base, 120), 255});
            } else {
                spr.SetValue("color", vector<int>{255, 255, 255, 255});
            }
        }
    }

    void FireSpread(EntityRef shard_template, float dx, float dy, int count, float spread, float speed) {
        if (CountActiveProjectiles() >= 28) return;
        float len = Sqrt(dx * dx + dy * dy);
        if (len < 0.001f) { dx = -1.0f; dy = 0.0f; len = 1.0f; }
        dx /= len; dy /= len;

        AbyssFx::SpawnBurst(this, Transform().X(), Transform().Y(),
                             140.0f, 0.18f, 220.0f, 360.0f,
                             7.0f, 0.0f, AbyssFx::Color{255, 120, 120, 255}, AbyssFx::Color{160, 60, 90, 0}, 0.05f);

        for (int i = 0; i < count; ++i) {
            float ang = (count == 1) ? 0.0f : (i - (count - 1) * 0.5f) * spread;
            float sx = dx * Cos(ang) - dy * Sin(ang);
            float sy = dx * Sin(ang) + dy * Cos(ang);
            // Host spawns shards via Replication::Spawn so all peers get them.
            EntityRef b;
            uint32_t net_id = entity ? Replication::NetIdOf(entity) : 0;
            if (net_id != 0) {
                b = Replication::Spawn(entities(), asset_dir_,
                                       shard_template.Value("prefab_source", string()),
                                       Transform().X(), Transform().Y());
            } else {
                b = Instantiate(shard_template, Transform().X(), Transform().Y());
            }
            if (!b) continue;
            b["active"] = true;
            b["team"] = 2;
            b["damage"] = 1;
            b["dir_x"] = sx;
            b["dir_y"] = sy;
            b["speed"] = speed;
            b["life_time"] = 1.2f;
            b["_destroy_timer"] = 1.34f;
        }
    }

    void FireRing(EntityRef shard_template, int count, float speed) {
        if (!shard_template || CountActiveProjectiles() >= 24) return;
        AbyssFx::SpawnBurst(this, Transform().X(), Transform().Y(),
                             180.0f, 0.18f, 170.0f, 360.0f,
                             8.0f, 0.0f, AbyssFx::Color{215, 120, 255, 255}, AbyssFx::Color{180, 80, 255, 0}, 0.06f);
        for (int i = 0; i < count; ++i) {
            const float a = (float)i * 6.2831853f / (float)count + anim_time * 0.42f;
            EntityRef shard;
            const uint32_t net_id = entity ? Replication::NetIdOf(entity) : 0;
            if (net_id != 0) {
                shard = Replication::Spawn(entities(), asset_dir_, shard_template.Value("prefab_source", string()),
                                           Transform().X(), Transform().Y());
            } else {
                shard = Instantiate(shard_template, Transform().X(), Transform().Y());
            }
            if (!shard) continue;
            shard["active"] = true;
            shard["team"] = 2;
            shard["damage"] = 1;
            shard["dir_x"] = Cos(a);
            shard["dir_y"] = Sin(a);
            shard["speed"] = speed;
            shard["life_time"] = 1.45f;
            shard["_destroy_timer"] = 1.58f;
            shard["parryable"] = true;
        }
    }

    void SummonCrawler(EntityRef crawler_template) {
        float offset_x = 60.0f * ((Sin((float)Time::elapsed_time) > 0.0f) ? 1.0f : -1.0f);
        EntityRef c;
        uint32_t net_id = entity ? Replication::NetIdOf(entity) : 0;
        if (net_id != 0) {
            c = Replication::Spawn(entities(), asset_dir_,
                                   crawler_template.Value("prefab_source", string()),
                                   Transform().X() + offset_x, Transform().Y() + 22.0f);
        } else {
            c = Instantiate(crawler_template, Transform().X() + offset_x, Transform().Y() + 22.0f);
        }
        if (!c) return;
        c["active"] = true;
        c["hp"] = 3;
        c["damage"] = 1;
        c["patrol_range"] = 170.0f;
        c["speed"] = 110.0f;
        c["abyss_summoned"] = true;
    }

    int ChoosePattern() const {
        const int pulse = (int)(Time::elapsed_time * 1.7f) + phase;
        if (phase == 0) return 1;                    // three crystal bolts
        if (phase == 1) return (pulse % 2 == 0) ? 2 : 1; // dash or spread
        return (pulse % 4) + 1;                      // spread, dash, summon, ring
    }

    void PerformPattern(EntityRef shard_template, EntityRef crawler_template, float dx, float dy) {
        if (telegraph_pattern == 1 && shard_template) {
            const int count = phase == 0 ? 3 : (phase == 1 ? 5 : 7);
            FireSpread(shard_template, dx, dy, count, phase == 2 ? 0.10f : 0.14f,
                       phase == 2 ? 790.0f : (phase == 1 ? 710.0f : 630.0f));
        } else if (telegraph_pattern == 2) {
            const float len = Max(1.0f, Sqrt(dx * dx + dy * dy));
            dash_dx = dx / len;
            dash_dy = Clamp(dy / len, -0.55f, 0.55f);
            dash_timer = phase == 2 ? 0.34f : 0.27f;
            AbyssFx::SpawnBurst(this, Transform().X(), Transform().Y(),
                                120.0f, 0.22f, 180.0f, 360.0f,
                                9.0f, 0.0f, AbyssFx::Color{255, 110, 175, 255}, AbyssFx::Color{255, 80, 130, 0}, 0.05f);
        } else if (telegraph_pattern == 3 && crawler_template && summon_cd <= 0.0f && CountActiveSummons() < 3) {
            SummonCrawler(crawler_template);
            summon_cd = phase == 2 ? 4.2f : 5.8f;
        } else if (telegraph_pattern == 4 && shard_template) {
            // Last-light phase: an evenly-spaced, deliberately slow ring is
            // a readable positioning puzzle and gives the player real parry
            // opportunities instead of another indistinct spread.
            FireRing(shard_template, 8, 470.0f);
        }
    }

    int CountActiveProjectiles() const {
        int count = 0;
        for (const auto& e : entities()) {
            if (!e.value("active", true) || e.value("_destroyed", false)) continue;
            const string name = e.value("name", string());
            if (name.rfind("AbyssShard", 0) == 0 || name.rfind("AbyssBolt", 0) == 0) ++count;
        }
        return count;
    }

    int CountActiveSummons() const {
        int count = 0;
        for (const auto& e : entities())
            if (e.value("abyss_summoned", false) && e.value("active", true)) ++count;
        return count;
    }

    void Hurt(EntityRef other) {
        if (defeated || hit_lock > 0.0f) return;
        if (!other || !other.Contains("team")) return;
        if (other.Value("team", 0) == team) return;
        if (!other.Contains("damage")) return;

        // Route all damage through the host-authoritative system.
        uint32_t my_net_id = entity ? Replication::NetIdOf(entity) : 0;
        if (my_net_id != 0) {
            Replication::RequestDamage(my_net_id,
                                        (float)Max(1, other.Value("damage", 1)),
                                        Network::LocalPeerId(),
                                        Transform().X(), Transform().Y());
        } else {
            hp -= Max(1, other.Value("damage", 1));
        }
        if (entity) entity["hp"] = hp;
        hit_lock = 0.06f;

        if (auto rb = Rigidbody()) rb.AddImpulse({(Transform().X() < GetX(other, Transform().X()) ? -1.0f : 1.0f) * 180.0f, -120.0f});
        AbyssFx::HitSpark(this, Transform().X(), Transform().Y(), AbyssFx::Color{255, 235, 220, 255}, 1.3f);
        if (hp <= 0) Defeat();
    }

    void Defeat() {
        if (defeated) return;
        defeated = true;
        PlayerPrefs::set_int("abyss_boss_defeated", 1);
        if (auto bar = Find("BossHealthBar")) bar["active"] = false;
        if (auto frame = Find("BossHealthFrame")) frame["active"] = false;
        if (auto title = Find("BossHealthTitle")) title["active"] = false;
        AbyssGame::MarkRoomCleared("Boss Sanctum");
        AbyssFx::Explosion(this, Transform().X(), Transform().Y(), AbyssFx::Color{255, 140, 90, 255}, 2.2f);
        AbyssFx::Explosion(this, Transform().X() - 30.0f, Transform().Y() - 20.0f, AbyssFx::Color{255, 90, 90, 255}, 1.6f);
        AbyssFx::Explosion(this, Transform().X() + 30.0f, Transform().Y() + 16.0f, AbyssFx::Color{200, 120, 255, 255}, 1.6f);
        if (Network::IsHost() || !Network::IsClient()) Replication::FireWorldEvent("boss_defeated", Entity::object());
        Destroy();
    }

    // Stored asset dir for networked spawns (set by the engine before Awake).
    string asset_dir_;

};
