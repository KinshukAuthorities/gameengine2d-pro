#pragma once

#include <algorithm>
#include <random>
#include <string>
#include <vector>
#include <functional>
#include <utility>
#include <memory>
#include <limits>
#include <cstdint>
#include <optional>
#include "net/network.hpp"
#include "net/matchmaking.hpp"
#include "net/replication_rpc.hpp"
#include "net/net_predict.hpp"
#include "net/lag_compensation.hpp"

// ── Common std names (available to all code in this header and user scripts) ──
using std::string;
using std::vector;
using std::function;
using std::pair;
using std::make_shared;
using std::numeric_limits;
using std::uint32_t;
using std::uint16_t;
using std::optional;

// Unity2D-style scripting façade built on top of the existing native C++ API.
// This is intentionally additive: every old ScriptBase function still works,
// but new scripts can read more like Unity 2D code.

struct Vector2 {
    float x = 0.f;
    float y = 0.f;

    Vector2() = default;
    Vector2(float x_, float y_) : x(x_), y(y_) {}

    // ── Arithmetic operators ────────────────────────────────────────────────
    Vector2 operator+(Vector2 o) const { return {x + o.x, y + o.y}; }
    Vector2 operator-(Vector2 o) const { return {x - o.x, y - o.y}; }
    Vector2 operator*(float s)          const { return {x * s,   y * s};   }
    Vector2 operator/(float s)          const { return {x / s,   y / s};   }
    Vector2& operator+=(Vector2 o) { x += o.x; y += o.y; return *this; }
    Vector2& operator-=(Vector2 o) { x -= o.x; y -= o.y; return *this; }
    Vector2& operator*=(float s)          { x *= s;   y *= s;   return *this; }
    Vector2 operator-() const { return {-x, -y}; }
    bool operator==(Vector2 o) const { return x == o.x && y == o.y; }
    bool operator!=(Vector2 o) const { return !(*this == o); }

    // ── Common math ─────────────────────────────────────────────────────────
    float Magnitude()        const { return std::hypot(x, y); }
    float SqrMagnitude()     const { return x * x + y * y; }
    // Returns a unit-length copy — returns {0,0} instead of NaN on zero vector.
    Vector2 Normalized() const {
        float m = Magnitude();
        return m > 1e-6f ? Vector2{x / m, y / m} : Vector2{};
    }
    // Dot product — positive if same direction, 0 if perpendicular, negative if opposite.
    static float Dot(Vector2 a, Vector2 b) { return a.x * b.x + a.y * b.y; }
    // Distance between two points.
    static float Distance(Vector2 a, Vector2 b) { return (b - a).Magnitude(); }
    // Linear interpolation between two vectors. t is clamped to [0,1].
    static Vector2 Lerp(Vector2 a, Vector2 b, float t) {
        t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
    }
    // Move a towards b by at most maxDelta, never overshooting.
    static Vector2 MoveTowards(Vector2 a, Vector2 b, float maxDelta) {
        Vector2 d = b - a;
        float dist = d.Magnitude();
        if (dist <= maxDelta || dist < 1e-6f) return b;
        return a + d * (maxDelta / dist);
    }
    // 2D "cross" (scalar z of the 3D cross product) — useful for left/right sign.
    static float Cross(Vector2 a, Vector2 b) { return a.x * b.y - a.y * b.x; }
    // Unsigned angle between two vectors, in degrees (0..180). Common for
    // aim-cone / spread checks: Vector2::Angle(aimDir, toTarget) < 30.f
    static float Angle(Vector2 a, Vector2 b) {
        float ma = a.Magnitude(), mb = b.Magnitude();
        if (ma < 1e-6f || mb < 1e-6f) return 0.f;
        float cos_t = Dot(a, b) / (ma * mb);
        cos_t = cos_t < -1.f ? -1.f : cos_t > 1.f ? 1.f : cos_t;
        return std::acos(cos_t) * (180.f / 3.14159265358979323846f);
    }

    // ── Named direction constants ────────────────────────────────────────────
    static Vector2 Zero()  { return {0.f, 0.f}; }
    static Vector2 One()   { return {1.f, 1.f}; }
    static Vector2 Up()    { return {0.f,-1.f}; } // screen-space: -Y is up
    static Vector2 Down()  { return {0.f, 1.f}; }
    static Vector2 Left()  { return {-1.f,0.f}; }
    static Vector2 Right() { return { 1.f,0.f}; }
};
inline Vector2 operator*(float s, Vector2 v) { return {v.x * s, v.y * s}; }

// Lightweight engine-level color, usable without pulling in SpriteRenderer2D
// (e.g. for UI tinting, particle color params, or any future component that
// wants a color without depending on the sprite wrapper's nested type).
// SpriteRenderer2D::Color stays as-is for backwards compatibility with
// existing scripts; the two are deliberately the same shape so a
// SpriteRenderer2D::Color can be constructed from one of these directly.
struct Color {
    Uint8 r = 255, g = 255, b = 255;
    float a = 1.f;
    Color() = default;
    Color(Uint8 r_, Uint8 g_, Uint8 b_, float a_ = 1.f) : r(r_), g(g_), b(b_), a(a_) {}
    static Color White()  { return {255,255,255,1.f}; }
    static Color Black()  { return {0,0,0,1.f}; }
    static Color Red()    { return {255,0,0,1.f}; }
    static Color Green()  { return {0,255,0,1.f}; }
    static Color Blue()   { return {0,0,255,1.f}; }
    static Color Yellow() { return {255,255,0,1.f}; }
    static Color Clear()  { return {255,255,255,0.f}; }
};

using KeyCode = SDL_Scancode;

// ─── Key ────────────────────────────────────────────────────────────────────
// Human-readable key codes so scripts write:
//   if (pressed(Key::Space)) jump();
//   if (held(Key::W)) move_up();
namespace Key {
    inline constexpr KeyCode None    = SDL_SCANCODE_UNKNOWN;
    inline constexpr KeyCode A       = SDL_SCANCODE_A;
    inline constexpr KeyCode B       = SDL_SCANCODE_B;
    inline constexpr KeyCode C       = SDL_SCANCODE_C;
    inline constexpr KeyCode D       = SDL_SCANCODE_D;
    inline constexpr KeyCode E       = SDL_SCANCODE_E;
    inline constexpr KeyCode F       = SDL_SCANCODE_F;
    inline constexpr KeyCode G       = SDL_SCANCODE_G;
    inline constexpr KeyCode H       = SDL_SCANCODE_H;
    inline constexpr KeyCode I       = SDL_SCANCODE_I;
    inline constexpr KeyCode J       = SDL_SCANCODE_J;
    inline constexpr KeyCode K       = SDL_SCANCODE_K;
    inline constexpr KeyCode L       = SDL_SCANCODE_L;
    inline constexpr KeyCode M       = SDL_SCANCODE_M;
    inline constexpr KeyCode N       = SDL_SCANCODE_N;
    inline constexpr KeyCode O       = SDL_SCANCODE_O;
    inline constexpr KeyCode P       = SDL_SCANCODE_P;
    inline constexpr KeyCode Q       = SDL_SCANCODE_Q;
    inline constexpr KeyCode R       = SDL_SCANCODE_R;
    inline constexpr KeyCode S       = SDL_SCANCODE_S;
    inline constexpr KeyCode T       = SDL_SCANCODE_T;
    inline constexpr KeyCode U       = SDL_SCANCODE_U;
    inline constexpr KeyCode V       = SDL_SCANCODE_V;
    inline constexpr KeyCode W       = SDL_SCANCODE_W;
    inline constexpr KeyCode X       = SDL_SCANCODE_X;
    inline constexpr KeyCode Y       = SDL_SCANCODE_Y;
    inline constexpr KeyCode Z       = SDL_SCANCODE_Z;
    inline constexpr KeyCode K0      = SDL_SCANCODE_0;
    inline constexpr KeyCode K1      = SDL_SCANCODE_1;
    inline constexpr KeyCode K2      = SDL_SCANCODE_2;
    inline constexpr KeyCode K3      = SDL_SCANCODE_3;
    inline constexpr KeyCode K4      = SDL_SCANCODE_4;
    inline constexpr KeyCode K5      = SDL_SCANCODE_5;
    inline constexpr KeyCode K6      = SDL_SCANCODE_6;
    inline constexpr KeyCode K7      = SDL_SCANCODE_7;
    inline constexpr KeyCode K8      = SDL_SCANCODE_8;
    inline constexpr KeyCode K9      = SDL_SCANCODE_9;
    inline constexpr KeyCode Space   = SDL_SCANCODE_SPACE;
    inline constexpr KeyCode Enter   = SDL_SCANCODE_RETURN;
    inline constexpr KeyCode Escape  = SDL_SCANCODE_ESCAPE;
    inline constexpr KeyCode Tab     = SDL_SCANCODE_TAB;
    inline constexpr KeyCode Shift   = SDL_SCANCODE_LSHIFT;
    inline constexpr KeyCode RightShift = SDL_SCANCODE_RSHIFT;
    inline constexpr KeyCode Ctrl    = SDL_SCANCODE_LCTRL;
    inline constexpr KeyCode Alt     = SDL_SCANCODE_LALT;
    inline constexpr KeyCode Up      = SDL_SCANCODE_UP;
    inline constexpr KeyCode Down    = SDL_SCANCODE_DOWN;
    inline constexpr KeyCode Left    = SDL_SCANCODE_LEFT;
    inline constexpr KeyCode Right   = SDL_SCANCODE_RIGHT;
    inline constexpr KeyCode Backspace = SDL_SCANCODE_BACKSPACE;
    inline constexpr KeyCode Delete  = SDL_SCANCODE_DELETE;
    inline constexpr KeyCode Home    = SDL_SCANCODE_HOME;
    inline constexpr KeyCode End     = SDL_SCANCODE_END;
    inline constexpr KeyCode PageUp  = SDL_SCANCODE_PAGEUP;
    inline constexpr KeyCode PageDown = SDL_SCANCODE_PAGEDOWN;
    inline constexpr KeyCode F1      = SDL_SCANCODE_F1;
    inline constexpr KeyCode F2      = SDL_SCANCODE_F2;
    inline constexpr KeyCode F3      = SDL_SCANCODE_F3;
    inline constexpr KeyCode F4      = SDL_SCANCODE_F4;
    inline constexpr KeyCode F5      = SDL_SCANCODE_F5;
    inline constexpr KeyCode F6      = SDL_SCANCODE_F6;
    inline constexpr KeyCode F7      = SDL_SCANCODE_F7;
    inline constexpr KeyCode F8      = SDL_SCANCODE_F8;
    inline constexpr KeyCode F9      = SDL_SCANCODE_F9;
    inline constexpr KeyCode F10     = SDL_SCANCODE_F10;
    inline constexpr KeyCode F11     = SDL_SCANCODE_F11;
    inline constexpr KeyCode F12     = SDL_SCANCODE_F12;
    inline constexpr KeyCode Minus   = SDL_SCANCODE_MINUS;
    inline constexpr KeyCode Equals  = SDL_SCANCODE_EQUALS;
    inline constexpr KeyCode LBracket = SDL_SCANCODE_LEFTBRACKET;
    inline constexpr KeyCode RBracket = SDL_SCANCODE_RIGHTBRACKET;
    inline constexpr KeyCode Semicolon = SDL_SCANCODE_SEMICOLON;
    inline constexpr KeyCode Quote   = SDL_SCANCODE_APOSTROPHE;
    inline constexpr KeyCode Comma   = SDL_SCANCODE_COMMA;
    inline constexpr KeyCode Period  = SDL_SCANCODE_PERIOD;
    inline constexpr KeyCode Slash   = SDL_SCANCODE_SLASH;
    inline constexpr KeyCode Backslash = SDL_SCANCODE_BACKSLASH;
}

// ─── Random ─────────────────────────────────────────────────────────────────
// Every script that needs randomness currently has to set up its own
// <random> engine/distribution by hand. This wraps one process-wide
// generator (seeded once, like Unity's UnityEngine.Random) behind the
// handful of calls actually used in game code.
namespace Random {
    inline std::mt19937& _engine() {
        static std::mt19937 eng{std::random_device{}()};
        return eng;
    }
    // Re-seed explicitly — e.g. for a reproducible run (replays, deterministic
    // testing). Mirrors Unity's Random.InitState(seed).
    inline void InitState(unsigned int seed) { _engine().seed(seed); }

    // Random float in [0, 1).
    inline float Value() {
        std::uniform_real_distribution<float> d(0.f, 1.f);
        return d(_engine());
    }
    // Random float in [min, max].
    inline float Range(float min, float max) {
        if (max < min) std::swap(min, max);
        std::uniform_real_distribution<float> d(min, max);
        return d(_engine());
    }
    // Random int in [min, max) — matches Unity's Random.Range(int,int) being
    // max-exclusive (the float overload above is max-inclusive, also matching
    // Unity's split behavior between the two overloads).
    inline int Range(int min, int max) {
        if (max <= min) return min;
        std::uniform_int_distribution<int> d(min, max - 1);
        return d(_engine());
    }
    // Uniformly distributed point inside the unit circle — handy for spread/
    // scatter effects (bullet spread, particle burst directions, spawn jitter).
    inline Vector2 InsideUnitCircle() {
        float angle = Range(0.f, 2.f * 3.14159265358979323846f);
        float r = std::sqrt(Value());
        return { std::cos(angle) * r, std::sin(angle) * r };
    }
    // Random unit-length direction vector (point ON the unit circle, not
    // inside it) — useful for "fire in a random direction" type effects.
    inline Vector2 OnUnitCircle() {
        float angle = Range(0.f, 2.f * 3.14159265358979323846f);
        return { std::cos(angle), std::sin(angle) };
    }
    // Random bool — convenience for 50/50 coin-flip branches.
    inline bool Bool() { return Value() < 0.5f; }
}


class ComponentRef {
public:
    ComponentRef() = default;
    explicit ComponentRef(Entity* comp) : _comp(comp) {}

    explicit operator bool() const { return _comp != nullptr; }
    bool valid() const { return _comp != nullptr; }

    Entity* operator->() { return _comp; }
    const Entity* operator->() const { return _comp; }
    Entity& operator*() { return *_comp; }
    const Entity& operator*() const { return *_comp; }

    template <class T>
    T value(string key, T def) const {
        if (!_comp) return def;
        return _comp->value(key, def);
    }

    template <class T>
    void set(string key, T value) {
        if (_comp) (*_comp)[key] = value;
    }

    template <class T>
    void SetValue(string key, T val) { set(key, val); }

    template <class T>
    T Value(string key, T def) const { return value(key, def); }

    bool Contains(string key) const { return has(key); }

    bool has(string key) const {
        return _comp && _comp->contains(key);
    }

