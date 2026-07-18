#include "../../../engine_cpp/script_system.hpp"
#include "abyss_fx.hpp"
#include <cmath>
#include <algorithm>
#include <string>

class AbyssSlash : public MonoBehaviour {
public:
    void Awake() override {
        life = entity ? entity.Value("life_time", 0.16f) : 0.16f;
        damage = entity ? entity.Value("damage", 2) : 2;
        team = entity ? entity.Value("team", 1) : 1;
        dir_x = entity ? entity.Value("dir_x", 1.0f) : 1.0f;
        dir_y = entity ? entity.Value("dir_y", 0.0f) : 0.0f;
        combo_step = entity ? entity.Value("combo_step", 1) : 1;
        finisher = combo_step >= 4;
        attack_style = entity ? entity.Value("attack_style", string("ground")) : string("ground");
        pogo_on_hit = entity ? entity.Value("pogo_on_hit", false) : false;
        attack_serial = entity ? entity.Value("attack_serial", 0) : 0;
        impact_resolved = false;

        float len = Sqrt(dir_x * dir_x + dir_y * dir_y);
        if (len < 0.001f) { dir_x = 1.0f; dir_y = 0.0f; len = 1.0f; }
        dir_x /= len;
        dir_y /= len;
        start_life = Max(0.001f, life);

        // The attack sheet is advanced here rather than by the generic
        // Animator.  This makes the source rectangle valid before the first
        // render and guarantees that a normal melee press can never draw the
        // complete 256px-wide sheet as four gold bars.
        if (entity && entity.Contains("components") && entity["components"].contains("Animator")) {
            auto& anim = entity["components"]["Animator"];
            anim["playing"] = false;
            anim["use_sprite_sheet"] = false;
        }
        ApplyFrame(0);

        float angle = -atan2(dir_y, dir_x) * 180.0f / 3.1415926535f;
        // Alternate the second link of a ground combo and give directional
        // cuts their own readable arc. The visual sweep now agrees with the
        // actual hit direction instead of always rotating clockwise.
        sweep_sign = (combo_step == 2 || attack_style == "upward") ? -1.0f : 1.0f;
        if (attack_style == "downward") sweep_sign = -1.0f;
        start_rotation = angle - sweep_sign * (attack_style == "charged" ? 46.0f : 30.0f);
        Transform().SetRotation(start_rotation);

        // The finisher (3rd combo hit) reads as the "big" swing: a bit
        // larger on spawn and tinted warm gold instead of plain white, on
        // top of the extra reach/damage/lunge already applied in
        // abyss_player.cpp's Slash().
        if (finisher) {
            Vector2 s = Transform().Scale();
            Transform().SetScale({s.x * 1.25f, s.y * 1.25f});
            auto sr = GetComponent("SpriteRenderer");
            if (sr) sr.SetValue("color", vector<int>{255, 214, 120, 255});
        }
    }

    void Update(float dt) override {
        life -= dt;
        if (life <= 0.0f) {
            Destroy();
            return;
        }

        float t = 1.0f - Max(0.0f, life) / Max(0.001f, start_life);
        float pulse_speed = finisher ? 46.0f : (attack_style == "aerial" ? 42.0f : 36.0f);
        float pulse = 1.0f + (finisher ? 0.30f : 0.20f) * Sin((float)Time::elapsed_time * pulse_speed);

        Vector2 p = Transform().Position();
        float travel = finisher ? 260.0f : (attack_style == "aerial" ? 210.0f : attack_style == "downward" ? 155.0f : 180.0f);
        p.x += dir_x * (travel * dt);
        p.y += dir_y * (travel * dt);
        Transform().SetPosition(p);
        const float arc = finisher ? 108.0f : (attack_style == "upward" ? 82.0f : attack_style == "downward" ? 72.0f : 64.0f);
        Transform().SetRotation(start_rotation + sweep_sign * t * arc);
        Transform().SetScale({pulse * (1.0f + t * 0.45f), pulse * (1.0f + t * 0.20f)});

        auto sr = GetComponent("SpriteRenderer");
        if (sr) {
            ApplyFrame(Min(3, Max(0, (int)(t * 4.0f))));
            sr.SetValue("opacity", Max(0.0f, 1.0f - t * 0.95f));
            sr.SetValue("flip_x", dir_x < 0.0f);
        }
    }