    Entity& raw() { return *_comp; }
    Entity raw() const { return *_comp; }

private:
    Entity* _comp = nullptr;
};

// ─── Screen ─────────────────────────────────────────────────────────────────
// Single source of truth for "what size is the UI canvas right now". UIPanel/
// UIButton/UIText/UIImage positions are all resolved as anchor*canvas + pos
// (see RenderSystem::draw_ui's resolve() lambda), so that resolution has to
// use the SAME width/height the renderer actually used to draw, whatever that
// happens to be this frame — a resizable editor viewport, a resizable
// standalone window, anything. Previously there was no way for a script to
// ask "how big is the screen", so click hit-testing (e.g. abyss_menu_
// controller.cpp's ButtonClicked()) had to guess/hardcode a size, which only
// matched the real render size by coincidence — breaking the instant the
// window/viewport was resized to anything else. Whoever owns the render
// target (core.cpp's run_game(), or the editor's ViewportPanel) calls
// Screen::Set(...) once per frame with the actual current output size;
// everything else — rendering AND click math — reads from here.
// ─── Screen / Input cross-module note ──────────────────────────────────────
// Both Screen and Input below hold process-wide state (current screen size,
// current frame's input snapshot) that this header — being header-only —
// would otherwise instantiate SEPARATELY in the host editor.exe AND in
// every per-project game_scripts_<project> DLL. That's a real bug, not a
// theoretical one: ViewportPanel calls Input::Bind(...) and Screen::Set(...)
// from the HOST every frame, but a script's update()/Awake() (e.g.
// AbyssPlayer reading Input::GetAxis("Horizontal")) runs as code compiled
// INTO the DLL — it would read the DLL's own copy of `current`/`_width`/
// `_height`, which Bind()/Set() never touched, so every Input::Get* call
// would silently return false/0 and Screen::Width()/Height() would sit at
// their stale compiled-in defaults forever. That's exactly the
// "script doesn't respond to input, UI buttons don't register clicks" bug.
//
// Fixed the same way ScriptRegistry/InstanceRegistry are: each namespace's
// real state lives in a small struct, the HOST's instance is the one true
// copy, and every loaded DLL is handed a pointer to it (bind_state, called
// from RegisterAllScripts right alongside the other two handoffs) so the
// DLL's Bind()/Set()/Get* calls all redirect into the host's storage
// instead of each binary keeping its own disconnected copy.
namespace Screen {
    struct State { int width = 1280; int height = 720; };

    inline State& _local_state() { static State s; return s; }
    inline State*& _state_ptr() { static State* p = nullptr; return p; }

    // Called by a DLL right after load (see RegisterAllScripts) so this
    // module's Screen:: calls redirect to the host's real state. The host
    // itself never calls this — its own _state_ptr() stays null, so _state()
    // falls through to _local_state(), which becomes the one true copy
    // every DLL gets pointed at.
    inline void bind_state(State* host) { _state_ptr() = host; }
    inline State& _state() { return _state_ptr() ? *_state_ptr() : _local_state(); }

    inline void Set(int w, int h) {
        auto& s = _state();
        if (w > 0) s.width  = w;
        if (h > 0) s.height = h;
    }
    inline int Width()  { return _state().width; }
    inline int Height() { return _state().height; }
}

namespace Input {
    struct State { InputSystem* current = nullptr; };

    inline State& _local_state() { static State s; return s; }
    inline State*& _state_ptr() { static State* p = nullptr; return p; }
    inline void bind_state(State* host) { _state_ptr() = host; }
    inline State& _state() { return _state_ptr() ? *_state_ptr() : _local_state(); }

    inline void Bind(InputSystem* input) { _state().current = input; }

    inline bool GetKey(KeyCode key) { auto* c = _state().current; return c && c->get_key(key); }
    inline bool GetKeyDown(KeyCode key) { auto* c = _state().current; return c && c->get_key_down(key); }
    inline bool GetKeyUp(KeyCode key) { auto* c = _state().current; return c && c->get_key_up(key); }

    inline float GetAxis(string name) { auto* c = _state().current; return c ? c->get_axis(name) : 0.f; }
    inline float GetAxisRaw(string name) { auto* c = _state().current; return c ? c->get_axis_raw(name) : 0.f; }
    inline bool GetButton(string name) { auto* c = _state().current; return c && c->get_button(name); }
    inline bool GetButtonDown(string name) { auto* c = _state().current; return c && c->get_button_down(name); }
    inline bool GetButtonUp(string name) { auto* c = _state().current; return c && c->get_button_up(name); }

    inline bool GetMouseButton(int btn) { auto* c = _state().current; return c && c->get_mouse_button(btn); }
    inline bool GetMouseButtonDown(int btn) { auto* c = _state().current; return c && c->get_mouse_button_down(btn); }
    inline bool GetMouseButtonUp(int btn) { auto* c = _state().current; return c && c->get_mouse_button_up(btn); }

    inline Vector2 MousePosition() {
        auto* c = _state().current;
        return c ? Vector2{c->mouse_world_x, c->mouse_world_y} : Vector2{};
    }

    inline int ScrollDelta() { auto* c = _state().current; return c ? c->mouse_scroll : 0; }
}

class ScriptBase; // already defined above; kept for clarity in this façade.

class Transform2D {
public:
    Transform2D() = default;
    explicit Transform2D(EntityRef e) : _entity(e) {}

    explicit operator bool() const { return _comp() != nullptr; }

    Vector2 Position() const {
        _detect();
        auto* t = _comp();
        if (!_script) return {_lf(t, "x", 0.f), _lf(t, "y", 0.f)};
        auto p = _script->get_world_position();
        return {p.first, p.second};
    }
    void SetPosition(Vector2 p) {
        _detect();
        if (_script) { _script->set_world_position(p.x, p.y); return; }
        auto* t = _comp(); if (t) { (*t)["x"] = p.x; (*t)["y"] = p.y; }
    }

    Vector2 LocalPosition() const {
        auto* t = _comp(); return {_lf(t, "x", 0.f), _lf(t, "y", 0.f)};
    }
    void SetLocalPosition(Vector2 p) { auto* t = _comp(); if (t) { (*t)["x"] = p.x; (*t)["y"] = p.y; } }

    float Rotation() const { auto* t = _comp(); return _lf(t, "rotation", 0.f); }
    void SetRotation(float degrees) { auto* t = _comp(); if (t) (*t)["rotation"] = degrees; }

    Vector2 Scale() const {
        auto* t = _comp(); return {_lf(t, "scale_x", 1.f), _lf(t, "scale_y", 1.f)};
    }
    void SetScale(Vector2 s) { auto* t = _comp(); if (t) { (*t)["scale_x"] = s.x; (*t)["scale_y"] = s.y; } }

    void Translate(Vector2 delta, bool local = true) {
        _detect();
        if (_script) { _script->translate(delta.x, delta.y, local); return; }
        SetLocalPosition({LocalX() + delta.x, LocalY() + delta.y});
    }
    void Rotate(float degrees) { SetRotation(Rotation() + degrees); }
    void LookAt(Vector2 target) {
        _detect();
        if (_script) { _script->look_at(target.x, target.y); return; }
        float dx = target.x - X(), dy = target.y - Y();
        SetRotation(std::atan2(dy, dx) * 180.f / 3.14159265f);
    }

    float X() const { return Position().x; }
    float Y() const { return Position().y; }
    void SetX(float x) {
        _detect();
        if (_script) { _script->set_world_position(x, Y()); return; }
        auto* t = _comp(); if (t) (*t)["x"] = x;
    }
    void SetY(float y) {
        _detect();
        if (_script) { _script->set_world_position(X(), y); return; }
        auto* t = _comp(); if (t) (*t)["y"] = y;
    }

    float LocalX() const { auto* t = _comp(); return _lf(t, "x", 0.f); }
    float LocalY() const { auto* t = _comp(); return _lf(t, "y", 0.f); }
    void SetLocalX(float x) { auto* t = _comp(); if (t) (*t)["x"] = x; }
    void SetLocalY(float y) { auto* t = _comp(); if (t) (*t)["y"] = y; }

    void MoveTowards(Vector2 target, float maxDelta) {
        float wx = X(), wy = Y();
        float dx = target.x - wx, dy = target.y - wy;
        float dist = std::hypot(dx, dy);
        if (dist <= maxDelta || dist < 1e-6f) { SetPosition(target); return; }
        float t = maxDelta / dist;
        SetPosition({wx + dx * t, wy + dy * t});
    }
    float DistanceTo(Vector2 other) const {
        return std::hypot(other.x - X(), other.y - Y());
    }
    Vector2 DirectionTo(Vector2 other) const {
        float dx = other.x - X(), dy = other.y - Y();
        float d = std::hypot(dx, dy);
        return d > 1e-6f ? Vector2{dx / d, dy / d} : Vector2{};
    }
    void LerpPosition(Vector2 target, float t) {
        SetPosition({
            Mathf::lerp(X(), target.x, Mathf::clamp01(t)),
            Mathf::lerp(Y(), target.y, Mathf::clamp01(t))});
    }
    void LerpRotation(float targetDeg, float t) {
        SetRotation(Mathf::lerp(Rotation(), targetDeg, Mathf::clamp01(t)));
    }

private:
    mutable ScriptBase* _script = nullptr;
    EntityRef _entity;
    mutable bool _detected = false;
    void _detect() const {
        if (!_detected) {
            _detected = true;
            if (!_script)
                const_cast<Transform2D*>(this)->_script = ScriptBase::current();
        }
    }
    Entity* _comp() const {
        _detect();
        if (_script) return _script->get_component("Transform");
        if (_entity) {
            if (!_entity.Contains("components") || !_entity["components"].contains("Transform")) return nullptr;
            return const_cast<Entity*>(&_entity["components"]["Transform"]);
        }
        return nullptr;
    }
    static float _lf(Entity* t, const char* key, float def) {
        return t ? t->value(key, def) : def;
    }
};

class Rigidbody2D {
public:
    Rigidbody2D() = default;
    explicit Rigidbody2D(EntityRef e) : _entity(e) {}

    explicit operator bool() const { return _script != nullptr || _entity; }

    // ── Velocity helpers (work for both self-wrappers and EntityRef wrappers) ──
    // _script path: used when the wrapper was default-constructed (lazy self-detect).
    // _entity path: used when constructed as Rigidbody2D(other) for another entity.
    // Previously the _entity path was missing from all velocity methods, so
    // Rigidbody2D(other).SetVelocity(...) silently did nothing.
    Vector2 Velocity() const {
        _detect();
        if (_script) return {_script->get_velocity_x(), _script->get_velocity_y()};
        auto* rb = _rb_direct(); if(!rb) return Vector2{};
        return rb->contains("vx") ? Vector2{(float)rb->value("vx",0.f), (float)rb->value("vy",0.f)} : Vector2{(float)rb->value("velocity_x",0.f), (float)rb->value("velocity_y",0.f)};
    }
    void SetVelocity(Vector2 v) { _detect(); if (_script) { _script->set_velocity(v.x, v.y); return; } auto* rb = _rb_direct(); if (rb) { (*rb)["velocity_x"]=v.x; (*rb)["velocity_y"]=v.y; (*rb)["vx"]=v.x; (*rb)["vy"]=v.y; (*rb)["_sleeping"]=false; } }
    void SetVelocity(float vx, float vy) { _detect(); if (_script) { _script->set_velocity(vx, vy); return; } auto* rb = _rb_direct(); if (rb) { (*rb)["velocity_x"]=vx; (*rb)["velocity_y"]=vy; (*rb)["vx"]=vx; (*rb)["vy"]=vy; (*rb)["_sleeping"]=false; } }
    void AddForce(Vector2 f) { _detect(); if (_script) { _script->add_force(f.x, f.y); return; } auto* rb = _rb_direct(); if (rb) { (*rb)["_force_x"]=(float)rb->value("_force_x",0.f)+f.x; (*rb)["_force_y"]=(float)rb->value("_force_y",0.f)+f.y; (*rb)["_sleeping"]=false; } }
    void AddForce(float fx, float fy) { _detect(); if (_script) { _script->add_force(fx, fy); return; } auto* rb = _rb_direct(); if (rb) { (*rb)["_force_x"]=(float)rb->value("_force_x",0.f)+fx; (*rb)["_force_y"]=(float)rb->value("_force_y",0.f)+fy; (*rb)["_sleeping"]=false; } }
    void AddImpulse(Vector2 j) { _detect(); if (_script) { _script->add_impulse(j.x, j.y); return; } auto* rb = _rb_direct(); if (rb) { float m=std::max(rb->value("mass",1.f),1e-9f); float nvx=(float)rb->value("velocity_x",0.f)+j.x/m; float nvy=(float)rb->value("velocity_y",0.f)+j.y/m; (*rb)["velocity_x"]=nvx; (*rb)["velocity_y"]=nvy; (*rb)["vx"]=nvx; (*rb)["vy"]=nvy; (*rb)["_sleeping"]=false; } }
    void AddImpulse(float jx, float jy) { _detect(); if (_script) { _script->add_impulse(jx, jy); return; } auto* rb = _rb_direct(); if (rb) { float m=std::max(rb->value("mass",1.f),1e-9f); float nvx=(float)rb->value("velocity_x",0.f)+jx/m; float nvy=(float)rb->value("velocity_y",0.f)+jy/m; (*rb)["velocity_x"]=nvx; (*rb)["velocity_y"]=nvy; (*rb)["vx"]=nvx; (*rb)["vy"]=nvy; (*rb)["_sleeping"]=false; } }
    bool IsGrounded(float tolerance = 4.f) const { _detect(); return _script ? _script->is_grounded(tolerance) : false; }

    float VX() const { _detect(); if (_script) return _script->get_velocity_x(); auto* rb = _rb_direct(); if(!rb) return 0.f; return rb->contains("vx") ? (float)rb->value("vx",0.f) : (float)rb->value("velocity_x",0.f); }
    float VY() const { _detect(); if (_script) return _script->get_velocity_y(); auto* rb = _rb_direct(); if(!rb) return 0.f; return rb->contains("vy") ? (float)rb->value("vy",0.f) : (float)rb->value("velocity_y",0.f); }
    void SetVX(float x) { _detect(); if (_script) { _script->set_velocity(x, VY()); return; } auto* rb = _rb_direct(); if (rb) { (*rb)["velocity_x"]=x; (*rb)["vx"]=x; (*rb)["_sleeping"]=false; } }
    void SetVY(float y) { _detect(); if (_script) { _script->set_velocity(VX(), y); return; } auto* rb = _rb_direct(); if (rb) { (*rb)["velocity_y"]=y; (*rb)["vy"]=y; (*rb)["_sleeping"]=false; } }

    float Mass() const { auto* rb = _rb(); return rb ? rb->value("mass", 1.f) : 1.f; }
    void SetMass(float m) { auto* rb = _rb(); if (rb) (*rb)["mass"] = std::max(0.001f, m); }

    float GravityScale() const { auto* rb = _rb(); return rb ? rb->value("gravity_scale", 1.f) : 1.f; }
    void SetGravityScale(float s) { auto* rb = _rb(); if (rb) (*rb)["gravity_scale"] = s; }

    float LinearDrag() const { auto* rb = _rb(); return rb ? rb->value("linear_drag", 0.f) : 0.f; }
    void SetLinearDrag(float d) { auto* rb = _rb(); if (rb) (*rb)["linear_drag"] = std::max(0.f, d); }

    bool IsKinematic() const { auto* rb = _rb(); return rb && rb->value("is_kinematic", false); }
    void SetKinematic(bool v) { auto* rb = _rb(); if (rb) (*rb)["is_kinematic"] = v; }

    void Stop() { _detect(); if (_script) { _script->set_velocity(0.f, 0.f); return; } auto* rb = _rb_direct(); if (rb) { (*rb)["velocity_x"]=0.f; (*rb)["velocity_y"]=0.f; (*rb)["vx"]=0.f; (*rb)["vy"]=0.f; (*rb)["_sleeping"]=false; } }

    bool FrozenX() const { auto* rb = _rb(); return rb && rb->value("freeze_x", false); }
    bool FrozenY() const { auto* rb = _rb(); return rb && rb->value("freeze_y", false); }
    void FreezeX(bool v = true) { auto* rb = _rb(); if (rb) (*rb)["freeze_x"] = v; }
    void FreezeY(bool v = true) { auto* rb = _rb(); if (rb) (*rb)["freeze_y"] = v; }

    Entity* _rb() const {
        _detect();
        if (_script) return _script->get_component("Rigidbody2D");
        if (_entity) {
            if (!_entity.Contains("components") || !_entity["components"].contains("Rigidbody2D")) return nullptr;
            return const_cast<Entity*>(&_entity["components"]["Rigidbody2D"]);
        }
        return nullptr;
    }
    // Direct entity path without _detect() — for use inside velocity methods
    // where _detect() has already been called and _script is confirmed null.
    Entity* _rb_direct() const {
        if (!_entity) return nullptr;
        if (!_entity.Contains("components") || !_entity["components"].contains("Rigidbody2D")) return nullptr;
        return const_cast<Entity*>(&_entity["components"]["Rigidbody2D"]);
    }
private:
    mutable ScriptBase* _script = nullptr;
    EntityRef _entity;
    mutable bool _detected = false;
    void _detect() const {
        if (!_detected) {
            _detected = true;
            if (!_script)
                const_cast<Rigidbody2D*>(this)->_script = ScriptBase::current();
        }
    }
};

// ─── SpriteRenderer2D ─────────────────────────────────────────────────────────
// Unity-style wrapper around the engine's "SpriteRenderer" component — see
// render_system.hpp's _draw_sprite()/_draw_sprite_tiled()/_draw_sprite_sliced()
// for how each field below is actually consumed at render time, and
// component_defs.hpp for the full default field set this class is mirroring.
//
// Sorting note: the engine has BOTH a legacy flat integer "layer" (kept for
// old scripts/scenes) AND, layered on top, Unity's two-tier model — a named
// "sorting_layer" string resolved against RenderSystem's ordered layer list
// (RenderSystem::set_sorting_layers / EditorState::sorting_layers) plus an
// "order_in_layer" tiebreaker within that layer. SortingOrder()/SetSortingOrder
// address the legacy field for backwards compatibility; SortingLayerName() /
// SetSortingLayerName() / OrderInLayer() / SetOrderInLayer() are the
// Unity-equivalent named-layer accessors and are what new scripts should use.
class SpriteRenderer2D {
public:
    SpriteRenderer2D() = default;
    explicit SpriteRenderer2D(EntityRef e) : _entity(e) {}

    explicit operator bool() const { return _comp() != nullptr; }

    // ── Sprite (Unity: spriteRenderer.sprite) ───────────────────────────────
    string Sprite() const { auto* c = _comp(); return c ? c->value("texture", string()) : string(); }
    void SetSprite(string textureName) {
        auto* c = _comp(); if (!c) return;
        (*c)["texture"] = textureName;
        (*c)["use_source_rect"] = false;
        (*c)["source_w"] = 0;
        (*c)["source_h"] = 0;
    }

    // ── Source rect / sub-sprite (Unity: sprite.rect / sprite.textureRect) ──
    // Crops the texture to a pixel sub-rect, e.g. one frame of a sheet that
    // isn't going through the Animator. Matches the "source_*"/"use_source_rect"
    // fields render_system.hpp actually reads (see AnimatorSystem in
    // systems.hpp for the equivalent it writes every frame for clip playback).
    struct Rect { int x = 0, y = 0, w = 0, h = 0; };
    Rect SourceRect() const {
        auto* c = _comp(); if (!c) return {};
        if (!c->value("use_source_rect", false)) return {};
        return { c->value("source_x",0), c->value("source_y",0),
                 c->value("source_w",0), c->value("source_h",0) };
    }
    void SetSourceRect(int x, int y, int w, int h) {
        auto* c = _comp(); if (!c) return;
        (*c)["use_source_rect"] = true;
        (*c)["source_x"] = x; (*c)["source_y"] = y;
        (*c)["source_w"] = w; (*c)["source_h"] = h;
    }
    void ClearSourceRect() {
        auto* c = _comp(); if (!c) return;
        (*c)["use_source_rect"] = false;
        (*c)["source_w"] = 0; (*c)["source_h"] = 0;
    }
    // Unity: spriteRenderer's "Set Native Size" inspector button. Resets the
    // Transform's scale to 1:1 so the sprite renders at its true pixel
    // dimensions instead of whatever scale it happened to have — eliminates
    // the "why is my sprite stretched" bug that comes from copy-pasting a
    // prefab whose scale was tuned for a differently-sized source image.
    // This only has a pixel size to restore TO when a source rect has been
    // set (via SetSourceRect, or by the Animator for a sprite-sheet clip) —
    // this engine's script layer has no other way to query a texture's raw
    // pixel dimensions, since texture data lives in the Vulkan render
    // backend and scripts intentionally never link against it. With no
    // source rect set, this is a no-op (logged once) rather than guessing.
    void SetNativeSize() {
        auto* c = _comp(); if (!c || !_script) return;
        Rect src = SourceRect();
        if (src.w <= 0 || src.h <= 0) {
            Debug::log_warning("SpriteRenderer2D::SetNativeSize() called with no source rect set — "
                                "nothing to reset to. Call SetSourceRect(...) first, or rely on the "
                                "Animator's sprite-sheet mode, which sets one automatically.");
            return;
        }
        _script->set_scale(1.f, 1.f);
    }

    // ── Flip (Unity: spriteRenderer.flipX / .flipY) ────────────────────────────
    bool FlipX() const { auto* c = _comp(); return c && c->value("flip_x", false); }
    bool FlipY() const { auto* c = _comp(); return c && c->value("flip_y", false); }
    void SetFlipX(bool v) { auto* c = _comp(); if (c) (*c)["flip_x"] = v; }
    void SetFlipY(bool v) { auto* c = _comp(); if (c) (*c)["flip_y"] = v; }
    // Common platformer one-liner: face left/right based on movement sign.
    // Equivalent to `spriteRenderer.flipX = velocity.x < 0f`.
    void FaceDirection(float xSign) { if (xSign != 0.f) SetFlipX(xSign < 0.f); }

    // ── Color / opacity (Unity: spriteRenderer.color) ──────────────────────────
    // Unity packs alpha into color.a; this engine keeps opacity as a separate
    // field (used independently by other renderers too), so Color()/SetColor()
    // read/write both color and opacity together to match Unity call sites
    // like `sr.color = new Color(1,1,1,0.5f)` doing what they expect.
    struct Color { Uint8 r = 255, g = 255, b = 255; float a = 1.f; };
    Color GetColor() const {
        auto* c = _comp(); if (!c) return {};
        auto col = c->value("color", vector<int>{255,255,255,255});
        Color out;
        if (col.size() >= 1) out.r = (Uint8)col[0];
        if (col.size() >= 2) out.g = (Uint8)col[1];
        if (col.size() >= 3) out.b = (Uint8)col[2];
        out.a = c->value("opacity", 1.f);
        return out;
    }
    void SetColor(Color col) {
        auto* c = _comp(); if (!c) return;
        (*c)["color"] = vector<int>{col.r, col.g, col.b, 255};
        (*c)["opacity"] = Mathf::clamp01(col.a);
    }
    // Convenience: SetColor(r, g, b) — full opacity, byte values 0-255.
    // Mirrors the common Unity pattern: sr.color = new Color32(255, 100, 100, 255).
    void SetColor(Uint8 r, Uint8 g, Uint8 b) { SetColor({r, g, b, 1.f}); }
    // Convenience: SetColor(r, g, b, a) — a is 0-255 byte, converted to 0..1.
    // Lets you copy Color32 values directly: SetColor(255, 100, 100, 200).
    void SetColor(Uint8 r, Uint8 g, Uint8 b, Uint8 a) { SetColor({r, g, b, a / 255.f}); }
    // Restore the sprite to plain white at full opacity — the engine's
    // "default" tint. Useful after a hit-flash or fade effect:
    //   sr.SetColorWhite();  // back to normal
    void SetColorWhite() { SetColor({255, 255, 255, 1.f}); }
    // Quick hit-flash red: call once, then SetColorWhite() after your flash delay.
    void SetColorRed(float alpha = 1.f) { SetColor({255, 60, 60, alpha}); }
    float Opacity() const { auto* c = _comp(); return c ? c->value("opacity", 1.f) : 1.f; }
    void SetOpacity(float a) { auto* c = _comp(); if (c) (*c)["opacity"] = Mathf::clamp01(a); }
    // Unity name: spriteRenderer.color.a
    float Alpha() const { return Opacity(); }
    void SetAlpha(float a) { SetOpacity(a); }
    // Lerp the current tint towards a target color — call from Update() each frame.
    // Example: sr.LerpColor(SpriteRenderer2D::Color{255,255,255,1}, 5.f * dt)
    void LerpColor(Color target, float t) {
        Color cur = GetColor();
        SetColor({
            (Uint8)Mathf::lerp((float)cur.r, (float)target.r, t),
            (Uint8)Mathf::lerp((float)cur.g, (float)target.g, t),
            (Uint8)Mathf::lerp((float)cur.b, (float)target.b, t),
            Mathf::lerp(cur.a, target.a, t)
        });
    }

    // ── Hit feedback (Flash / Blink) ────────────────────────────────────────
    // The single most common visual feedback pattern in any game: tint the
    // sprite a color for a moment, then restore it. Previously this needed a
    // caller-written coroutine every time; now it's one call, backed by a
    // one-shot coroutine started on the owning script instance internally
    // (see ScriptBase::start_coroutine in script_system.hpp), so it shares
    // the same pause/timescale semantics as everything else.
    //
    //   SpriteRenderer().Flash(255, 60, 60, 0.1f); // red for 0.1s, then restores
    //
    // Calling Flash()/Blink() again while one is already running on this
    // instance starts a second coroutine — the most recently restored color
    // wins once both finish. For the common "restart the flash on every hit"
    // case, store the returned handle and StopCoroutine() it first if you
    // need the strict single-flash-at-a-time guarantee.
    coro::Handle Flash(Uint8 r, Uint8 g, Uint8 b, float duration = 0.1f, Uint8 a = 255) {
        _detect();
        if (!_script) return nullptr;
        Color restore = GetColor();
        ScriptBase* s = _script;
        return s->start_coroutine([s, r, g, b, a, duration, restore](coro::Coroutine& co) -> coro::CoroutineStep {
            CO_BODY_BEGIN(co);
            SpriteRenderer2D(EntityRef(s->entity)).SetColor(r, g, b, a);
            CO_WAIT_SECONDS(co, duration);
            SpriteRenderer2D(EntityRef(s->entity)).SetColor(restore);
            CO_BODY_END();
        });
    }
    // Overload taking a Color directly — mirrors SetColor(Color).
    coro::Handle Flash(Color flashColor, float duration = 0.1f) {
        return Flash(flashColor.r, flashColor.g, flashColor.b, duration, (Uint8)(Mathf::clamp01(flashColor.a) * 255.f));
    }

    // Blinks visibility on/off `count` times — the classic invincibility-
    // frames effect. Restores Enabled(true) when finished (even if `count`
    // is even, so the sprite never ends up stuck invisible if interrupted
    // logic forgets to clean up).
    //   SpriteRenderer().Blink(0.1f, 0.1f, 6); // 6 blinks during i-frames
    //
    // Implementation note: the coroutine body below uses a `while` loop with
    // its counter declared OUTSIDE the CO_BODY_BEGIN/END block (captured by
    // reference into a heap-held counter), not a `for` loop. CO_WAIT_SECONDS
    // resumes by jumping into the middle of a switch via `case __LINE__`,
    // and a jump into a `for(int i=...)` loop's body crosses that loop
    // variable's initialization — illegal in C++, and rejected at compile
    // time. Any coroutine body you write with CO_WAIT_* macros needs the
    // same care: keep loop counters declared before CO_BODY_BEGIN(co), and
    // use `while` rather than `for` around a wait.
    coro::Handle Blink(float onTime, float offTime, int count) {
        _detect();
        if (!_script || count <= 0) return nullptr;
        ScriptBase* s = _script;
        auto remaining = make_shared<int>(count);
        return s->start_coroutine([s, onTime, offTime, remaining](coro::Coroutine& co) -> coro::CoroutineStep {
            CO_BODY_BEGIN(co);
            while (*remaining > 0) {
                SpriteRenderer2D(EntityRef(s->entity)).SetEnabled(false);
                CO_WAIT_SECONDS(co, offTime);
                SpriteRenderer2D(EntityRef(s->entity)).SetEnabled(true);
                CO_WAIT_SECONDS(co, onTime);
                --(*remaining);
            }
            SpriteRenderer2D(EntityRef(s->entity)).SetEnabled(true);
            CO_BODY_END();
        });
    }

    // ── Visibility (Unity: spriteRenderer.enabled) ──────────────────────────────
    bool Enabled() const { auto* c = _comp(); return c ? c->value("enabled", true) : false; }
    void SetEnabled(bool v) { auto* c = _comp(); if (c) (*c)["enabled"] = v; }

    // ── Sorting ──────────────────────────────────────────────────────────────
    // Legacy flat layer (kept for old scripts/scenes — still read by
    // render_system.hpp as a final tiebreaker after sorting_layer/order_in_layer).
    int SortingOrder() const { auto* c = _comp(); return c ? c->value("layer", 0) : 0; }
    void SetSortingOrder(int order) { auto* c = _comp(); if (c) (*c)["layer"] = order; }