    void OnTriggerEnter2D(EntityRef other) override {
        if (impact_resolved || !other) return;
        // Read team straight off our own entity JSON rather than the
        // cached `team` member: physics can deliver a trigger callback on
        // the very same frame the slash spawns, BEFORE Awake() has run
        // and set `team` from "1" -> the member would still be its
        // default-constructed 0, which doesn't match the player's own
        // team (1) and let a slash-vs-player overlap slip through as a
        // false "hit" on spawn (inflating the combo with zero enemies
        // anywhere nearby, and zero damage since the player ignores
        // same-team hits).
        int my_team = entity ? entity.Value("team", team) : team;
        if (other.Contains("team") && other.Value("team", 0) == my_team) return;
        // Belt-and-suspenders: never count the player itself as a combo
        // hit no matter what team bookkeeping says.
        if (other == entity) return;
        if (other.Contains("name") && other.Value("name", string()) == "AbyssPlayer") return;

        // Only count as a real "hit" (and tell the player to keep/advance
        // the combo) when we actually clipped something with a team that
        // can take damage — i.e. an enemy/hazard, not scenery/triggers
        // that happen to lack a team tag. Swinging at empty air should
        // never look or feel like a successful hit.
        bool is_damageable_target = other.Contains("team") && other.Contains("hp")
            && other.Value("team", 0) != my_team;
        if (!is_damageable_target) {
            // Room volumes, checkpoints, portals, pickups, and other trigger
            // helpers are not sword targets.  Destroying a slash on those
            // invisible entities made a melee hit disappear as soon as it
            // entered an authored room.
            return;
        }

        int hits = PlayerPrefs::get_int("abyss_slash_hit_signal", 0);
        PlayerPrefs::set_int("abyss_slash_hit_signal", hits + 1);
        if (pogo_on_hit) {
            PlayerPrefs::set_int("abyss_downstrike_signal",
                                 PlayerPrefs::get_int("abyss_downstrike_signal", 0) + 1);
        }
        PlayerPrefs::set_int("abyss_hitstop_signal",
                             PlayerPrefs::get_int("abyss_hitstop_signal", 0) + 1);
        if (other.Contains("components") && other["components"].contains("Rigidbody2D")) {
            const float force = finisher ? 300.0f : 155.0f;
            other["components"]["Rigidbody2D"]["velocity_x"] = dir_x * force;
            other["components"]["Rigidbody2D"]["velocity_y"] = -force * 0.32f;
        }
        other["last_abyss_attack_serial"] = attack_serial;
        other["last_abyss_attack_style"] = attack_style;

            // Confirmed-hit spark, right at the point of impact — bright
            // and punchy for a quick poke, bigger and warmer for a finisher.
        float hit_x = Transform().X();
        float hit_y = Transform().Y();
        if (finisher) {
            AbyssFx::HitSpark(this, hit_x, hit_y, AbyssFx::Color{255, 220, 140, 255}, 1.6f);
        } else {
            AbyssFx::HitSpark(this, hit_x, hit_y, AbyssFx::Color{235, 245, 255, 255}, 1.0f);
        }
        impact_resolved = true;
        Destroy();
    }

private:
    void ApplyFrame(int frame) {
        auto sr = GetComponent("SpriteRenderer");
        if (!sr) return;
        sr.SetValue("texture", string("slash_abyss.png"));
        sr.SetValue("use_source_rect", true);
        int style_bias = 0;
        if (attack_style == "upward") style_bias = 1;
        else if (attack_style == "downward") style_bias = 2;
        else if (finisher || attack_style == "charged") style_bias = 3;
        const int display_frame = Min(3, Max(0, frame + style_bias));
        sr.SetValue("source_x", display_frame * 64);
        sr.SetValue("source_y", 0);
        sr.SetValue("source_w", 64);
        sr.SetValue("source_h", 64);
    }

    float life = 0.0f;
    float start_life = 0.16f;
    int damage = 0;
    int team = 1;
    float dir_x = 1.0f;
    float dir_y = 0.0f;
    float start_rotation = 0.0f;
    int combo_step = 1;
    bool finisher = false;
    string attack_style = "ground";
    bool pogo_on_hit = false;
    bool impact_resolved = false;
    int attack_serial = 0;
    float sweep_sign = 1.0f;
};