    // Unity's actual two-tier model: named sorting layer (resolved against
    // RenderSystem's project-wide ordered layer list) + an int tiebreaker
    // within that layer. This is what `sortingLayerName`/`sortingOrder`
    // mean in real Unity scripts.
    string SortingLayerName() const { auto* c = _comp(); return c ? c->value("sorting_layer", string()) : string(); }
    void SetSortingLayerName(string name) { auto* c = _comp(); if (c) (*c)["sorting_layer"] = name; }
    int OrderInLayer() const { auto* c = _comp(); return c ? c->value("order_in_layer", 0) : 0; }
    void SetOrderInLayer(int order) { auto* c = _comp(); if (c) (*c)["order_in_layer"] = order; }
    // Old name kept as an alias so existing call sites reading "SortingLayer()"
    // as an int (pre-named-layers) keep compiling; prefer SortingLayerName().
    int SortingLayer() const { return SortingOrder(); }
    void SetSortingLayer(int order) { SetSortingOrder(order); }

    // ── Draw Mode (Unity: spriteRenderer.drawMode / .size / .tileMode) ─────────
    // "Simple" = stretch to the Transform's scale (engine default/legacy).
    // "Tiled"   = texture repeats at its own pixel size to fill Size().
    // "Sliced"  = 9-slice using the sprite's border (see SetBorder below).
    // See render_system.hpp's _draw_sprite() draw_mode dispatch for the
    // actual rendering of each mode.
    enum class DrawMode { Simple, Tiled, Sliced };
    DrawMode GetDrawMode() const {
        auto* c = _comp(); if (!c) return DrawMode::Simple;
        string m = c->value("draw_mode", string("simple"));
        if (m == "tiled") return DrawMode::Tiled;
        if (m == "sliced") return DrawMode::Sliced;
        return DrawMode::Simple;
    }
    void SetDrawMode(DrawMode mode) {
        auto* c = _comp(); if (!c) return;
        (*c)["draw_mode"] = mode == DrawMode::Tiled ? string("tiled")
                            : mode == DrawMode::Sliced ? string("sliced")
                            : string("simple");
    }
    // Unity: spriteRenderer.size — only meaningful in Tiled/Sliced mode; this
    // is the world-space box the sprite tiles/9-slices to fill, independent
    // of the Transform's own scale (that's the whole point of Tiled mode:
    // the source art keeps its native pixel size while this box grows).
    Vector2 Size() const {
        auto* c = _comp(); if (!c) return {128.f, 128.f};
        return { c->value("tile_width", 128.f), c->value("tile_height", 128.f) };
    }
    void SetSize(Vector2 s) {
        auto* c = _comp(); if (!c) return;
        (*c)["tile_width"] = s.x; (*c)["tile_height"] = s.y;
    }
    void SetSize(float width, float height) { SetSize({width, height}); }

    // Unity: sprite.pixelsPerUnit — converts texture pixels to world units in
    // Tiled mode (how big one repeated tile is on screen).
    float PixelsPerUnit() const { auto* c = _comp(); return c ? c->value("pixels_per_unit", 100.f) : 100.f; }
    void SetPixelsPerUnit(float ppu) { auto* c = _comp(); if (c) (*c)["pixels_per_unit"] = std::max(1.f, ppu); }

    // Unity: spriteRenderer.tileMode (Continuous vs Adaptive) controls how a
    // Tiled sprite handles partial edge tiles. This engine always clips
    // partial tiles cleanly at the box edge (render_system.hpp's
    // _blit_tile_grid), which matches Unity's "Continuous" behavior — the
    // common case — so there's no separate setting to expose here.

    // ── 9-Slice border (Unity: sprite.border) ───────────────────────────────
    // Pixel insets from each edge of the source texture that stay unscaled
    // in Sliced draw mode; only the middle stretches. Order matches Unity's
    // Vector4 border (left, bottom, right, top) conceptually, exposed here as
    // four named values since this engine has no Vector4 type.
    struct Border { int left = 0, right = 0, top = 0, bottom = 0; };
    Border GetBorder() const {
        auto* c = _comp(); if (!c) return {};
        return { c->value("border_left",0), c->value("border_right",0),
                 c->value("border_top",0),  c->value("border_bottom",0) };
    }
    void SetBorder(int left, int right, int top, int bottom) {
        auto* c = _comp(); if (!c) return;
        (*c)["border_left"]=left; (*c)["border_right"]=right;
        (*c)["border_top"]=top;   (*c)["border_bottom"]=bottom;
    }
    // Unity: spriteRenderer.maskInteraction — whether the center/edges of a
    // Sliced sprite stretch to fill (true, Unity default) or are simply
    // omitted, leaving just the frame (false).
    bool FillCenter() const { auto* c = _comp(); return c ? c->value("sliced_fill_center", true) : true; }
    void SetFillCenter(bool v) { auto* c = _comp(); if (c) (*c)["sliced_fill_center"] = v; }

    // ── Pivot (Unity: sprite.pivot, normalized) ─────────────────────────────
    // (0,0) = sprite's bottom-left sits at the Transform position, (1,1) =
    // top-left, (0.5,0.5) = centered (engine default, matches the original
    // hardcoded center-pivot behavior before this wrapper existed).
    Vector2 Pivot() const {
        auto* c = _comp(); if (!c) return {0.5f, 0.5f};
        return { c->value("pivot_x", 0.5f), c->value("pivot_y", 0.5f) };
    }
    void SetPivot(Vector2 p) {
        auto* c = _comp(); if (!c) return;
        (*c)["pivot_x"] = Mathf::clamp01(p.x);
        (*c)["pivot_y"] = Mathf::clamp01(p.y);
    }
    void SetPivot(float x, float y) { SetPivot({x, y}); }
    // Named pivot presets — no need to remember which corner is which number.
    // SetPivotBottomCenter() is the most useful for platformer characters:
    // the Transform position sits at the feet, so jumping/landing math is
    // just "is my Y above the ground line?" rather than "Y + half-height".
    void SetPivotCenter()       { SetPivot(0.5f, 0.5f); }
    void SetPivotBottomCenter() { SetPivot(0.5f, 1.0f); } // feet at Transform.y
    void SetPivotTopCenter()    { SetPivot(0.5f, 0.0f); }
    void SetPivotBottomLeft()   { SetPivot(0.0f, 1.0f); }
    void SetPivotBottomRight()  { SetPivot(1.0f, 1.0f); }
    void SetPivotTopLeft()      { SetPivot(0.0f, 0.0f); }
    void SetPivotTopRight()     { SetPivot(1.0f, 0.0f); }

    // ── Mask Interaction (Unity: spriteRenderer.maskInteraction) ───────────────
    // How this sprite reacts to the nearest SpriteMask component below it in
    // sort order — see render_system.hpp's _draw_sprite_mask()/_draw_sprite()
    // clip-rect handling.
    enum class MaskInteraction { None, VisibleInsideMask, VisibleOutsideMask };
    MaskInteraction GetMaskInteraction() const {
        auto* c = _comp(); if (!c) return MaskInteraction::None;
        string m = c->value("mask_interaction", string("none"));
        if (m == "visible_inside_mask") return MaskInteraction::VisibleInsideMask;
        if (m == "visible_outside_mask") return MaskInteraction::VisibleOutsideMask;
        return MaskInteraction::None;
    }
    void SetMaskInteraction(MaskInteraction mode) {
        auto* c = _comp(); if (!c) return;
        (*c)["mask_interaction"] = mode == MaskInteraction::VisibleInsideMask ? string("visible_inside_mask")
                                  : mode == MaskInteraction::VisibleOutsideMask ? string("visible_outside_mask")
                                  : string("none");
    }

    // ── Filter Mode (Unity: texture.filterMode, exposed per-sprite here) ───────
    // "Point" = nearest-neighbor (crisp pixel art, engine default), "Bilinear"
    // = smoothed. The closest equivalent this engine has to Unity's
    // material/shader-level texture filtering, since there's no full
    // material system to swap (Material()/SharedMaterial() below are no-ops
    // for the same reason — documented there rather than silently missing).
    enum class FilterMode { Point, Bilinear };
    FilterMode GetFilterMode() const {
        auto* c = _comp(); if (!c) return FilterMode::Point;
        return c->value("filter_mode", string("point")) == "bilinear" ? FilterMode::Bilinear : FilterMode::Point;
    }
    void SetFilterMode(FilterMode mode) {
        auto* c = _comp(); if (!c) return;
        (*c)["filter_mode"] = mode == FilterMode::Bilinear ? string("bilinear") : string("point");
    }

    // ── Material (Unity: spriteRenderer.material / .sharedMaterial) ────────────
    // Returns/sets the path to this sprite's .material asset (empty = the
    // engine's built-in default material — plain alpha blend, no tint).
    // See material_system.hpp for the asset format and render_system.hpp's
    // _draw_sprite() for how a material's shader/tint/filter/texture
    // override actually get applied at draw time. This engine has one
    // physical material per path (Unity's sharedMaterial model) rather than
    // per-instance materials, so Material() and SharedMaterial() are the
    // same call here — scripts that only ever read/write .material (the
    // overwhelmingly common case) work unchanged either way.
    string Material() const { auto* c = _comp(); return c ? c->value("material", string()) : string(); }
    void SetMaterial(string materialPath) { auto* c = _comp(); if (c) (*c)["material"] = materialPath; }
    string SharedMaterial() const { return Material(); }
    void SetSharedMaterial(string materialPath) { SetMaterial(materialPath); }

    // ── Bounds (Unity: spriteRenderer.bounds, world-space AABB) ────────────────
    // Approximates Unity's bounds: in Simple mode this is the sprite's native
    // pixel size scaled by the Transform; in Tiled/Sliced mode it's exactly
    // Size() (the box the sprite fills), since that's the renderer's actual
    // on-screen footprint regardless of texture content. Native pixel size
    // isn't queryable from script (no texture-introspection API on this
    // façade), so Simple-mode bounds use the Transform scale only — accurate
    // whenever a source rect or known sprite size is set via SetSourceRect.
    struct Bounds { Vector2 center; Vector2 size; };
    Bounds GetBounds() const {
        _detect();
        Bounds b;
        if (!_script) return b;
        b.center = {_script->get_world_x(), _script->get_world_y()};
        if (GetDrawMode() != DrawMode::Simple) {
            b.size = Size();
            return b;
        }
        Rect src = SourceRect();
        float base_w = src.w > 0 ? (float)src.w : 32.f;
        float base_h = src.h > 0 ? (float)src.h : 32.f;
        b.size = { base_w * _script->get_scale_x(), base_h * _script->get_scale_y() };
        return b;
    }

    Entity* _comp() const {
        _detect();
        if (_script) return _script->get_component("SpriteRenderer");
        if (_entity) {
            if (!_entity.Contains("components") || !_entity["components"].contains("SpriteRenderer")) return nullptr;
            return const_cast<Entity*>(&_entity["components"]["SpriteRenderer"]);
        }
        return nullptr;
    }
private:
    mutable ScriptBase* _script = nullptr;
    EntityRef _entity;
    mutable bool _detected = false;
    void _detect() const {
        if (!_detected) {
            _detected = true;
            if (!_script)
                const_cast<SpriteRenderer2D*>(this)->_script = ScriptBase::current();
        }
    }
};


// Unity-style wrapper around the engine's "Animator" component, which
// AnimatorSystem (systems.hpp) drives every frame. The component holds a
// flat sprite-flipbook clip table ("animations": name -> frames/fps/loop/
// ping_pong), a "parameters" dict (floats/ints/bools/triggers), and now also
// a transition graph ("transitions": from/to/conditions/duration/exit-time)
// and blend trees ("blend_trees": param-driven nearest-clip selection) —
// see systems.hpp's AnimatorSystem for the authoritative evaluation logic
// and an explanation of what "blending" means in a single-sprite-per-frame
// renderer (nearest-neighbor clip selection, not alpha cross-dissolve).
//
// Trigger semantics match Unity: SetTrigger marks a trigger as armed; it's
// consumed either by a transition condition firing (AnimatorSystem) or by a
// script calling ConsumeTrigger()/GetTrigger()+ResetTrigger(). Triggers are
// stored in parameters under a "__trig_" prefix so they don't collide with
// same-named bool/float/int parameters.
class Animator {
public:
    Animator() = default;
    explicit Animator(EntityRef e) : _entity(e) {}

    explicit operator bool() const { return _comp() != nullptr; }

    // ── Playback ─────────────────────────────────────────────────────────────
    // Play: Unity signature is Play(stateName, layer=-1, normalizedTime=NaN).
    // Layers aren't modeled by this engine's Animator, so `layer` is accepted
    // and ignored for now; normalizedTime (0..1) seeks into the clip if given.
    void Play(string stateName, int /*layer*/ = -1, float normalizedTime = -1.f) {
        auto* a = _comp(); if (!a) return;
        (*a)["current_animation"] = stateName;
        (*a)["playing"] = true;
        if (normalizedTime >= 0.f) {
            int len = ClipLength(stateName);
            (*a)["frame"] = len > 0 ? normalizedTime * (float)len : 0.f;
        } else {
            (*a)["frame"] = 0.f;
        }
        // Clear the "already fired" guard for this clip so a replay (e.g. an
        // attack animation triggered again) refires its frame-0 events
        // instead of AnimatorSystem thinking it already delivered them.
        (*a)["_last_event_frame__" + stateName] = -1;
    }

    // No true cross-fade blending exists at the engine level (single active
    // clip per Animator), so CrossFade behaves like Play: it switches the
    // current animation immediately. Exposed under Unity's name so existing
    // Unity-style scripts compile and run unmodified; `duration`/`layer` are
    // accepted for signature compatibility but currently have no blending
    // effect.
    void CrossFade(string stateName, float /*duration*/ = 0.f, int layer = -1) {
        Play(stateName, layer);
    }
    void CrossFadeInFixedTime(string stateName, float duration = 0.f, int layer = -1) {
        CrossFade(stateName, duration, layer);
    }

    void Stop() { auto* a = _comp(); if (a) (*a)["playing"] = false; }
    void Resume() { auto* a = _comp(); if (a) (*a)["playing"] = true; }

    bool IsPlaying() const { auto* a = _comp(); return a && a->value("playing", false); }

    // True when a non-looping clip has played past its last frame and stopped.
    // Useful for one-shot animations (attacks, deaths, transitions) where you
    // need to know exactly when they end without polling NormalizedTime() >= 1.
    //   if (anim.IsFinished()) TransitionToIdleState();
    bool IsFinished() const {
        auto* a = _comp(); if (!a) return true;
        if (a->value("playing", false)) return false;
        string cur = a->value("current_animation", string());
        if (cur.empty()) return true;
        if (!a->contains("animations")) return true;
        auto& anims = (*a)["animations"];
        if (!anims.contains(cur)) return true;
        auto& clip = anims[cur];
        bool loops = clip.is_object() ? clip.value("loop", true) : true;
        return !loops; // looping clips are never "finished" by this definition
    }

    // Play only if a different clip is not already active — avoids restarting
    // the animation from frame 0 on every Update() call.
    //   anim.PlayIfNotPlaying("Run");  // safe to call every frame
    void PlayIfNotPlaying(string stateName) {
        if (CurrentStateName() != stateName || !IsPlaying()) Play(stateName);
    }

    // Restart the current clip from the beginning. Useful for edge cases
    // where you want the same animation to replay (e.g. a double-jump resets
    // the jump arc animation without transitioning to a different state).
    void Restart() {
        auto* a = _comp(); if (!a) return;
        (*a)["frame"] = 0.f;
        (*a)["playing"] = true;
    }

    // ── speed / update / culling ────────────────────────────────────────────
    float Speed() const { auto* a = _comp(); return a ? a->value("speed", 1.f) : 1.f; }
    void SetSpeed(float s) { auto* a = _comp(); if (a) (*a)["speed"] = s; }

    // updateMode/cullingMode aren't simulated by this engine (animation always
    // advances with scaled game time, every frame, regardless of visibility);
    // these are stored so a script can still read back what it set, in case a
    // future AnimatorSystem revision starts honoring them.
    void SetUpdateMode(string mode) { auto* a = _comp(); if (a) (*a)["update_mode"] = mode; }
    string UpdateMode() const { auto* a = _comp(); return a ? a->value("update_mode", string("Normal")) : string("Normal"); }
    void SetCullingMode(string mode) { auto* a = _comp(); if (a) (*a)["culling_mode"] = mode; }
    string CullingMode() const { auto* a = _comp(); return a ? a->value("culling_mode", string("AlwaysAnimate")) : string("AlwaysAnimate"); }

    // runtimeAnimatorController equivalent: which named clip table is bound.
    // This engine keeps clips inline on the component ("animations"), so
    // there's no separate controller asset to swap — this just reports/sets
    // a label for scripts that want to branch on "which controller is this".
    string RuntimeAnimatorController() const { auto* a = _comp(); return a ? a->value("controller", string()) : string(); }
    void SetRuntimeAnimatorController(string name) { auto* a = _comp(); if (a) (*a)["controller"] = name; }

    // ── Parameters: Float / Int / Bool / Trigger ────────────────────────────
    void SetFloat(string name, float value) { _params()[name] = value; }
    float GetFloat(string name, float def = 0.f) const {
        auto* p = _params_const(); return p ? p->value(name, def) : def;
    }

    void SetInt(string name, int value) { _params()[name] = value; }
    int GetInt(string name, int def = 0) const {
        auto* p = _params_const(); return p ? p->value(name, def) : def;
    }

    void SetBool(string name, bool value) { _params()[name] = value; }
    bool GetBool(string name, bool def = false) const {
        auto* p = _params_const(); return p ? p->value(name, def) : def;
    }

    // Triggers are stored separately from bools so a same-named bool and
    // trigger parameter never collide.
    void SetTrigger(string name) { _params()["__trig_" + name] = true; }
    void ResetTrigger(string name) { _params()["__trig_" + name] = false; }
    bool GetTrigger(string name) const {
        auto* p = _params_const(); return p ? p->value("__trig_" + name, false) : false;
    }
    // Convenience matching common Unity script patterns: check-and-clear in
    // one call, e.g. `if (anim.ConsumeTrigger("Jump")) { ... }`.
    bool ConsumeTrigger(string name) {
        if (!GetTrigger(name)) return false;
        ResetTrigger(name);
        return true;
    }

    bool HasParameter(string name) const {
        auto* p = _params_const();
        return p && (p->contains(name) || p->contains("__trig_" + name));
    }

    // ── Parameter introspection ─────────────────────────────────────────────
    // Unity's Animator.parameters / GetParameter(i) equivalent. Since this
    // engine's parameters are a flat dict rather than typed asset metadata,
    // "type" here is inferred from the stored value, and triggers (stored
    // under the internal "__trig_" prefix) are reported under their public
    // name with type "Trigger" so a script can enumerate them uniformly.
    struct ParamInfo { string name; string type; };
    vector<ParamInfo> Parameters() const {
        vector<ParamInfo> out;
        auto* p = _params_const(); if (!p || !p->is_object()) return out;
        for (auto& kv : p->items()) {
            string key = kv.first;
            const Entity& v = kv.second;
            if (key.rfind("__trig_", 0) == 0) {
                out.push_back({key.substr(7), "Trigger"});
            } else if (v.is_bool()) {
                out.push_back({key, "Bool"});
            } else if (v.is_number_integer()) {
                out.push_back({key, "Int"});
            } else if (v.is_number()) {
                out.push_back({key, "Float"});
            }
        }
        return out;
    }
    int ParameterCount() const { return (int)Parameters().size(); }

    // ── Animation events ─────────────────────────────────────────────────────
    // Attach a named event to a specific frame of a clip (Unity: an
    // AnimationEvent on the clip's timeline). AnimatorSystem fires
    // ScriptBase::on_animation_event(name) -- override OnAnimationEvent in a
    // MonoBehaviour-derived script -- the frame AnimatorSystem advances onto
    // that index. Delivery is queued through the same _pending_events
    // mechanism collision/trigger callbacks use, so it arrives on the
    // following ScriptSystem update (one frame later) rather than
    // synchronously mid-AnimatorSystem::update -- see ScriptBase::
    // on_animation_event's comment for why.
    void AddEvent(string clipName, int frameIndex, string eventName) {
        auto* a = _comp(); if (!a || !a->contains("animations")) return;
        auto& anims = (*a)["animations"];
        if (!anims.contains(clipName) || !anims[clipName].is_object()) return;
        auto& clip = anims[clipName];
        if (!clip.contains("events") || !clip["events"].is_array()) clip["events"] = Entity::array();
        Entity ev = Entity::object();
        ev["frame"] = frameIndex;
        ev["name"] = eventName;
        clip["events"].push_back(ev);
    }
    void ClearEvents(string clipName) {
        auto* a = _comp(); if (!a || !a->contains("animations")) return;
        auto& anims = (*a)["animations"];
        if (anims.contains(clipName) && anims[clipName].is_object()) anims[clipName]["events"] = Entity::array();
    }

    // ── Layers ───────────────────────────────────────────────────────────────
    // Unity layers normally composite (e.g. a full-body locomotion layer plus
    // a masked upper-body "aim" layer blended on top via bone masks). This
    // renderer draws one SpriteRenderer texture per entity per frame, so true
    // mask-based compositing of two animations isn't possible here. What IS
    // implemented: each layer is an independent state machine (its own
    // current state, frame, playing flag, advanced by AnimatorSystem exactly
    // like the base layer); every frame, the highest-index layer with
    // weight > 0 decides which clip actually drives the SpriteRenderer
    // (Unity calls this "override" blend mode -- the only mode that makes
    // sense for a single flat sprite). Layer 0 ("Base Layer") always exists
    // implicitly and maps to the Animator's own top-level state, so existing
    // scripts that never touch layers keep working unchanged.
    void AddLayer(string layerName, float weight = 1.f) {
        auto* a = _comp(); if (!a) return;
        if (!a->contains("layers") || !(*a)["layers"].is_array()) (*a)["layers"] = Entity::array();
        for (auto& l : (*a)["layers"]) if (l.value("name", string()) == layerName) { l["weight"] = weight; return; }
        Entity layer = Entity::object();
        layer["name"] = layerName;
        layer["weight"] = weight;
        layer["current_animation"] = string();
        layer["frame"] = 0.f;
        layer["playing"] = false;
        (*a)["layers"].push_back(layer);
    }
    void SetLayerWeight(int layerIndex, float weight) {
        auto* a = _comp(); if (!a || !a->contains("layers")) return;
        auto& layers = (*a)["layers"];
        int idx = layerIndex - 1; // layer 0 is the implicit base layer
        if (idx < 0 || idx >= (int)layers.size()) return;
        layers[idx]["weight"] = weight;
    }
    float GetLayerWeight(int layerIndex) const {
        if (layerIndex == 0) return 1.f;
        auto* a = _comp(); if (!a || !a->contains("layers")) return 0.f;
        auto& layers = (*a)["layers"];
        int idx = layerIndex - 1;
        if (idx < 0 || idx >= (int)layers.size()) return 0.f;
        return layers[idx].value("weight", 1.f);
    }
    int LayerCount() const {
        auto* a = _comp(); if (!a || !a->contains("layers")) return 1; // implicit base layer
        return 1 + (int)(*a)["layers"].size();
    }
    string GetLayerName(int layerIndex) const {
        if (layerIndex == 0) return "Base Layer";
        auto* a = _comp(); if (!a || !a->contains("layers")) return {};
        auto& layers = (*a)["layers"];
        int idx = layerIndex - 1;
        if (idx < 0 || idx >= (int)layers.size()) return {};
        return layers[idx].value("name", string());
    }
    // Plays a clip on a specific override layer (layer 0 == Play(stateName)).
    void PlayOnLayer(string stateName, int layerIndex) {
        if (layerIndex <= 0) { Play(stateName); return; }
        auto* a = _comp(); if (!a || !a->contains("layers")) return;
        auto& layers = (*a)["layers"];
        int idx = layerIndex - 1;
        if (idx < 0 || idx >= (int)layers.size()) return;
        layers[idx]["current_animation"] = stateName;
        layers[idx]["frame"] = 0.f;
        layers[idx]["playing"] = true;
    }

    // ── State info ───────────────────────────────────────────────────────────
    // Stand-in for GetCurrentAnimatorStateInfo(layer).IsName(...)/.normalized
    // Time, scoped to what this engine actually tracks: which clip is current,
    // its progress, and whether it's still playing. There is no layer system,
    // so `layer` is accepted for signature compatibility and ignored.
    string CurrentStateName(int /*layer*/ = 0) const {
        auto* a = _comp(); if (!a) return {};
        return a->value("current_animation", a->value("autoplay_clip", a->value("state", string())));
    }
    bool IsName(string stateName, int layer = 0) const { return CurrentStateName(layer) == stateName; }

    int ClipLength(string stateName) const {
        auto* a = _comp(); if (!a || !a->contains("animations")) return 0;
        auto& anims = (*a)["animations"];
        if (!anims.contains(stateName)) return 0;
        auto& clip = anims[stateName];
        if (clip.is_array()) return (int)clip.size();
        if (clip.is_object()) {
            auto frames = clip.value("frames", clip.value("textures", Entity::array()));
            return (int)frames.size();
        }
        return 0;
    }

    // 0..1 progress through the current clip; loops/ping-pongs wrap the same
    // way AnimatorSystem's own frame indexing does, so this matches what's
    // actually on screen rather than a naive frame/length divide.
    float NormalizedTime(int layer = 0) const {
        auto* a = _comp(); if (!a) return 0.f;
        string cur = CurrentStateName(layer);
        int len = ClipLength(cur);
        if (len <= 0) return 0.f;
        float frame = a->value("frame", 0.f);
        bool loop = ClipLoop(cur, a->value("loop", true));
        bool pp   = ClipPingPong(cur, a->value("ping_pong", false));
        int raw = (int)frame;
        if (pp && len > 1) {
            int cycle = len * 2 - 2;
            int idx = raw % cycle; if (idx >= len) idx = cycle - idx;
            return (float)std::max(0, std::min(len - 1, idx)) / (float)len;
        }
        if (loop) return (float)(raw % len) / (float)len;
        return (float)std::min(len - 1, raw) / (float)len;
    }

    bool IsInTransition(int /*layer*/ = 0) const {
        auto* a = _comp();
        if (!a || !a->contains("_transition")) return false;
        auto& tr = (*a)["_transition"];
        return tr.is_object() && !tr.is_null();
    }

    // ── Clip authoring helpers ───────────────────────────────────────────────
    // Define (or overwrite) a named clip by supplying its frame list inline.
    // Eliminates the raw JSON boilerplate normally needed to author clips in
    // Awake(). All parameters have sane defaults so the minimal call is:
    //   anim.DefineClip("Run", {"run_0","run_1","run_2","run_3"});
    void DefineClip(string name,
                    vector<string> frames,
                    float fps = 12.f,
                    bool loop = true,
                    bool pingPong = false) {
        auto* a = _comp(); if (!a) return;
        if (!a->contains("animations") || (*a)["animations"].is_null())
            (*a)["animations"] = Entity::object();
        Entity clip = Entity::object();
        Entity farr = Entity::array();
        for (auto& f : frames) farr.push_back(f);
        clip["frames"]    = farr;
        clip["fps"]       = fps;
        clip["loop"]      = loop;
        clip["ping_pong"] = pingPong;
        (*a)["animations"][name] = clip;
    }

    // Like DefineClip but for sprite-sheet mode — supply the sheet path,
    // frame size, and the column indices that belong to this clip.
    // Sets up the Animator component-level sheet fields too so you don't
    // have to touch them separately.
    //   anim.DefineClipSheet("Walk", "player_sheet.png", 32, 32, {0,1,2,3});
    void DefineClipSheet(string name,
                         string sheetPath,
                         int frameW, int frameH,
                         vector<int> frameIndices,
                         float fps = 12.f,
                         bool loop = true,
                         int sheetColumns = 8) {
        auto* a = _comp(); if (!a) return;
        (*a)["use_sprite_sheet"] = true;
        (*a)["sprite_sheet"]     = sheetPath;
        (*a)["frame_width"]      = frameW;
        (*a)["frame_height"]     = frameH;
        (*a)["sheet_columns"]    = sheetColumns;
        if (!a->contains("animations") || (*a)["animations"].is_null())
            (*a)["animations"] = Entity::object();
        Entity clip = Entity::object();
        Entity farr = Entity::array();
        for (int idx : frameIndices) farr.push_back(idx);
        clip["frames"]    = farr;
        clip["fps"]       = fps;
        clip["loop"]      = loop;
        (*a)["animations"][name] = clip;
    }

    // Remove a single named clip. Useful when swapping controller sets at runtime.
    void RemoveClip(string name) {
        auto* a = _comp(); if (!a || !a->contains("animations")) return;
        (*a)["animations"].erase(name);
    }

    // ── Transition graph authoring ──────────────────────────────────────────
    // Adds one entry to anim["transitions"]; evaluated by AnimatorSystem each
    // frame (see systems.hpp). `from` may be "*"/"Any" for an Any-State
    // transition. duration==0 switches instantly once conditions are met;
    // duration>0 holds the source clip until the timer elapses, then snaps to
    // the target (see AnimatorSystem header comment for why this isn't a
    // visual cross-dissolve — the renderer draws a single sprite texture).
    struct Condition {
        string param;
        string op;     // "trigger" | "bool_true" | "bool_false" | "greater" | "less" | "equals" | "notequal"
        float value = 0.f;
    };
    static Condition When(string param) { return {param, "trigger", 0.f}; }
    static Condition WhenTrue(string param) { return {param, "bool_true", 0.f}; }
    static Condition WhenFalse(string param) { return {param, "bool_false", 0.f}; }
    static Condition WhenGreater(string param, float v) { return {param, "greater", v}; }
    static Condition WhenLess(string param, float v) { return {param, "less", v}; }
    static Condition WhenEquals(string param, float v) { return {param, "equals", v}; }

    void AddTransition(string from, string to,
                        vector<Condition> conditions = {},
                        float duration = 0.f,
                        bool hasExitTime = false, float exitTime = 1.f) {
        auto* a = _comp(); if (!a) return;
        if (!a->contains("transitions") || !(*a)["transitions"].is_array()) (*a)["transitions"] = Entity::array();
        Entity t = Entity::object();
        t["from"] = from;
        t["to"] = to;
        t["duration"] = duration;
        t["has_exit_time"] = hasExitTime;
        t["exit_time"] = exitTime;
        Entity conds = Entity::array();
        for (auto& c : conditions) {
            Entity co = Entity::object();
            co["param"] = c.param; co["op"] = c.op; co["value"] = c.value;
            conds.push_back(co);
        }
        t["conditions"] = conds;
        (*a)["transitions"].push_back(t);
    }
    // Shortcut for a trigger-only Any-State transition, the most common case
    // ("from anywhere, on Jump trigger, go to Jump").
    void AddAnyStateTransition(string to, string triggerParam, float duration = 0.f) {
        AddTransition("*", to, {When(triggerParam)}, duration);
    }
    void ClearTransitions() { auto* a = _comp(); if (a) (*a)["transitions"] = Entity::array(); }

    // ── Blend tree authoring ────────────────────────────────────────────────
    // Defines `stateName` as a 1D blend tree over `param`: AnimatorSystem
    // picks the nearest child clip by |param - threshold| every frame (see
    // systems.hpp's _resolve_state). Play(stateName)/CrossFade(stateName)
    // both work normally once this is set up — the tree name acts as the
    // current_animation value, same as a normal clip name.
    void SetBlendTree1D(string stateName, string param,
                         vector<pair<string,float>> clipsAndThresholds) {
        auto* a = _comp(); if (!a) return;
        if (!a->contains("blend_trees") || (*a)["blend_trees"].is_null()) (*a)["blend_trees"] = Entity::object();
        Entity tree = Entity::object();
        tree["type"] = "1d";
        tree["param_x"] = param;
        Entity children = Entity::array();
        for (auto& [clip, thr] : clipsAndThresholds) {
            Entity c = Entity::object(); c["clip"] = clip; c["threshold"] = thr;
            children.push_back(c);
        }
        tree["children"] = children;
        (*a)["blend_trees"][stateName] = tree;
    }

    // 2D blend tree: nearest child by Euclidean distance in (param_x, param_y).
    struct BlendPoint2D { string clip; float x = 0.f; float y = 0.f; };
    void SetBlendTree2D(string stateName, string paramX, string paramY,
                         vector<BlendPoint2D> points) {
        auto* a = _comp(); if (!a) return;
        if (!a->contains("blend_trees") || (*a)["blend_trees"].is_null()) (*a)["blend_trees"] = Entity::object();
        Entity tree = Entity::object();
        tree["type"] = "2d";
        tree["param_x"] = paramX;
        tree["param_y"] = paramY;
        Entity children = Entity::array();
        for (auto& p : points) {
            Entity c = Entity::object(); c["clip"] = p.clip; c["pos_x"] = p.x; c["pos_y"] = p.y;
            children.push_back(c);
        }
        tree["children"] = children;
        (*a)["blend_trees"][stateName] = tree;
    }

    // Which concrete clip a blend-tree state currently resolves to (useful
    // for debugging / driving other logic off "what's actually drawing").
    string ResolvedClip() const {
        auto* a = _comp(); if (!a) return {};
        string cur = CurrentStateName();
        if (!a->contains("blend_trees") || !(*a)["blend_trees"].contains(cur)) return cur;
        // Mirror AnimatorSystem::_resolve_state's nearest-neighbor pick so a
        // script can query it without duplicating engine internals.
        auto& tree = const_cast<Entity&>(*a)["blend_trees"][cur];
        if (!tree.contains("children") || !tree["children"].is_array() || tree["children"].empty()) return cur;
        auto& params = _params_const() ? *const_cast<Entity*>(_params_const()) : const_cast<Entity&>(*a)["parameters"];
        string type = tree.value("type", string("1d"));
        string best = cur; float best_d = numeric_limits<float>::infinity();
        if (type == "2d") {
            float px = params.value(tree.value("param_x", string("X")), 0.f);
            float py = params.value(tree.value("param_y", string("Y")), 0.f);
            for (auto& ch : tree["children"]) {
                float cx = ch.value("pos_x", 0.f), cy = ch.value("pos_y", 0.f);
                float d = (px-cx)*(px-cx) + (py-cy)*(py-cy);
                if (d < best_d) { best_d = d; best = ch.value("clip", cur); }
            }
        } else {
            float pv = params.value(tree.value("param_x", string("X")), 0.f);
            for (auto& ch : tree["children"]) {
                float d = std::abs(pv - ch.value("threshold", 0.f));
                if (d < best_d) { best_d = d; best = ch.value("clip", cur); }
            }
        }
        return best;
    }

    Entity* _comp() const {
        _detect();
        if (_script) return _script->get_component("Animator");
        if (_entity) {
            if (!_entity.Contains("components") || !_entity["components"].contains("Animator")) return nullptr;
            return const_cast<Entity*>(&_entity["components"]["Animator"]);
        }
        return nullptr;
    }
    Entity& _params() {
        auto* a = _comp();
        static Entity dummy = Entity::object();
        if (!a) return dummy;
        if (!a->contains("parameters") || (*a)["parameters"].is_null()) (*a)["parameters"] = Entity::object();
        return (*a)["parameters"];
    }
    const Entity* _params_const() const {
        auto* a = _comp();
        if (!a || !a->contains("parameters")) return nullptr;
        return &(*a)["parameters"];
    }
    bool ClipLoop(string stateName, bool fallback) const {
        auto* a = _comp(); if (!a || !a->contains("animations")) return fallback;
        auto& anims = (*a)["animations"];
        if (!anims.contains(stateName) || !anims[stateName].is_object()) return fallback;
        return anims[stateName].value("loop", fallback);
    }
    bool ClipPingPong(string stateName, bool fallback) const {
        auto* a = _comp(); if (!a || !a->contains("animations")) return fallback;
        auto& anims = (*a)["animations"];
        if (!anims.contains(stateName) || !anims[stateName].is_object()) return fallback;
        return anims[stateName].value("ping_pong", fallback);
    }

private:
    mutable ScriptBase* _script = nullptr;
    EntityRef _entity;
    mutable bool _detected = false;
    void _detect() const {
        if (!_detected) {
            _detected = true;
            if (!_script)
                const_cast<Animator*>(this)->_script = ScriptBase::current();
        }
    }
};

// ─── AudioSource2D ─────────────────────────────────────────────────────────
// Unity-style wrapper around the engine's AudioSource component.
// Mirrors Unity's AudioSource API: clip, volume, pitch, loop, Play/Stop/IsPlaying.
class AudioSource2D {
public:
    AudioSource2D() = default;
    explicit AudioSource2D(EntityRef e) : _entity(e) {}

    explicit operator bool() const { return _comp() != nullptr; }

    // ── Playback ────────────────────────────────────────────────────────────
    void Play() { auto* c = _comp(); if (c) { (*c)["_play_now"] = true; } }
    void Stop() { auto* c = _comp(); if (c) { (*c)["_is_playing"] = false; (*c)["_play_now"] = false; } }
    bool IsPlaying() const { auto* c = _comp(); return c && c->value("_is_playing", false); }
    void PlayOneShot(string clipPath, float volume = 1.f) {
        auto* c = _comp(); if (!c) return;
        (*c)["_oneshot_clip"] = clipPath;
        (*c)["_oneshot_volume"] = volume;
        (*c)["_oneshot_play"] = true;
    }
    void PlaySound(string clipPath) { PlayOneShot(clipPath); }

    // ── Clip ────────────────────────────────────────────────────────────────
    string Clip() const { auto* c = _comp(); return c ? c->value("clip", string()) : string(); }
    void SetClip(string clipPath) { auto* c = _comp(); if (c) (*c)["clip"] = clipPath; }
    float Time() const { auto* c = _comp(); return c ? c->value("_playback_time", 0.f) : 0.f; }
    void SetTime(float t) { auto* c = _comp(); if (c) (*c)["_playback_time"] = t; }

    // ── Volume / Pitch / Loop ───────────────────────────────────────────────
    float Volume() const { auto* c = _comp(); return c ? c->value("volume", 1.f) : 1.f; }
    void SetVolume(float v) { auto* c = _comp(); if (c) (*c)["volume"] = v; }
    float Pitch() const { auto* c = _comp(); return c ? c->value("pitch", 1.f) : 1.f; }
    void SetPitch(float p) { auto* c = _comp(); if (c) (*c)["pitch"] = p; }
    bool Loop() const { auto* c = _comp(); return c && c->value("loop", false); }
    void SetLoop(bool v) { auto* c = _comp(); if (c) (*c)["loop"] = v; }

    // ── Spatial (Unity: minDistance / maxDistance / spatialBlend) ────────────
    // This engine does not simulate 3D spatial audio, but stores the fields
    // so a script can read back what it set for future use.
    float MinDistance() const { auto* c = _comp(); return c ? c->value("min_distance", 1.f) : 1.f; }
    void SetMinDistance(float d) { auto* c = _comp(); if (c) (*c)["min_distance"] = d; }
    float MaxDistance() const { auto* c = _comp(); return c ? c->value("max_distance", 500.f) : 500.f; }
    void SetMaxDistance(float d) { auto* c = _comp(); if (c) (*c)["max_distance"] = d; }
    float SpatialBlend() const { auto* c = _comp(); return c ? c->value("spatial_blend", 0.f) : 0.f; }
    void SetSpatialBlend(float v) { auto* c = _comp(); if (c) (*c)["spatial_blend"] = v; }

    // ── State helpers ───────────────────────────────────────────────────────
    bool IsDone() const { return !IsPlaying(); }
    void StopWithFadeOut(float /*duration*/) { Stop(); } // no fade support yet

    Entity* _comp() const {
        _detect();
        if (_script) return _script->get_component("AudioSource");
        if (_entity) {
            if (!_entity.Contains("components") || !_entity["components"].contains("AudioSource")) return nullptr;
            return const_cast<Entity*>(&_entity["components"]["AudioSource"]);
        }
        return nullptr;
    }
private:
    mutable ScriptBase* _script = nullptr;
    EntityRef _entity;
    mutable bool _detected = false;
    void _detect() const {
        if (!_detected) {
            _detected = true;
            if (!_script)
                const_cast<AudioSource2D*>(this)->_script = ScriptBase::current();
        }
    }
};

// ─── Typed GetComponent<T>() dispatch ──────────────────────────────────────
// Unity scripts almost never call the stringly-typed GetComponent("Foo");
// they call GetComponent<Foo>() and get a real typed wrapper back. C++ has
// no runtime reflection to make that automatic, so this is a small per-
// wrapper-type specialization table instead: each supported T gets one
// explicit specialization of component_traits<T>::get(ScriptBase*), and
// MonoBehaviour::GetComponent<T>()/RequireComponent<T>() below just forward
// into it. Adding a new wrapper class later (e.g. a future Collider2D
// wrapper) means adding one more specialization here — nothing about the
// call site in user scripts needs to change.
//
// Every wrapper class already follows the same shape (constructible from a
// ScriptBase*, with an explicit operator bool() reporting whether the
// underlying component actually exists on the entity), so get() and the
// "is it present" check both reduce to that one pattern per type.
template <class T>
struct component_traits; // no generic definition — only specializations below are valid

#define NOVA_DEFINE_COMPONENT_TRAIT(WrapperType)                              \
    template <> struct component_traits<WrapperType> {                       \
        static WrapperType get(ScriptBase* s) { return WrapperType(EntityRef(s->entity)); } \
        static const char* type_name() { return #WrapperType; }               \
    };

NOVA_DEFINE_COMPONENT_TRAIT(Transform2D)
NOVA_DEFINE_COMPONENT_TRAIT(Rigidbody2D)
NOVA_DEFINE_COMPONENT_TRAIT(SpriteRenderer2D)
NOVA_DEFINE_COMPONENT_TRAIT(Animator)
NOVA_DEFINE_COMPONENT_TRAIT(AudioSource2D)

#undef NOVA_DEFINE_COMPONENT_TRAIT

class GameObject {
public:
    GameObject() = default;
    explicit GameObject(EntityRef e) : _entity(e) {}

    explicit operator bool() const {
        if (_entity) return true;
        if (!_detected) {
            _detected = true;
            if (!_script) const_cast<GameObject*>(this)->_script = ScriptBase::current();
        }
        return _script && _script->entity;
    }

    string Name() const {
        auto* e = _ptr();
        return e ? e->value("name", string{}) : string{};
    }
    string Tag() const {
        auto* e = _ptr();
        return e ? e->value("tag", string{}) : string{};
    }
    bool CompareTag(string t) const {
        auto* e = _ptr();
        return e && e->value("tag", string{}) == t;
    }

    bool Active() const {
        auto* e = _ptr();
        return e ? e->value("active", true) : false;
    }
    void SetActive(bool active) { auto* e = _ptr(); if (e) (*e)["active"] = active; }

    EntityRef GetEntity() const { return _entity ? _entity : (_script && _script->entity ? EntityRef(_script->entity) : EntityRef()); }
    Transform2D Transform() const { return _entity ? Transform2D(_entity) : component_traits<Transform2D>::get(_script); }
    Rigidbody2D Rigidbody() const { return _entity ? Rigidbody2D(_entity) : component_traits<Rigidbody2D>::get(_script); }
    Animator GetAnimator() const { return _entity ? Animator(_entity) : component_traits<Animator>::get(_script); }
    SpriteRenderer2D SpriteRenderer() const { return _entity ? SpriteRenderer2D(_entity) : component_traits<SpriteRenderer2D>::get(_script); }

    template <class T>
    T GetComponent() const {
        return _entity ? T(_entity) : component_traits<T>::get(_script);
    }
    bool HasComponent(string name) const {
        auto* e = _ptr(); return e && has_component(*e, name);
    }
    void SetTag(string t) { auto* e = _ptr(); if (e) (*e)["tag"] = t; }

    void Destroy() {
        if (_entity) { _entity.Destroy(); return; }
        if (_script) _script->destroy();
    }
    void Destroy(float delay) {
        if (_entity) { _entity.DestroyWithDelay(delay); return; }
        if (_script) _script->destroy(_script->entity._ptr, delay);
    }

private:
    mutable ScriptBase* _script = nullptr;
    EntityRef _entity;
    mutable bool _detected = false;
    Entity* _ptr() const {
        if (_entity) return _entity._ptr;
        if (!_detected) {
            _detected = true;
            if (!_script) const_cast<GameObject*>(this)->_script = ScriptBase::current();
        }
        return _script ? _script->entity._ptr : nullptr;
    }
};

class MonoBehaviour : public ScriptBase {
public:
    virtual ~MonoBehaviour() = default;

    // Unity-style lifecycle. Old lowercase methods still exist in ScriptBase.
    virtual void Awake() {}
    virtual void Start() {}
    virtual void Update(float /*dt*/) {}
    // Override both methods for a pause menu/controller that must continue to
    // read input while the rest of the simulation is frozen.
    virtual bool UpdateWhilePaused() const { return false; }
    virtual void UpdateUnscaled(float /*rawDt*/) {}
    // Called every fixed physics timestep (1/120 s by default).
    // Write physics forces/velocities here instead of Update() so they stay
    // in sync with the physics integrator — mirrors Unity's FixedUpdate().
    virtual void FixedUpdate(float /*fixedDt*/) {}
    // Called after all Update()s each frame, before the next render.
    // Ideal for camera-follow logic that must see the final character position
    // — mirrors Unity's LateUpdate().
    virtual void LateUpdate(float /*dt*/) {}
    // Fired the moment entity["active"] transitions to true — whether that
    // happened via the editor Inspector, ObjectPoolSystem reusing this
    // instance, a ScriptGraph SetActive node, or your own gameObject().
    // SetActive(true) call elsewhere. Also fires once right after Awake()
    // if the entity starts out active (matching Unity, where OnEnable runs
    // before Start() for anything active at scene load). Use this instead
    // of polling entity active state in Update() — e.g. to resume a timer,
    // re-subscribe to an event, or restart an effect.
    virtual void OnEnable()  {}
    // Fired the moment entity["active"] transitions to false. The natural
    // place to cancel timers (CancelInvoke()), pause/stop audio, or reset
    // per-activation state, so it's ready clean the next time OnEnable()
    // fires (e.g. when ObjectPoolSystem returns this instance to the pool
    // and later reuses it for a different spawn).
    virtual void OnDisable() {}
    virtual void OnDestroy() {}
    virtual void OnCollisionEnter2D(EntityRef /*other*/) {}
    virtual void OnCollisionStay2D(EntityRef /*other*/) {}
    virtual void OnCollisionExit2D(EntityRef /*other*/) {}
    virtual void OnTriggerEnter2D(EntityRef /*other*/) {}
    virtual void OnTriggerStay2D(EntityRef /*other*/) {}
    virtual void OnTriggerExit2D(EntityRef /*other*/) {}
    // Fired by AnimatorSystem when it advances onto a frame that has a named
    // event attached via Animator::AddEvent. See ScriptBase::
    // on_animation_event for the one-frame delivery-lag note.
    virtual void OnAnimationEvent(string /*event_name*/) {}

    // ScriptBase bridge
    void awake() override { Awake(); }
    void start() override { Start(); }
    void update(float dt) override { Update(dt); }
    bool update_while_paused() const override { return UpdateWhilePaused(); }
    void update_unscaled(float raw_dt) override { UpdateUnscaled(raw_dt); }
    void fixed_update(float fixed_dt) override { FixedUpdate(fixed_dt); }
    void late_update(float dt) override { LateUpdate(dt); }
    void on_enable()  override { OnEnable(); }
    void on_disable() override { OnDisable(); }
    void on_destroy() override { OnDestroy(); }
    void on_collision_enter(EntityRef other) override { OnCollisionEnter2D(other); }
    void on_collision_stay(EntityRef other) override { OnCollisionStay2D(other); }
    void on_collision_exit(EntityRef other) override { OnCollisionExit2D(other); }
    void on_trigger_enter(EntityRef other) override { OnTriggerEnter2D(other); }
    void on_trigger_stay(EntityRef other) override { OnTriggerStay2D(other); }
    void on_trigger_exit(EntityRef other) override { OnTriggerExit2D(other); }
    void on_animation_event(string event_name) override { OnAnimationEvent(event_name); }
    // Bridges ScriptBase::on_draw_gizmos() (called every frame by
    // ScriptSystem::draw_gizmos in the editor) to the Unity-cased virtual.
    // Without this one-line override, a MonoBehaviour-derived class that
    // overrides OnDrawGizmos() was silently ignored — on_draw_gizmos()
    // never called it, so the gizmo just never drew.
    virtual void OnDrawGizmos() {}
    // Unity's "only while this object is selected in the Hierarchy" gizmo
    // variant. ScriptSystem::draw_gizmos() has no concept of editor
    // selection (that lives in EditorState, not in script_system.hpp), so
    // this fires every frame exactly like OnDrawGizmos() above — the
    // separate name still lets a script choose to put expensive/noisy debug
    // drawing here and opt into it being visually distinguished later if
    // selection-gating is added to draw_gizmos().
    virtual void OnDrawGizmosSelected() {}
    void on_draw_gizmos() override { OnDrawGizmos(); OnDrawGizmosSelected(); }

    // Unity-like helpers.
    GameObject gameObject() { return GameObject(entity); }
    Transform2D Transform() { return Transform2D(entity); }
    Rigidbody2D Rigidbody() { return Rigidbody2D(entity); }
    Animator GetAnimator() { return Animator(entity); }
    SpriteRenderer2D SpriteRenderer() { return SpriteRenderer2D(entity); }
    ComponentRef GetComponent(string name) { return ComponentRef(get_component(name)); }
    // Typed GetComponent<T>() — the #1 most common Unity call. Returns a real
    // wrapper (Rigidbody2D, SpriteRenderer2D, Animator, AudioSource2D,
    // Transform2D) instead of a raw ComponentRef, so you get the actual
    // typed API immediately:
    //   auto rb  = GetComponent<Rigidbody2D>();
    //   auto sr  = GetComponent<SpriteRenderer2D>();
    //   if (sr) sr.SetColor(255, 0, 0);
    // Every wrapper's operator bool() reports whether the underlying
    // component is actually attached, so the null-check idiom above always
    // works the same way regardless of which T you asked for. Pure
    // compile-time sugar — see component_traits<T> above; zero runtime cost
    // beyond what the equivalent hand-written GetComponent("...") call did.
    template <class T>
    T GetComponent() { return component_traits<T>::get(this); }
    // RequireComponent<T>() — Unity's [RequireComponent] attribute, but as a
    // checked getter instead of a class-level annotation (C++ has no
    // attribute system to hook the same way). Call it in Awake() for any
    // component your script can't function without:
    //   auto& rb = RequireComponent<Rigidbody2D>();   // (well, see below)
    // Unlike GetComponent<T>(), this asserts + logs loudly and returns a
    // wrapper that is still safe to call methods on (every wrapper method is
    // already null-guarded internally) rather than silently handing back an
    // empty wrapper that then fails mysteriously three call sites later.
    // Prefer this over GetComponent<T>() specifically for the dependencies
    // your script assumes exist at every call site — it turns a future
    // silent-null-deref bug into a clear, immediate, named error at startup.
    template <class T>
    T RequireComponent() {
        T comp = component_traits<T>::get(this);
        if (!comp) {
            Debug::log_error(string("RequireComponent<") + component_traits<T>::type_name() +
                              "> failed on \"" + name() +
                              "\" — required component is missing from this entity.");
        }
        return comp;
    }
    // Check whether a component is attached without fetching it.
    // Avoids null-checking a GetComponent() result for a simple guard:
    //   if (HasComponent("AudioSource")) PlaySound();
    bool HasComponent(string name) { return entity && has_component(*entity, name); }
    EntityRef Find(string name) { return find_entity(name); }
    EntityRef FindById(int id) { return find_entity_by_id(id); }
    EntityRef FindWithTag(string t) {
        auto list = find_entities_with_tag(t);
        return list.empty() ? nullptr : list.front();
    }
    vector<EntityRef> FindGameObjectsWithTag(string t) {
        auto raw = find_entities_with_tag(t);
        vector<EntityRef> out; out.reserve(raw.size());
        for (auto* e : raw) out.push_back(e);
        return out;
    }
    vector<EntityRef> FindObjectsOfType(string comp) {
        auto raw = find_entities_with(comp);
        vector<EntityRef> out; out.reserve(raw.size());
        for (auto* e : raw) out.push_back(e);
        return out;
    }

    // ── World-position shorthands (very commonly needed in Update) ───────────
    // Position() / SetPosition() — world-space, same as Transform().Position().
    // Saves allocating a Transform2D wrapper when you only need the position.
    Vector2 Position() { return {get_world_x(), get_world_y()}; }
    void SetPosition(float wx, float wy) { set_world_position(wx, wy); }
    void SetPosition(Vector2 p) { set_world_position(p.x, p.y); }
    // Shorthand to move by a delta each frame without constructing a Transform2D.
    void MoveBy(float dx, float dy) { move(dx, dy); }
    void MoveBy(Vector2 delta) { move(delta.x, delta.y); }

    // Clear shorthand wrappers.
    void Play(string clip) { play_animation(clip); }
    bool Grounded() { return is_grounded(); }
    Entity* Comp(string name) { return get_component(name); }

    EntityRef Instantiate(EntityRef tmpl, optional<float> x = {},
                          optional<float> y = {}) {
        return tmpl ? EntityRef(instantiate(*tmpl, x, y)) : EntityRef();
    }
    // Spawn at a Vector2 position — avoids awkward {.x, .y} unpacking.
    EntityRef Instantiate(EntityRef tmpl, Vector2 pos) {
        return tmpl ? EntityRef(instantiate(*tmpl, pos.x, pos.y)) : EntityRef();
    }
    void Destroy() { entity.Destroy(); }
    void Destroy(EntityRef target) { target.Destroy(); }
    void Destroy(EntityRef target, float delay) { target.DestroyWithDelay(delay); }

    // Input shortcuts (Unity-style)
    bool GetKey(KeyCode k) { return Input::GetKey(k); }
    bool GetKeyDown(KeyCode k) { return Input::GetKeyDown(k); }
    bool GetKeyUp(KeyCode k) { return Input::GetKeyUp(k); }
    bool GetButton(string n) { return Input::GetButton(n); }
    bool GetButtonDown(string n) { return Input::GetButtonDown(n); }
    bool GetButtonUp(string n) { return Input::GetButtonUp(n); }
    float GetAxis(string n) { return Input::GetAxis(n); }
    float GetAxisRaw(string n) { return Input::GetAxisRaw(n); }
    bool GetMouseButton(int b) { return Input::GetMouseButton(b); }
    bool GetMouseButtonDown(int b) { return Input::GetMouseButtonDown(b); }
    bool GetMouseButtonUp(int b) { return Input::GetMouseButtonUp(b); }
    Vector2 MousePosition() { return Input::MousePosition(); }

    // Networking helpers.
    bool IsHost() const { return Network::IsHost(); }
    bool IsClient() const { return Network::IsClient(); }
    uint32_t LocalPeerId() const { return Network::LocalPeerId(); }

    // Returns true if THIS entity is the local player's character.
    // Player scripts MUST guard all input (movement, shooting, etc.) with
    // this check — without it, every player prefab instance runs the same
    // input code and all players move/shoot together.
    //
    // Usage in your player script's Update():
    //   if (!IsLocalPlayer()) return;
    //   // ... read input and move/shoot here ...
    //
    // Internally reads the "net_is_local" field written by NetSpawn::SpawnAllPlayers()
    // onto the entity's root, which is set to true for exactly one spawned
    // player per machine — the one whose net_owner_peer_id matches LocalPeerId().
    bool IsLocalPlayer() const {
        if (!entity) return false;
        // Not in a networked match — treat as local (single-player / offline).
        if (!Network::IsHost() && !Network::IsClient()) return true;
        // If net_owner_peer_id hasn't been stamped yet (SpawnAllPlayers stamps
        // it right after instantiate() returns, so there is a one-frame window
        // before it exists), return true to avoid blocking the local player.
        // Once net_owner_peer_id is present, net_is_local is also reliably set.
        if (!entity->contains("net_owner_peer_id")) return true;
        return entity->value("net_is_local", false);
    }

    // Returns the peer id of the player that owns this entity (0 = unowned / host).
    uint32_t OwnerPeerId() const {
        if (!entity) return 0;
        return (uint32_t)entity->value("net_owner_peer_id", 0);
    }
    void SendEvent(string event_name, Entity data, bool reliable = true) {
        Network::SendEvent(event_name, data, reliable);
    }
    void SendEventTo(uint32_t peer_id, string event_name, Entity data, bool reliable = true) {
        Network::SendEventTo(peer_id, event_name, data, reliable);
    }
    void RefreshLobbyBrowser() { Matchmaking::RefreshBrowser(); }
    void BroadcastTransform() {
        if (entity) Network::BroadcastTransform(*entity);
    }

    // ── Networked combat / replication helpers ──────────────────────────────
    // Use these instead of take_damage()/destroy() for anything that must
    // behave correctly in multiplayer (enemies, players, breakables). See
    // net/replication.hpp and net/replication_rpc.hpp for the full design:
    // short version, only the HOST's call actually mutates HealthComponent
    // or despawns anything — every machine (including a client calling
    // these) just sends/receives the request and waits for the host's
    // broadcast result.

    // Returns this entity's network id (0 if it isn't a networked/replicated
    // entity — e.g. cosmetic particles, or single-player/offline play).
    uint32_t NetId() const { return entity ? Replication::NetIdOf(*entity) : 0; }
    bool IsNetworked() const { return entity && Replication::IsNetworked(*entity); }

    // Requests damage be applied to THIS entity's net-replicated health.
    // Equivalent to take_damage() but routed through the host so death is
    // decided once, consistently, for every player. No-op if this entity
    // has no NetId (falls back to nothing — use take_damage() directly for
    // purely local/offline entities).
    void RequestDamage(float amount, uint32_t attacker_peer_id = 0, float hit_x = 0.f, float hit_y = 0.f) {
        if (!entity) return;
        Replication::RequestDamage(Replication::NetIdOf(*entity), amount, attacker_peer_id, hit_x, hit_y);
    }
    // Same, but targeting another entity by NetId — e.g. a projectile or
    // melee hitbox script calling this on whatever it just collided with.
    void RequestDamageTo(uint32_t target_net_id, float amount, uint32_t attacker_peer_id = 0,
                         float hit_x = 0.f, float hit_y = 0.f) {
        Replication::RequestDamage(target_net_id, amount, attacker_peer_id, hit_x, hit_y);
    }
    // Requests that the local player pick up THIS entity (an item). The
    // host validates it's still there (handles two players grabbing the
    // same drop the same frame) and broadcasts net_item_collected + a
    // despawn if it succeeds; listen for "net_item_collected" via
    // EventBus to react (add to inventory, play a sound, etc.).
    void RequestPickup() {
        if (!entity) return;
        Replication::RequestPickup(Replication::NetIdOf(*entity), Network::LocalPeerId());
    }

    // Client-side reconciliation: call once per frame for the LOCAL
    // player's own entity, after your movement script has already moved
    // it this frame, to smoothly blend in any authoritative correction the
    // host has sent (see net/net_predict.hpp). No-op if nothing is pending.
    void ReconcileMovement(float dt) {
        if (entity) NetPredict::ReconcileLocalPlayer(*entity, dt);
    }
    // Periodic "here's where I think I am" report a client can send so the
    // host's own movement-validation script (if any) has something to
    // check against. Call this a few times a second, not every frame.
    void SendMoveState(float vx, float vy) {
        if (!entity || !has_component(*entity, "Transform")) return;
        auto& t = (*entity)["components"]["Transform"];
        NetPredict::SendMoveState(t.value("x", 0.f), t.value("y", 0.f), vx, vy);
    }

    bool InLobby() const { return Matchmaking::InLobby(); }
    bool InMatch() const { return Matchmaking::InMatch(); }
    bool IsLobbyHost() const { return Matchmaking::IsHosting(); }
    string LobbyName() const { return Matchmaking::_state().lobby_name; }
    string LobbyError() const { return Matchmaking::LastError(); }
    string LobbySummary() const { return Matchmaking::LobbySummary(); }
    void HostLobby(string lobby_name, string player_name, uint16_t port = 7777, int max_players = 8, string mode_name = "Casual", string map_name = string(), bool public_lobby = true, bool password_required = false, string password = string(), string region = "LAN", bool auto_start = false, bool lan_discovery = true, string build_version = "1.0") {
        Matchmaking::Host(lobby_name, player_name, port, max_players, mode_name, map_name, public_lobby, password_required, password, region, auto_start, lan_discovery, build_version);
    }
    void JoinLobby(string address, uint16_t port, string player_name = "Player") { Matchmaking::Join(address, port, player_name); }
    void LeaveLobby() { Matchmaking::Leave(); }
    void SetLobbyReady(bool value) { Matchmaking::SetReady(value); }
    void SendLobbyChat(string msg) { Matchmaking::SendChat(msg); }
    void StartLobbyMatch() { Matchmaking::StartMatch(); }
    void QuickMatch(string mode_name = string(), string map_name = string(), int party_size = 1) { Matchmaking::QuickMatch(mode_name, map_name, party_size); }
    void SetLobbyPlayerName(string name) { Matchmaking::SetPlayerName(name); }
    void SetLobbyMeta(string lobby_name, string mode_name, string map_name, string region, string build_version) {
        Matchmaking::SetLobbyName(lobby_name);
        Matchmaking::SetModeName(mode_name);
        Matchmaking::SetMapName(map_name);
        Matchmaking::SetRegion(region);
        Matchmaking::SetBuildVersion(build_version);
    }

    // Safe entity value read (Unity-cased).
    //   int hp = Get("hp", 3);
    template <class T>
    T Get(string key, T def) { return val(key, def); }
    bool Has(string key) { return has(key); }

    // Small quality-of-life aliases.
    float DeltaTime() const { return (float)Time::delta_time; }
    float TimeScale() const { return (float)Time::time_scale; }
    // Total elapsed game time in seconds (Time.time in Unity).
    float ElapsedTime() const { return (float)::Time::elapsed_time; }
    string Name() { return name(); }
    void Log(string msg) { log(msg); }
    void LogWarning(string msg) { Debug::log_warning(msg); }
    void LogError(string msg) { Debug::log_error(msg); }
    void DrawDebugLine(float x1, float y1, float x2, float y2, Uint8 r=255, Uint8 g=255, Uint8 b=0, float duration=0.f) {
        Debug::draw_line(x1, y1, x2, y2, r, g, b, 255, duration);
    }

    // ── Audio shortcuts (Unity-style names) ──────────────────────────────────
    void PlaySound(string clipPath)              { play_sound(clipPath); }
    void PlayOneShot(string clipPath, float vol=1.f) { play_one_shot(clipPath, vol); }
    void StopSound()                                         { stop_audio_source(); }
    bool IsAudioPlaying()                                    { return is_audio_playing(); }
    void SetVolume(float v)                                  { set_audio_volume(v); }
    void SetPitch(float p)                                   { set_audio_pitch(p); }

    // ── Health shortcuts (Unity-style names) ──────────────────────────────────
    void TakeDamage(float amount)    { take_damage(amount); }
    // Overload that also credits an attacker — see ScriptBase::take_damage's
    // two-argument overload. Pass the entity that caused the hit (a bullet,
    // an enemy's melee hitbox, etc.) so a registered OnDamageTaken handler
    // can use it for knockback direction or kill-credit.
    void TakeDamage(float amount, Entity* attacker) { take_damage(amount, attacker); }
    void Heal(float amount)          { heal(amount); }
    bool IsDead()                    { return is_dead(); }
    float CurrentHealth()            { return current_health(); }
    float MaxHealth()                { return max_health(); }
    float HealthRatio()              { return health_ratio(); }
    void SetInvincible(bool v)       { set_invincible(v); }
    // Register a handler fired exactly once when this entity's HealthComponent
    // reaches 0. See ScriptBase::OnDeath's comment for the full explanation
    // and an example. Typically called once, in Awake().
    void OnDeath(DeathHandler handler) { ScriptBase::OnDeath(std::move(handler)); }
    // Register a handler fired once per frame this entity's health drops,
    // with the amount lost and (if known) the attacking entity.
    void OnDamageTaken(DamageHandler handler) { ScriptBase::OnDamageTaken(std::move(handler)); }

    // ── Tween shortcuts ───────────────────────────────────────────────────────
    void TweenTo(float tx, float ty, float dur, string ease="ease_in_out") { tween_to(tx, ty, dur, ease); }
    void TweenScale(float s, float dur)  { tween_scale(s, dur); }
    void FadeOut(float dur = 0.5f)       { fade_out(dur); }
    void FadeIn(float dur = 0.5f)        { fade_in(dur); }

    // ── Physics shortcuts ─────────────────────────────────────────────────────
    bool IsGrounded()                    { return is_grounded(); }
    void SetVelocity(float vx, float vy) { set_velocity(vx, vy); }
    void AddForce(float fx, float fy)    { add_force(fx, fy); }
    void AddImpulse(float jx, float jy)  { add_impulse(jx, jy); }
    void StopMovement()                  { set_velocity(0.f, 0.f); }

    // ── Coroutines (Unity-style) ─────────────────────────────────────────────
    // See script_system.hpp's coro:: namespace + CO_WAIT_* macros for the
    // body syntax (CO_BODY_BEGIN/CO_WAIT_SECONDS/CO_WAIT_UNTIL/CO_WAIT_FRAME/
    // CO_BODY_END). Typical usage inside a MonoBehaviour:
    //
    //   coro::Handle _flash;
    //   void OnHit() {
    //       StopCoroutine(_flash); // cancel any flash already in progress
    //       _flash = StartCoroutine([this](coro::Coroutine& co) -> coro::CoroutineStep {
    //           CO_BODY_BEGIN(co);
    //           SpriteRenderer().SetColor(255,80,80,255);
    //           CO_WAIT_SECONDS(co, 0.1f);
    //           SpriteRenderer().SetColor(255,255,255,255);
    //           CO_BODY_END();
    //       });
    //   }
    coro::Handle StartCoroutine(coro::Body body) { return start_coroutine(std::move(body)); }
    void StopCoroutine(const coro::Handle& handle) { stop_coroutine(handle); }
    void StopAllCoroutines() { stop_all_coroutines(); }

    // ── Boilerplate-reducing helpers ────────────────────────────────────────────

    // Self-destruction with optional delay.
    void Destroy(float delay) { entity.DestroyWithDelay(delay); }
    // Clear delayed-destruction alias.
    void DestroyWithDelay(float delay) { entity.DestroyWithDelay(delay); }

    // Active state — delegates to entity (EntityRef).
    void SetActive(bool v) { entity.SetActive(v); }
    bool IsActive() { return entity.IsActive(); }
    void Show() { entity.Show(); }
    void Hide() { entity.Hide(); }

    // Tag helpers — delegates to entity (EntityRef).
    void SetTag(string t) { entity.SetTag(t); }
    bool IsTag(string t) { return entity.CompareTag(t); }
    string Tag() { return entity.Tag(); }

    // Flash — delegates to SpriteRenderer2D without wrapping manually.
    coro::Handle Flash(Uint8 r, Uint8 g, Uint8 b, float duration = 0.1f, Uint8 a = 255) {
        return SpriteRenderer2D(EntityRef(entity)).Flash(r, g, b, duration, a);
    }

    // Scene loading — no SceneManager:: prefix needed.
    void LoadScene(string path) { SceneManager::LoadScene(path); }

    // Random helpers — no Random:: prefix needed.
    float Rand() { return Random::Value(); }
    float RandRange(float lo, float hi) { return Random::Range(lo, hi); }
    int RandRange(int lo, int hi) { return Random::Range(lo, hi); }

    // ── World-position / velocity shorthands ────────────────────────────────
    float GetPositionX() { return get_world_x(); }
    float GetPositionY() { return get_world_y(); }
    float GetVelocityX() { return get_velocity_x(); }
    float GetVelocityY() { return get_velocity_y(); }

    // Math helpers — no std:: or Mathf:: prefix needed in scripts.
    static float Max(float a, float b) { return a > b ? a : b; }
    static float Min(float a, float b) { return a < b ? a : b; }
    static float Abs(float v) { return v < 0 ? -v : v; }
    static float Sqrt(float v) { return std::sqrt(v); }
    static float Sin(float v) { return std::sin(v); }
    static float Cos(float v) { return std::cos(v); }
    static float Clamp(float v, float lo, float hi) { return Mathf::clamp(v, lo, hi); }
    static float Clamp01(float v) { return Mathf::clamp01(v); }
    static float Lerp(float a, float b, float t) { return Mathf::lerp(a, b, t); }
    static float Sign(float v) { return Mathf::sign(v); }
    static float Dist(float ax, float ay, float bx, float by) { return Mathf::distance(ax, ay, bx, by); }
    static float MoveTowards(float cur, float target, float maxDelta) { return Mathf::move_towards(cur, target, maxDelta); }
    static int RoundToInt(float v) { return (int)std::round(v); }
    static string ToStr(int v) { return std::to_string(v); }
    static string ToStr(float v) { return std::to_string(v); }
    static string ToStr(string v) { return v; }

    // ── Cached-entity resolver ─────────────────────────────────────────────────
    // Collapses the common 5-line "FindById + fallback to Find + cache update"
    // pattern into one line. Call it every frame or once in Awake:
    //   EntityRef player = Resolve(player_id, "AbyssPlayer");
    EntityRef Resolve(int& cachedId, string fallbackName) {
        EntityRef e = find_entity_by_id(cachedId);
        if (!e) {
            e = find_entity(fallbackName);
            if (e) cachedId = e.Value("id", -1);
        }
        return e;
    }

    // ── Other-entity transform helpers ─────────────────────────────────────────
    // Replaces the copy-pasted static GetX()/GetY() helpers in user scripts.
    static float EntityX(EntityRef e, float def = 0) {
        if (!e || !e.Contains("components") || !e["components"].contains("Transform")) return def;
        return e["components"]["Transform"].value("x", def);
    }
    static float EntityY(EntityRef e, float def = 0) {
        if (!e || !e.Contains("components") || !e["components"].contains("Transform")) return def;
        return e["components"]["Transform"].value("y", def);
    }

    // ── Read a component field on any entity ───────────────────────────────────
    //   bool grounded = ComponentValue(entity, "Rigidbody2D", "_grounded", false);
    template <class T>
    T ComponentValue(EntityRef e, string comp, string field, T def) {
        if (!e) return def;
        return e.ComponentValue<T>(comp, field, def);
    }

    // ── Scale ───────────────────────────────────────────────────────────────────
    void SetScale(float sx, float sy) { set_scale(sx, sy); }
    float ScaleX() { return get_scale_x(); }
    float ScaleY() { return get_scale_y(); }

    // ── Component field write ───────────────────────────────────────────────────
    // Writes a field on one of this entity's components.
    //   SetComponent("Rigidbody2D", "gravity_scale", 0.0f);
    template <class T>
    void SetComponent(string comp, string field, T val) {
        entity.SetComponent(comp, field, val);
    }
    // Static overload for writing a component field on any entity.
    //   SetComponent(other, "SpriteRenderer", "flip_x", true);
    template <class T>
    static void SetComponent(EntityRef e, string comp, string field, T val) {
        if (e) e.SetComponent(comp, field, val);
    }

    // EventBus shortcuts — no manual subscribe/emit/entity check.
    void On(string eventName, function<void(EntityRef)> handler) {
        EventBus::instance().subscribe(eventName, [this, handler](EntityRef data, EntityRef target) {
            if (target && target != this->entity) return;
            handler(data);
        });
    }
    // Overload with EntityRef target.
    void On(string eventName, function<void(EntityRef, EntityRef)> handler) {
        EventBus::instance().subscribe(eventName, [this, handler](EntityRef data, EntityRef target) {
            if (target && target != this->entity) return;
            handler(data, target);
        });
    }
    void Emit(string eventName, EntityRef data) {
        EventBus::instance().emit(eventName, *data, data._ptr);
    }
};

using Behaviour2D = MonoBehaviour;
using Behaviour = MonoBehaviour;

// ── Scripting convenience: bring additional std names into scope ───
using std::unordered_map;
using std::abs;
using std::max;
using std::round;
using std::chrono::steady_clock;

using std::atan2;
using std::cos;
using std::hypot;
using std::floor;
using std::remove;
using std::mt19937;
using std::min;
using std::sin;
using std::sqrt;
using std::chrono::duration;
using std::uniform_int_distribution;
using std::uniform_real_distribution;

// Standalone math helpers (mirror MonoBehaviour shorthands for static code).
inline float Round(float v) { return round(v); }
inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int    Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
