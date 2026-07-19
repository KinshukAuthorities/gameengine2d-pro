#pragma once
/*
 * script_system.hpp — C++ native scripting system
 *
 * Replaces Python ScriptBase entirely. Game logic is written as C++ classes
 * that inherit from ScriptBase and are registered with REGISTER_SCRIPT().
 *
 * Lifecycle mirrors Unity:
 *   awake()   → called once when entity is first processed
 *   start()   → called once before first update()
 *   update(dt)→ called every frame
 *   on_collision_enter/stay/exit(other)
 *   on_trigger_enter/stay/exit(other)
 *   on_destroy()
 */

#include "entity.hpp"
#include "input_system.hpp"
#include "physics.hpp"
#include "time.hpp"
#include "feature_systems.hpp" // EventBus — used by ScriptBase's destructor (unsubscribe_all)
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <iostream>
#include <limits>
#include <filesystem>
#include <nlohmann/json.hpp>

// ─── Forward decls ────────────────────────────────────────────────────────────
class ScriptSystem;

// ─── Mathf helpers ────────────────────────────────────────────────────────────
namespace Mathf {
    inline float clamp(float v, float lo, float hi) { return v<lo?lo:v>hi?hi:v; }
    inline float clamp01(float v) { return clamp(v,0.f,1.f); }
    inline float lerp(float a, float b, float t) { return a+(b-a)*clamp01(t); }
    inline float sign(float v) { return v>0?1.f:v<0?-1.f:0.f; }
    inline float abs(float v) { return std::abs(v); }
    inline float sqrt(float v) { return std::sqrt(v); }
    inline float distance(float ax,float ay,float bx,float by){return std::hypot(bx-ax,by-ay);}
    static constexpr float PI = 3.14159265358979323846f;
    static constexpr float DEG2RAD = PI/180.f;
    static constexpr float RAD2DEG = 180.f/PI;
    inline float deg2rad(float d){return d*DEG2RAD;}
    inline float rad2deg(float r){return r*RAD2DEG;}
    inline float sin(float v){return std::sin(v);}
    inline float cos(float v){return std::cos(v);}
    inline float atan2(float y, float x){return std::atan2(y,x);}
    inline float pow(float base, float exp){return std::pow(base,exp);}
    inline float floor(float v){return std::floor(v);}
    inline float ceil(float v){return std::ceil(v);}
    inline float round(float v){return std::round(v);}
    inline float min(float a,float b){return a<b?a:b;}
    inline float max(float a,float b){return a>b?a:b;}
    inline float smooth_step(float a,float b,float t){
        t=clamp01((t-a)/(b-a));
        return t*t*(3-2*t);
    }
    inline float move_towards(float cur,float target,float max_delta){
        float d=target-cur;
        if (std::abs(d)<=max_delta) return target;
        return cur + sign(d)*max_delta;
    }
    // Loops t so it never exceeds length. Mirrors Unity's Mathf.Repeat.
    // repeat(2.5f, 2.0f) → 0.5f
    inline float repeat(float t, float length) {
        if (length <= 0.f) return 0.f;
        return t - std::floor(t / length) * length;
    }
    // Bounces t back and forth between 0 and length. Mirrors Unity's Mathf.PingPong.
    // ping_pong(2.5f, 2.0f) → 1.5f
    inline float ping_pong(float t, float length) {
        if (length <= 0.f) return 0.f;
        float r = repeat(t, length * 2.f);
        return length - std::abs(r - length);
    }
    // Inverse of lerp: given a value v in [a,b], returns the t that produces it.
    // Clamps to [0,1]. inverse_lerp(0,10,7) → 0.7
    inline float inverse_lerp(float a, float b, float v) {
        return (b - a) != 0.f ? clamp01((v - a) / (b - a)) : 0.f;
    }
    // Shortest signed difference between two angles (degrees). Result is in (-180,180].
    // Use for smooth rotation: rotate towards target by DeltaAngle.
    //   set_rotation(get_rotation() + sign(Mathf::delta_angle(cur, target)) * speed * dt)
    inline float delta_angle(float current, float target) {
        float d = std::fmod(target - current, 360.f);
        if (d > 180.f) d -= 360.f;
        if (d < -180.f) d += 360.f;
        return d;
    }
    // Lerp between two angles (degrees) the SHORT way around, wrapping at
    // 360 — plain lerp() on raw angles breaks near the 0/360 seam (e.g.
    // lerp(350, 10, 0.5) would swing all the way through 180 instead of
    // through 0). Use for smoothly rotating towards a target heading.
    inline float lerp_angle(float current, float target, float t) {
        return current + delta_angle(current, target) * clamp01(t);
    }
    // True if |a - b| < epsilon. Avoids floating-point equality pitfalls.
    inline bool approximately(float a, float b, float eps = 1e-5f) { return std::abs(a - b) < eps; }
}


// ─── Script name helpers ──────────────────────────────────────────────────────
namespace detail {
    inline std::string to_lower_copy(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return s;
    }

    inline std::string to_pascal_case(const std::string& raw) {
        std::string out;
        bool cap = true;
        for (unsigned char c : raw) {
            if (std::isalnum(c)) {
                out.push_back(cap ? (char)std::toupper(c) : (char)c);
                cap = false;
            } else {
                cap = true;
            }
        }
        return out;
    }

    inline std::vector<std::string> normalize_script_names(const std::string& raw) {
        std::vector<std::string> out;
        if (raw.empty()) return out;
        out.push_back(raw);
        out.push_back(to_pascal_case(raw));
        auto lower = to_lower_copy(raw);
        if (lower != raw) out.push_back(lower);
        auto pascal_case = to_pascal_case(raw);
        auto pascal_lower = to_lower_copy(pascal_case);
        if (pascal_lower != lower) out.push_back(pascal_lower);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    inline std::string make_script_key(const char* project, const std::string& name) {
        if (project && *project) return std::string(project) + "::" + name;
        return name;
    }

    inline std::string infer_project_from_scene_path(const std::string& scene_path) {
        namespace fs = std::filesystem;
        if (scene_path.empty()) return {};
        std::string gen = fs::path(scene_path).generic_string();
        const std::string marker = "games/";
        auto pos = gen.find(marker);
        if (pos == std::string::npos) return {};
        std::string rest = gen.substr(pos + marker.size());
        auto slash = rest.find('/');
        if (slash == std::string::npos) return {};
        return rest.substr(0, slash);
    }
}


// ─── Debug ────────────────────────────────────────────────────────────────────
// Matches Python Debug class: draw_line/draw_circle leave fading lines that
// the renderer picks up and draws on top of the world each frame.
struct DebugLine { float x1,y1,x2,y2; Uint8 r,g,b,a; float duration; };

class Debug {
public:
    // A debug primitive is intentionally cheap, but it is still user/script
    // supplied data. Keep a finite ceiling so an accidentally per-frame
    // persistent draw call cannot turn a long play session into an OOM crash.
    static constexpr size_t kMaxLines = 16384;

    // Optional editor console sink. When the editor calls
    // Debug::set_log_callback(...), all Debug::log/log_warning/log_error
    // output is routed through it (in addition to stdout/stderr) so messages
    // from NetSpawn, network code, and scripts appear in the in-editor
    // Console panel, not just the terminal. The standalone runtime never
    // calls this, so it falls through to stdout/stderr only — no change.
    using LogCallback = std::function<void(const std::string& msg, int level)>; // level: 0=log,1=warn,2=error
    // Header-local statics would make every hot-reload DLL keep private debug
    // lines and a private console callback. Bind script modules to the host's
    // state so Debug.DrawLine and Debug.Log are visible in editor.exe.
    struct State {
        std::vector<DebugLine> lines;
        LogCallback log_callback;
    };
    static State& _local_state() { static State state; return state; }
    static State*& _state_ptr() { static State* state = nullptr; return state; }
    static State& _state() { return _state_ptr() ? *_state_ptr() : _local_state(); }
    static void bind_state(State* host) { _state_ptr() = host; }
    static std::vector<DebugLine>& lines() { return _state().lines; }
    static LogCallback& _log_callback() { return _state().log_callback; }
    static void set_log_callback(LogCallback cb) { _log_callback() = std::move(cb); }

    static void log(const std::string& msg) {
        std::cout << "[Debug] " << msg << "\n";
        if (auto& cb = _log_callback()) cb(msg, 0);
    }
    static void log_warning(const std::string& msg) {
        std::cout << "[Warning] " << msg << "\n";
        if (auto& cb = _log_callback()) cb(msg, 1);
    }
    static void log_error(const std::string& msg) {
        std::cerr << "[Error] " << msg << "\n";
        if (auto& cb = _log_callback()) cb(msg, 2);
    }

    static void draw_line(float x1,float y1,float x2,float y2,
                           Uint8 r=255,Uint8 g=255,Uint8 b=0,Uint8 a=255,
                           float duration=0.f) {
        auto& l = lines();
        if (l.size() >= kMaxLines) {
            constexpr size_t kTrim = kMaxLines / 4;
            l.erase(l.begin(), l.begin() + std::min(kTrim, l.size()));
        }
        l.push_back({x1,y1,x2,y2,r,g,b,a,duration});
    }

    static void draw_circle(float x,float y,float radius,
                             Uint8 r=255,Uint8 g=255,Uint8 b=0,Uint8 a=255,
                             float duration=0.f) {
        constexpr int segs = 16;
        for (int i=0;i<segs;++i) {
            float a1=(float)i/segs*2.f*Mathf::PI, a2=(float)(i+1)/segs*2.f*Mathf::PI;
            draw_line(x+std::cos(a1)*radius, y+std::sin(a1)*radius,
                      x+std::cos(a2)*radius, y+std::sin(a2)*radius, r,g,b,a, duration);
        }
    }

    // Called once per frame (by core.cpp) to age out expired lines.
    static void update(float dt) {
        auto& l = lines();
        l.erase(std::remove_if(l.begin(), l.end(), [&](DebugLine& ln){
            ln.duration -= dt;
            return ln.duration < 0.f;
        }), l.end());
    }

    static void clear_lines() { lines().clear(); }
};

// ─── PlayerPrefs ──────────────────────────────────────────────────────────────
// Simple persistent key-value store, saved to player_prefs.json next to the exe.
// Matches Python PlayerPrefs (used for high scores, settings, save data).
class PlayerPrefs {
public:
    static nlohmann::json& _data() {
        static nlohmann::json d = _load();
        return d;
    }

    static void set_int(const std::string& k, int v)         { _data()[k]=v; _save(); }
    static int  get_int(const std::string& k, int def=0)      { return _data().value(k, def); }
    static void set_float(const std::string& k, float v)      { _data()[k]=v; _save(); }
    static float get_float(const std::string& k, float def=0.f){ return _data().value(k, def); }
    static void set_string(const std::string& k, const std::string& v) { _data()[k]=v; _save(); }
    static std::string get_string(const std::string& k, const std::string& def="") { return _data().value(k, def); }
    static bool has_key(const std::string& k) { return _data().contains(k); }
    static void delete_key(const std::string& k) { _data().erase(k); _save(); }
    static void delete_all() { _data().clear(); _save(); }

private:
    static constexpr const char* PATH = "player_prefs.json";

    static nlohmann::json _load() {
        try {
            std::ifstream f(PATH);
            if (!f) return nlohmann::json::object();
            nlohmann::json j; f >> j;
            // A missing/empty/corrupted file can parse to `null` (or any
            // non-object JSON value). PlayerPrefs must always be an object,
            // otherwise every get_int/get_float/get_string call below throws
            // "[json.exception.type_error.306] cannot use value() with null".
            if (!j.is_object()) return nlohmann::json::object();
            return j;
        } catch(...) { return nlohmann::json::object(); }
    }
    static void _save() {
        try {
            std::ofstream f(PATH);
            f << _data().dump();
        } catch(...) {}
    }
};

// ─── Exposed fields (Inspector reflection) ────────────────────────────────────
// Scripts can expose member variables to the Inspector with EXPOSE_FIELD,
// the way Unity exposes public fields on a MonoBehaviour. This is opt-in
// (C++ has no runtime reflection) but needs only one line per field:
//
//   class PlayerController : public MonoBehaviour {
//   public:
//       float move_speed = 520.0f;
//       int   max_jumps  = 2;
//       void Awake() override {
//           EXPOSE_FIELD(move_speed);
//           EXPOSE_FIELD(max_jumps);
//       }
//       ...
//   };
//
// Call EXPOSE_FIELD(...) once per field, anywhere before the field is first
// used (Awake() is the natural place). Supported types: float, int, bool,
// std::string. The Inspector reads/writes these through the registered
// getter/setter, so dragging a value in the Script panel edits the live
// member directly — and the chosen value is also saved into the scene file
// (Script component → "field_overrides") and re-applied automatically the
// next time the script is instantiated (in the editor or at runtime).
namespace scriptfields {

enum class FieldType { Float, Int, Bool, String };

struct FieldHandle {
    std::string         name;
    FieldType           type;
    std::function<Entity()>            get;
    std::function<void(const Entity&)> set;
};

// Per-instance registry: one field list per live ScriptBase*. Populated by
// EXPOSE_FIELD calls (typically from inside awake()/Awake()) and consumed by
// the Inspector and by ScriptSystem (to re-apply saved overrides).
//
// IMPORTANT cross-module note: script_system.hpp is header-only and gets
// compiled separately into BOTH the host editor.exe AND every per-project
// game_scripts_<project> DLL. A naive function-local static
// (`static InstanceRegistry r; return r;`) would give each binary its OWN
// independent instance — EXPOSE_FIELD calls made from script code running
// inside a DLL (e.g. inside awake()) would populate a registry that only
// exists in that DLL's memory, while the Inspector and
// ScriptSystem::apply_field_overrides (both compiled into and running from
// the HOST) would always read from a separate, permanently-empty registry.
// That exact bug is why exposed fields silently never showed up in the
// Inspector and saved field_overrides were silently never re-applied: both
// reads were hitting the wrong copy.
//
// Fixed the same way ScriptRegistry is: the HOST owns the one real
// instance, and every DLL is told where it is (set_host_instance, called
// from RegisterAllScripts right after LoadLibrary/dlopen, same moment
// ScriptRegistry gets handed across) instead of each side resolving its
// own local singleton. If no host pointer has been set (e.g. the
// standalone export build, where everything is linked into one binary and
// there's no DLL boundary to cross at all), instance() falls back to the
// plain function-local static exactly as before — so this is a no-op
// change for any context that isn't crossing a DLL boundary.
class InstanceRegistry {
public:
    static InstanceRegistry& instance() {
        if (_host_instance) return *_host_instance;
        static InstanceRegistry r;
        return r;
    }

    // Called by a DLL right after load, telling THIS module's copy of
    // InstanceRegistry where the host's real instance lives, so this
    // module's EXPOSE_FIELD calls (and any ~ScriptBase() cleanup) land in
    // the same place the host's Inspector/ScriptSystem read from.
    static void set_host_instance(InstanceRegistry* host) {
        _host_instance = host;
    }

    void clear(const void* owner) { _fields.erase(owner); }

    void add(const void* owner, FieldHandle f) {
        auto& vec = _fields[owner];
        for (auto& existing : vec) if (existing.name == f.name) { existing = std::move(f); return; }
        vec.push_back(std::move(f));
    }

    const std::vector<FieldHandle>* fields_for(const void* owner) const {
        auto it = _fields.find(owner);
        return it == _fields.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<const void*, std::vector<FieldHandle>> _fields;
    // NOT a function-local static — this is a plain static DATA member, so
    // each binary (host exe, each project's DLL) gets its own copy of the
    // POINTER itself, which is exactly what we want: the host's pointer
    // stays null and instance() falls through to its own local registry
    // (becoming the one true instance), while each DLL's copy of this
    // pointer gets explicitly set to point AT the host's instance via
    // set_host_instance(), so the DLL's instance() calls redirect there
    // instead of creating their own.
    static inline InstanceRegistry* _host_instance = nullptr;
};

inline FieldType type_of(const float&)       { return FieldType::Float; }
inline FieldType type_of(const int&)         { return FieldType::Int; }
inline FieldType type_of(const bool&)        { return FieldType::Bool; }
inline FieldType type_of(const std::string&) { return FieldType::String; }

} // namespace scriptfields

// Registers `member` (a field on `this`) under `field_name` so the Inspector
// can see and edit it. Safe to call every awake() — re-registering just
// refreshes the getter/setter for the (possibly new) instance.
// Also automatically reads the matching key from `entity` if it exists,
// eliminating the `hp = entity ? entity->value("hp", 3) : 3` pattern in Awake().
#define EXPOSE_FIELD(member) \
    do { \
        ::scriptfields::InstanceRegistry::instance().add(this, ::scriptfields::FieldHandle{ \
            #member, \
            ::scriptfields::type_of(member), \
            [this]() -> Entity { return Entity(member); }, \
            [this](const Entity& v) { (member) = v.get<std::decay_t<decltype(member)>>(); } \
        }); \
        if (entity && entity->contains(#member)) { \
            (member) = entity->value(#member, member); \
        } \
    } while (0)

// ── Variadic EXPOSE_FIELDS — expose many fields in one line ────────────────────
//   EXPOSE_FIELDS(hp, speed, damage);   // instead of 3 separate EXPOSE_FIELD lines
#define EXPOSE_FIELDS_1(m) EXPOSE_FIELD(m)
#define EXPOSE_FIELDS_2(m, ...) EXPOSE_FIELD(m); EXPOSE_FIELDS_1(__VA_ARGS__)
#define EXPOSE_FIELDS_3(m, ...) EXPOSE_FIELD(m); EXPOSE_FIELDS_2(__VA_ARGS__)
#define EXPOSE_FIELDS_4(m, ...) EXPOSE_FIELD(m); EXPOSE_FIELDS_3(__VA_ARGS__)
#define EXPOSE_FIELDS_5(m, ...) EXPOSE_FIELD(m); EXPOSE_FIELDS_4(__VA_ARGS__)
#define EXPOSE_FIELDS_6(m, ...) EXPOSE_FIELD(m); EXPOSE_FIELDS_5(__VA_ARGS__)
#define EXPOSE_FIELDS_7(m, ...) EXPOSE_FIELD(m); EXPOSE_FIELDS_6(__VA_ARGS__)
#define EXPOSE_FIELDS_8(m, ...) EXPOSE_FIELD(m); EXPOSE_FIELDS_7(__VA_ARGS__)
#define EXPOSE_FIELDS_9(m, ...) EXPOSE_FIELD(m); EXPOSE_FIELDS_8(__VA_ARGS__)
#define EXPOSE_FIELDS_10(m, ...) EXPOSE_FIELD(m); EXPOSE_FIELDS_9(__VA_ARGS__)
#define EXPOSE_FIELDS_CHOOSER(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,N,...) EXPOSE_FIELDS_##N
#define EXPOSE_FIELDS(...) EXPOSE_FIELDS_CHOOSER(__VA_ARGS__,10,9,8,7,6,5,4,3,2,1)(__VA_ARGS__)

// ─── SceneManager ─────────────────────────────────────────────────────────────
// Unity-style scene switching hook. Scripts call SceneManager::LoadScene(...)
// and the engine decides when/how to swap the active scene file.
//
// Fixed the same way ScriptRegistry/InstanceRegistry/Screen/Input are: game
// scripts live in a per-project DLL (see editor/scripts_module/CMakeLists.txt),
// and a DLL does NOT share the host exe's function-local statics — each
// binary that includes this header gets its OWN separate copy of `handler()`.
// The editor calls SetLoadSceneHandler(...) from panels.hpp, which sets the
// HOST's copy; a script's LoadScene(...) call runs inside the project's DLL
// and was checking the DLL's own, always-empty copy, so it always fell
// through to the warning below no matter when the handler was installed.
// Like Screen/Input, the real handler lives in a State struct, the host's
// instance is the one true copy, and every loaded DLL is handed a pointer
// to it (bind_state, called from RegisterAllScripts) so the DLL's
// LoadScene() redirects into the host's storage instead of its own.
namespace SceneManager {
    using LoadSceneHandler = std::function<void(const std::string&)>;

    struct State { LoadSceneHandler handler; };

    inline State& _local_state() { static State s; return s; }
    inline State*& _state_ptr() { static State* p = nullptr; return p; }

    // Called by a DLL right after load (see RegisterAllScripts) so this
    // module's SceneManager:: calls redirect to the host's real state. The
    // host itself never calls this — its own _state_ptr() stays null, so
    // _state() falls through to _local_state(), which becomes the one true
    // copy every DLL gets pointed at.
    inline void bind_state(State* host) { _state_ptr() = host; }
    inline State& _state() { return _state_ptr() ? *_state_ptr() : _local_state(); }

    inline void SetLoadSceneHandler(LoadSceneHandler h) {
        _state().handler = std::move(h);
    }

    inline void ClearLoadSceneHandler() {
        _state().handler = nullptr;
    }

    inline bool HasLoadSceneHandler() {
        return static_cast<bool>(_state().handler);
    }

    inline void LoadScene(const std::string& scene_path) {
        auto& h = _state().handler;
        if (h) {
            h(scene_path);
            return;
        }
        Debug::log_warning(std::string("SceneManager::LoadScene called before engine handler was installed: ") + scene_path);
    }
}

// ─── Coroutines (Unity-style StartCoroutine / yield return) ──────────────────
// C++17 has no language coroutines, so this is the standard "step function"
// approach Unity-style C++ engines use instead: a coroutine body is written
// as a sequence of CO_WAIT_* macros inside one lambda, and the driver below
// advances it one logical "yield" per call. Internally it's a switch-on-
// resume-point generator (the same trick Duff's-device-style C generators
// have used for decades), wrapped so script authors never see the machinery.
//
// Usage in a script (see unity2d_script_api.hpp's MonoBehaviour for the
// Unity-cased StartCoroutine/StopCoroutine/StopAllCoroutines wrappers):
//
//   auto h = start_coroutine([this](coro::Coroutine& co) -> coro::CoroutineStep {
//       CO_BODY_BEGIN(co);
//       CO_WAIT_SECONDS(co, 1.0f);
//       log("one second later");
//       CO_WAIT_UNTIL(co, [this]{ return is_grounded(); });
//       log("now grounded");
//       CO_WAIT_FRAME(co);
//       log("one frame later");
//       CO_BODY_END();
//   });
//   stop_coroutine(h);
//
// Each coroutine is driven from ScriptSystem::update(), right after the
// owning script's update(dt) call, so it shares the exact same dt/pause/
// time-scale semantics as everything else here — pausing play mode pauses
// every running coroutine for free, and a coroutine started in awake()/
// start() doesn't tick until that frame's update() has already run.
namespace coro {

enum class WaitKind { None, Frame, Seconds, Until, EndOfFrame };

// Mutable state threaded through one coroutine body across resumes. The body
// lambda receives this by reference; CO_WAIT_* macros write into it and
// `return` out of the lambda, exactly like a Unity `yield return ...;`
// followed by the implicit resume point C# iterator blocks get for free.
// Resuming back into the same point is done with a resume-counter + switch.
struct Coroutine {
    int      resume_point = 0;   // which CO_WAIT_* site to jump back into
    WaitKind wait_kind     = WaitKind::None;
    float    wait_timer    = 0.f;
    std::function<bool()> wait_until;
};

// Result of one resume of a coroutine body: either it's done (ran off the
// end, like a C# IEnumerator with no more MoveNext()), or it yielded and
// wants to be resumed again once the condition in `co.wait_kind` clears.
enum class CoroutineStep { Done, Yielded };

using Body = std::function<CoroutineStep(Coroutine&)>;

// One running coroutine instance: the body closure plus its own resumable
// state. ScriptBase owns a vector of these per script instance.
struct Instance {
    Coroutine co;
    Body      body;
    bool      stopped = false; // set by stop_coroutine(); reaped next tick
};

using Handle = std::shared_ptr<Instance>;

} // namespace coro

// CO_WAIT_* macros: each expands to a unique resume label (via __LINE__,
// unique per call site — exactly one CO_WAIT_* per source line is the one
// restriction this places on coroutine bodies, same restriction Duff's-
// device-based generators in C carry). The yield itself (set wait state +
// return) always runs straight through on the way down; the `case __LINE__`
// label is placed AFTER that return — not on it — so jumping back in on a
// later resume lands on the statement immediately following the wait, not
// on the wait's own setup code (which would just re-arm the same wait and
// immediately yield again, forever).
#define CO_WAIT_FRAME(co)                                                    \
    do {                                                                     \
        (co).wait_kind    = coro::WaitKind::Frame;                          \
        (co).resume_point = __LINE__;                                       \
        return coro::CoroutineStep::Yielded;                                \
    } while (0);                                                             \
    case __LINE__: ;

// Suspends until AFTER this frame's RenderSystem::draw() has returned —
// i.e. the world has been fully rasterized but the frame hasn't been
// presented/swapped yet. Needed for screenshot capture, render-texture
// reads, or any post-process effect that needs the completed frame's
// pixels rather than just "next tick" (CO_WAIT_FRAME resumes on the next
// ScriptSystem::update, which runs BEFORE that frame's render pass).
// Fired by ScriptSystem::resume_end_of_frame_waiters(), called once per
// frame right after render.draw() returns — see engine/core.cpp's
// run_game() and editor/src/panels.hpp's ViewportPanel for the two call
// sites (standalone build and in-editor Play mode respectively).
#define CO_WAIT_END_OF_FRAME(co)                                            \
    do {                                                                     \
        (co).wait_kind    = coro::WaitKind::EndOfFrame;                     \
        (co).resume_point = __LINE__;                                       \
        return coro::CoroutineStep::Yielded;                                \
    } while (0);                                                             \
    case __LINE__: ;

#define CO_WAIT_SECONDS(co, seconds)                                         \
    do {                                                                     \
        (co).wait_kind    = coro::WaitKind::Seconds;                        \
        (co).wait_timer   = (seconds);                                      \
        (co).resume_point = __LINE__;                                       \
        return coro::CoroutineStep::Yielded;                                \
    } while (0);                                                             \
    case __LINE__: ;

#define CO_WAIT_UNTIL(co, predicate)                                         \
    do {                                                                     \
        (co).wait_kind    = coro::WaitKind::Until;                          \
        (co).wait_until   = (predicate);                                    \
        (co).resume_point = __LINE__;                                       \
        return coro::CoroutineStep::Yielded;                                \
    } while (0);                                                             \
    case __LINE__: ;

// CO_BODY_BEGIN/END wrap the switch that makes resuming-into-the-middle
// actually work. Every coroutine lambda must start with CO_BODY_BEGIN(co)
// and end with CO_BODY_END().
#define CO_BODY_BEGIN(co) switch ((co).resume_point) { case 0: ;
#define CO_BODY_END() } return coro::CoroutineStep::Done

// CO_WAIT_ANIM_DONE — suspends the coroutine until the named animation clip
// on this entity has finished playing (non-looping clips only; a looping
// clip will never satisfy the condition, so pair with a timeout if needed).
//   CO_WAIT_ANIM_DONE(co, "Attack");  // wait until attack anim ends
#define CO_WAIT_ANIM_DONE(co, clip_name) \
    CO_WAIT_UNTIL(co, [this]{ \
        auto* a = get_component("Animator"); \
        if (!a) return true; \
        std::string cur = a->value("current_animation", std::string()); \
        return cur != (clip_name) || !a->value("playing", false); \
    })

// CO_WAIT_DEAD — suspends until this entity's health reaches 0.
// Useful for death sequences: CO_WAIT_DEAD(co); play_death_vfx(); destroy();
#define CO_WAIT_DEAD(co) CO_WAIT_UNTIL(co, [this]{ return is_dead(); })

// EntityRef is now provided by feature_systems.hpp (included above).

// ─── ScriptBase ───────────────────────────────────────────────────────────────
class ScriptBase {
public:
    // Set by ScriptSystem before any lifecycle call
    EntityRef    entity;
    EntityList*  all_entities = nullptr;
    InputSystem* input      = nullptr;
    bool         _started   = false;
    bool         _awoke     = false;
    bool         _destroyed = false;

    // Safe reference to the entity list — no * needed for range-for or Spawn.
    EntityList& entities() { return *all_entities; }
    const EntityList& entities() const { return *all_entities; }

    // Thread-local current script context — set during ScriptBase construction
    // and update dispatch. Used by lazy wrapper detect() to auto-link.
    static ScriptBase*& current() {
        static thread_local ScriptBase* ctx = nullptr;
        return ctx;
    }

    // Keep the script callback context and EventBus subscription owner in
    // lockstep. EventBus uses this while Awake/Start/Update run to attach a
    // raw subscribe("event", [this]{...}) call to the correct instance.
    static void set_current(ScriptBase* ctx) {
        current() = ctx;
        EventBus::set_subscription_owner(ctx);
    }

    virtual ~ScriptBase() {
        // The destructor runs while the producing script DLL is still loaded
        // (ScriptSystem destroys instances before FreeLibrary). Removing its
        // owner-tagged callbacks here prevents a later EventBus emit from
        // calling a lambda whose captured `this` has already been freed.
        EventBus::instance().unsubscribe_all(this);
        scriptfields::InstanceRegistry::instance().clear(this);
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    virtual void awake()  {}
    virtual void start()  {}
    virtual void update(float /*dt*/) {}
    // Opt-in escape hatch for UI/pause controllers.  Normal gameplay is
    // frozen when Time::time_scale is zero; an explicitly pause-safe script
    // may keep receiving raw wall-clock time here to draw/operate its menu.
    // Keeping this separate from update() prevents accidental simulation
    // during a pause and makes the contract visible at every call site.
    virtual bool update_while_paused() const { return false; }
    virtual void update_unscaled(float /*raw_dt*/) {}
    // Called every fixed physics timestep (matches PHYSICS_STEP = 1/120s).
    // Use for physics forces/velocities instead of update() to stay in sync
    // with the physics integrator — mirrors Unity's FixedUpdate().
    virtual void fixed_update(float /*fixed_dt*/) {}
    // Called after all update()s, then after render interpolation is computed
    // — mirrors Unity's LateUpdate(). Ideal for camera follow logic that must
    // see the final position of a character after their update() has run.
    virtual void late_update(float /*dt*/) {}
    // Called for editor/debug drawing — mirrors Unity's OnDrawGizmos().
    // Push gizmo draw commands to entity["_gizmos"] for viewport overlay.
    virtual void on_draw_gizmos() {}
    // Fired on the entity["active"] false→true / true→false edge — see
    // _poll_active_state()'s comment below for how this is detected, and
    // unity2d_script_api.hpp's MonoBehaviour::OnEnable()/OnDisable() for the
    // Unity-cased overrides scripts actually use.
    virtual void on_enable()  {}
    virtual void on_disable() {}
    virtual void on_destroy() {}

    // ── Physics callbacks ─────────────────────────────────────────────────────
    virtual void on_collision_enter(EntityRef /*other*/) {}
    virtual void on_collision_stay (EntityRef /*other*/) {}
    virtual void on_collision_exit (EntityRef /*other*/) {}
    virtual void on_trigger_enter  (EntityRef /*other*/) {}
    virtual void on_trigger_stay   (EntityRef /*other*/) {}
    virtual void on_trigger_exit   (EntityRef /*other*/) {}

    // ── Animation events ──────────────────────────────────────────────────────
    // Fired (one ScriptSystem frame after the triggering AnimatorSystem frame,
    // same delivery path as collision/trigger callbacks — see _pending_events
    // in ScriptSystem::update) when AnimatorSystem advances onto a clip frame
    // that has a named event attached via unity2d_script_api.hpp's
    // Animator::AddEvent(). MonoBehaviour bridges this to OnAnimationEvent().
    virtual void on_animation_event(std::string /*event_name*/) {}

    // ── Convenience: Transform ────────────────────────────────────────────────
    // Naming follows Unity: get_x/get_y/set_x/set_y/get_position/set_position
    // operate in LOCAL space (Transform.localPosition) — relative to the
    // parent, or to world origin for an un-parented entity (the common case,
    // and the only case that existed before parenting was added, so this
    // keeps every existing script working exactly as before). Use the
    // get_world_*/set_world_position family below when you specifically need
    // world space (Transform.position), e.g. distance checks, aiming, or
    // camera-relative logic on a parented object.
    Entity* transform() {
        if (!entity) return nullptr;
        if (!has_component(*entity,"Transform")) return nullptr;
        return &(*entity)["components"]["Transform"];
    }

    float get_x() { auto* t=transform(); return t?t->value("x",0.f):0.f; }
    float get_y() { auto* t=transform(); return t?t->value("y",0.f):0.f; }
    void  set_x(float v){ auto* t=transform(); if(t && t->value("x",0.f)!=v){ (*t)["x"]=v; transform::mark_local_dirty(entity->value("id",0)); } }
    void  set_y(float v){ auto* t=transform(); if(t && t->value("y",0.f)!=v){ (*t)["y"]=v; transform::mark_local_dirty(entity->value("id",0)); } }
    void  move(float dx,float dy){ auto* t=transform(); if(t){ (*t)["x"] = get_x()+dx; (*t)["y"] = get_y()+dy; transform::mark_local_dirty(entity->value("id",0)); } }

    float get_rotation(){ auto* t=transform(); return t?t->value("rotation",0.f):0.f; }
    void  set_rotation(float r){ auto* t=transform(); if(t && t->value("rotation",0.f)!=r){ (*t)["rotation"]=r; transform::mark_local_dirty(entity->value("id",0)); } }

    float get_scale_x(){ auto* t=transform(); return t?t->value("scale_x",1.f):1.f; }
    float get_scale_y(){ auto* t=transform(); return t?t->value("scale_y",1.f):1.f; }
    void  set_scale(float sx,float sy){ auto* t=transform(); if(t && (t->value("scale_x",1.f)!=sx || t->value("scale_y",1.f)!=sy)){(*t)["scale_x"]=sx;(*t)["scale_y"]=sy; transform::mark_local_dirty(entity->value("id",0));} }

    // ── World-space transform (Unity's Transform.position / .eulerAngles) ───
    // Safe to call on any entity, parented or not — these read through the
    // per-frame cached world TRS (transform_system.hpp) computed by walking
    // up the parent chain, and fall back to local values for root entities.
    float get_world_x()        { return entity ? transform::cached_world(*entity).x : 0.f; }
    float get_world_y()        { return entity ? transform::cached_world(*entity).y : 0.f; }
    float get_world_rotation() { return entity ? transform::cached_world(*entity).rotation : 0.f; }
    float get_world_scale_x()  { return entity ? transform::cached_world(*entity).scale_x : 1.f; }
    float get_world_scale_y()  { return entity ? transform::cached_world(*entity).scale_y : 1.f; }
    std::pair<float,float> get_world_position() {
        if (!entity) return {0.f, 0.f};
        auto wt = transform::cached_world(*entity);
        return {wt.x, wt.y};
    }

    // Moves the entity so its WORLD position equals (x, y), correctly
    // accounting for any parent transform (i.e. converts to local space
    // before writing Transform.x/y). Equivalent to Unity's
    // `transform.position = new Vector3(x, y, 0)`.
    void set_world_position(float x, float y) {
        if (!entity || !all_entities) { set_position(x, y); return; }
        int pid = transform::parent_id_of(*entity);
        if (pid < 0) { set_position(x, y); return; }
        Entity* parent = _find_by_id(pid);
        if (!parent) { set_position(x, y); return; }
        transform::WorldTRS parent_world = transform::cached_world(*parent);
        transform::WorldTRS target_world; target_world.x = x; target_world.y = y;
        auto wt = transform::cached_world(*entity);
        target_world.rotation = wt.rotation;
        target_world.scale_x = wt.scale_x;
        target_world.scale_y = wt.scale_y;
        transform::WorldTRS local = transform::world_to_local(parent_world, target_world);
        auto* t = transform();
        if (t) { (*t)["x"] = local.x; (*t)["y"] = local.y; transform::mark_local_dirty(entity->value("id",0)); }
    }

    // ── Parenting (Unity Transform.parent / Transform.SetParent) ──────────────
    // Returns the parent entity, or nullptr if this entity is a root object.
    Entity* get_parent() {
        if (!entity) return nullptr;
        int pid = transform::parent_id_of(*entity);
        if (pid < 0) return nullptr;
        return _find_by_id(pid);
    }

    // Parents this entity under `new_parent` (pass nullptr to un-parent,
    // making it a root object again). By default preserves world-space
    // position/rotation/scale — the object will not visually jump, matching
    // what Unity does when you drag an object onto a new parent in the
    // Hierarchy window. Pass keep_world_position=false to instead keep the
    // current local values and let the object jump into the new parent's
    // local space. Refuses (returns false, logs a warning) if new_parent is
    // this entity itself or one of its own descendants, which would create
    // a cycle.
    bool set_parent(Entity* new_parent, bool keep_world_position = true) {
        if (!entity) return false;
        int new_parent_id = new_parent ? new_parent->value("id", -1) : -1;
        bool ok = transform::set_parent(*entity, new_parent_id, keep_world_position,
                                         [this](int id){ return _find_by_id(id); });
        if (ok) transform::mark_structure_dirty();
        if (!ok) {
            Debug::log("set_parent() ignored: would create a cycle for entity \"" +
                       entity->value("name", std::string("?")) + "\"");
        }
        return ok;
    }

private:
    Entity* _find_by_id(int id) {
        if (!all_entities) return nullptr;
        for (auto& e : *all_entities) if (e.value("id", 0) == id) return &e;
        return nullptr;
    }

    // Recursive helpers for Make() — key-value pair setup.
    void _make_setup(Entity*) {}
    template <class T, class... Rest>
    void _make_setup(Entity* e, const char* key, T val, Rest... rest) {
        if (e) (*e)[key] = val;
        _make_setup(e, rest...);
    }

public:

    // ── Convenience: Rigidbody ────────────────────────────────────────────────
    Entity* rigidbody() {
        if (!entity||!has_component(*entity,"Rigidbody2D")) return nullptr;
        return &(*entity)["components"]["Rigidbody2D"];
    }

    float get_velocity_x(){ auto* rb=rigidbody(); if(!rb) return 0.f; if(rb->contains("vx")) return rb->value("vx",0.f); return rb->value("velocity_x",0.f); }
    float get_velocity_y(){ auto* rb=rigidbody(); if(!rb) return 0.f; if(rb->contains("vy")) return rb->value("vy",0.f); return rb->value("velocity_y",0.f); }
    void  set_velocity(float vx,float vy){
        auto* rb=rigidbody();
        if (!rb) return;
        (*rb)["velocity_x"]=vx; (*rb)["velocity_y"]=vy;
        (*rb)["vx"]=vx; (*rb)["vy"]=vy;
        (*rb)["_sleeping"]=false;
        // If moving upward (jump), clear stale grounded flag immediately so the
        // physics gravity/clamp block in the same-frame script update does not see
        // grounded=true and zero out the jump velocity before the solver runs.
        if (vy < 0.f) (*rb)["_grounded"] = false;
    }
    void  add_force(float fx,float fy){
        auto* rb=rigidbody();
        if (rb){
            (*rb)["_force_x"]=(float)rb->value("_force_x",0.f)+fx;
            (*rb)["_force_y"]=(float)rb->value("_force_y",0.f)+fy;
            (*rb)["_sleeping"]=false;
        }
    }
    void  add_impulse(float jx,float jy){
        auto* rb=rigidbody();
        if (!rb) return;
        float m=std::max(rb->value("mass",1.f),1e-9f);
        float nvx=(float)rb->value("velocity_x",0.f)+jx/m;
        float nvy=(float)rb->value("velocity_y",0.f)+jy/m;
        (*rb)["velocity_x"]=nvx; (*rb)["velocity_y"]=nvy;
        (*rb)["vx"]=nvx; (*rb)["vy"]=nvy;
        (*rb)["_sleeping"]=false;
    }

    bool is_grounded(float tolerance=4.f){
        (void)tolerance;
        auto* rb=rigidbody(); if(!rb) return false;
        return rb->value("_grounded",false);
    }

    // ── Convenience: get arbitrary component ─────────────────────────────────
    Entity* get_component(const std::string& name){
        if (!entity||!has_component(*entity,name)) return nullptr;
        return &(*entity)["components"][name];
    }

    // ── Find another entity by name ───────────────────────────────────────────
    Entity* find_entity(const std::string& name){
        if (!all_entities) return nullptr;
        for (auto& e : *all_entities)
            if (e.value("name","")==name) return &e;
        return nullptr;
    }

    // ── Find another entity by id ─────────────────────────────────────────────
    // IMPORTANT: EntityList is a std::vector<Entity>. Its buffer can move
    // (on push_back/instantiate growth) and its elements can shift (on
    // erase, e.g. when any entity is destroyed) on basically any frame.
    // That means an Entity* cached across frames - e.g. `player =
    // find_entity("Player")` stored in awake() and reused in update() -
    // can silently dangle the very next time something else spawns or is
    // destroyed anywhere in the scene. Prefer caching the entity's `id`
    // (stable for the entity's lifetime) and re-resolving with this helper
    // each time you need the pointer, instead of caching the Entity* itself.
    Entity* find_entity_by_id(int id){
        if (!all_entities) return nullptr;
        for (auto& e : *all_entities)
            if (e.value("id",0)==id) return &e;
        return nullptr;
    }

    // ── Destroy this entity (optionally after a delay) ────────────────────────
    void destroy(){ if(entity) (*entity)["_destroyed"]=true; }
    void destroy(Entity* target, float delay = 0.f) {
        if (!target) return;
        if (delay > 0.f) (*target)["_destroy_timer"] = target->value("_destroy_timer",0.f) + delay;
        else             (*target)["_destroyed"] = true;
    }

    // ── Instantiate / find ─────────────────────────────────────────────────────
    // Clone a template entity into the live scene (e.g. spawning a bullet/enemy
    // prefab-style). Mirrors Python ScriptBase.instantiate().
    Entity* instantiate(const Entity& tmpl, std::optional<float> x = std::nullopt,
                                              std::optional<float> y = std::nullopt) {
        if (!all_entities) return nullptr;

        // IMPORTANT: `tmpl` is very often a reference to an entity that lives
        // INSIDE *all_entities* itself (every bullet/enemy prefab in this
        // project - PulseBoltTemplate, EnemyPulseBoltTemplate, etc. - is a
        // real entity in the scene that scripts Find() and pass in here).
        // We must read everything we need from `tmpl` BEFORE doing anything
        // that could reallocate the vector's buffer. If we grew/reserved
        // first, `tmpl` would be left pointing at freed memory and
        // deep_clone()/value() below would read through a dangling
        // reference - a use-after-free that only manifests once the vector
        // actually needs to grow (i.e. intermittently, after enough
        // bullets/enemies have spawned in a session).
        Entity copy = tmpl.deep_clone();
        std::string clone_name = tmpl.value("name", "Clone") + std::string("(Clone)");
        int new_id = next_entity_id();

        // Save this script's own entity ID so we can re-resolve the pointer
        // after reserve() below potentially reallocates the buffer.
        int self_id = entity ? entity->value("id", -1) : -1;

        // EntityList is a std::vector<Entity>. If push_back would exceed the
        // vector's current capacity, the buffer reallocates and EVERY
        // existing Entity* into it - including this script's own `entity`
        // pointer and every other script instance's entity pointer - 
        // immediately dangles. load_scene() reserves generous headroom up
        // front, but a long enough play session could still exhaust it.
        // Grow proactively here.
        if (all_entities->size() + 1 > all_entities->capacity()) {
            size_t grown = std::max<size_t>(all_entities->capacity() * 4, all_entities->capacity() + 1024);
            all_entities->reserve(grown);
        }

        copy["id"]   = new_id;
        copy["name"] = clone_name;
        if (has_component(copy,"Transform")) {
            if (x) copy["components"]["Transform"]["x"] = *x;
            if (y) copy["components"]["Transform"]["y"] = *y;
        }

        // A clone created during another script's Update is rendered before
        // its own Animator necessarily gets a first tick.  Prime sheet-mode
        // SpriteRenderers now so that one frame never shows the entire atlas
        // (for example all five slash frames or all four bolt frames).
        if (has_component(copy, "Animator") && has_component(copy, "SpriteRenderer")) {
            auto& anim = copy["components"]["Animator"];
            if (anim.value("use_sprite_sheet", false)) {
                const std::string sheet = anim.value("sprite_sheet", std::string{});
                const int fw = anim.value("frame_width", 0);
                const int fh = anim.value("frame_height", 0);
                if (!sheet.empty() && fw > 0 && fh > 0) {
                    const int cols = std::max(1, anim.value("sheet_columns", 1));
                    const int frame = std::max(0, (int)anim.value("frame", 0.f));
                    const int spacing = anim.value("sheet_spacing", 0);
                    const int padding = anim.value("sheet_padding", 0);
                    auto& sr = copy["components"]["SpriteRenderer"];
                    sr["texture"] = sheet;
                    sr["use_source_rect"] = true;
                    sr["source_x"] = (frame % cols) * (fw + spacing) + padding;
                    sr["source_y"] = (frame / cols) * (fh + spacing) + padding;
                    sr["source_w"] = fw;
                    sr["source_h"] = fh;
                }
            }
        }
        all_entities->push_back(copy);
        transform::mark_structure_dirty();

        // Re-resolve this script's own entity pointer if the buffer was
        // reallocated by the reserve() above. Without this, any access to
        // `this->entity` after instantiate() returns — including the very
        // next line of the caller's update()/awake() — reads through freed
        // memory (use-after-free), which is the exact intermittent crash
        // reported: works until a spawn triggers a growth, then crashes
        // "most of the time" after that (depends on whether the allocator
        // has overwritten the old buffer by the time the script touches it).
        if (self_id >= 0) {
            // Check if the buffer address changed (means reallocation happened).
            // We saved self_id before reserve() so we can scan for it now.
            bool found = false;
            for (auto& e : *all_entities) {
                if (e.value("id", 0) == self_id) {
                    entity = EntityRef(e);
                    found = true;
                    break;
                }
            }
            if (!found) entity = EntityRef(nullptr);
        }

        return &all_entities->back();
    }

    std::vector<Entity*> find_entities_with(const std::string& comp_type) {
        std::vector<Entity*> out;
        if (!all_entities) return out;
        for (auto& e : *all_entities)
            if (has_component(e, comp_type)) out.push_back(&e);
        return out;
    }

    std::vector<Entity*> find_entities_with_tag(const std::string& t) {
        std::vector<Entity*> out;
        if (!all_entities) return out;
        for (auto& e : *all_entities) {
            if (!e.contains("tags")) continue;
            for (auto& tg : e["tags"])
                if (tg.is_string() && tg.get<std::string>()==t) { out.push_back(&e); break; }
        }
        return out;
    }

    // ── Messaging ──────────────────────────────────────────────────────────────
    // NOTE: C++ has no Python-style getattr-by-name, so this can't call an
    // arbitrary method name on another script. It currently logs the call;
    // for real cross-script signaling, prefer EventBus::instance().emit().
    void send_message(Entity* target, const std::string& method, Entity* /*arg*/ = nullptr) {
        if (!target) return;
        Debug::log(std::string("send_message(\"") + method + "\") -> " + target->value("name", "?") +
                   "  [no-op: use EventBus for custom messages]");
    }
    void broadcast_message(const std::string& method, Entity* arg = nullptr) {
        if (!all_entities) return;
        for (auto& e : *all_entities) send_message(&e, method, arg);
    }

    // ── Physics queries (delegate to phys:: namespace in physics.hpp) ─────────
    std::optional<phys::RayHit> raycast(float ox,float oy,float dx,float dy,
                                         float max_dist=1000.f, int layer_mask=0xFFFF,
                                         bool include_triggers=false) {
        if (!all_entities) return std::nullopt;
        return phys::raycast(*all_entities, ox,oy,dx,dy, max_dist, layer_mask, include_triggers);
    }
    std::vector<Entity*> overlap_circle(float cx,float cy,float radius,int layer_mask=0xFFFF) {
        if (!all_entities) return {};
        return phys::overlap_circle(*all_entities, cx,cy,radius, layer_mask);
    }
    std::vector<Entity*> overlap_box(float cx,float cy,float w,float h,float rotation=0.f,int layer_mask=0xFFFF) {
        if (!all_entities) return {};
        return phys::overlap_box(*all_entities, cx,cy,w,h,rotation, layer_mask);
    }
    std::vector<Entity*> point_cast(float x,float y,int layer_mask=0xFFFF) {
        if (!all_entities) return {};
        return phys::point_cast(*all_entities, x,y, layer_mask);
    }

    // ── Transform movement helpers (Unity-style) ───────────────────────────────
    void set_position(float x,float y){ auto* t=transform(); if(t){ (*t)["x"]=x; (*t)["y"]=y; transform::mark_local_dirty(entity->value("id",0)); } }
    std::pair<float,float> get_position(){ return {get_x(), get_y()}; }

    // Local-space translate (moves along current rotation), or world-space if local=false
    void translate(float dx, float dy, bool local=true) {
        auto* t = transform(); if (!t) return;
        if (local) {
            float rad = Mathf::deg2rad(t->value("rotation",0.f));
            float rx = dx*std::cos(rad) - dy*std::sin(rad);
            float ry = dx*std::sin(rad) + dy*std::cos(rad);
            (*t)["x"] = t->value("x",0.f)+rx;
            (*t)["y"] = t->value("y",0.f)+ry;
        } else {
            auto [wx, wy] = get_world_position();
            set_world_position(wx+dx, wy+dy);
        }
    }

    void rotate(float degrees) {
        auto* t = transform(); if (!t) return;
        float r = std::fmod(t->value("rotation",0.f)+degrees, 360.f);
        if (r < 0) r += 360.f;
        (*t)["rotation"] = r;
    }

    // Rotates so the entity's local +X axis points at world point (tx, ty).
    void look_at(float tx, float ty) {
        auto* t = transform(); if (!t) return;
        auto [wx, wy] = get_world_position();
        float dx = tx - wx, dy = ty - wy;
        float world_target_rot = -Mathf::rad2deg(std::atan2(dy,dx));
        // Convert the desired world rotation into local rotation if parented.
        if (entity) {
            int pid = transform::parent_id_of(*entity);
            Entity* parent = pid >= 0 ? _find_by_id(pid) : nullptr;
            if (parent) {
                float parent_rot = transform::cached_world(*parent).rotation;
                (*t)["rotation"] = world_target_rot - parent_rot;
                return;
            }
        }
        (*t)["rotation"] = world_target_rot;
    }

    // World-space distance to another entity (correct regardless of whether
    // either entity is parented).
    float distance_to(Entity* other) {
        if (!other || !has_component(*other,"Transform")) return std::numeric_limits<float>::infinity();
        auto [wx, wy] = get_world_position();
        auto wt = transform::cached_world(*other);
        float ox = wt.x, oy = wt.y;
        return Mathf::distance(wx, wy, ox, oy);
    }

    // ── Animation helpers ───────────────────────────────────────────────────────
    void play_animation(const std::string& anim_name) {
        auto* a = get_component("Animator"); if (!a) return;
        (*a)["current_animation"] = anim_name;
        (*a)["frame"] = 0.f;
        (*a)["playing"] = true;
        (*a)["_last_event_frame__" + anim_name] = -1;
    }
    void stop_animation() { auto* a = get_component("Animator"); if (a) (*a)["playing"]=false; }
    void set_animator_param(const std::string& k, float v) {
        auto* a = get_component("Animator"); if (!a) return;
        (*a)["parameters"][k] = v;
    }
    float get_animator_param(const std::string& k, float def=0.f) {
        auto* a = get_component("Animator");
        if (!a || !a->contains("parameters")) return def;
        return (*a)["parameters"].value(k, def);
    }

    // ── Audio helpers (component-driven; runtime audio is handled by AudioSystem) ─
    void play_audio_source() {
        auto* a = get_component("AudioSource"); if (!a) return;
        (*a)["_play_now"] = true;
    }
    void stop_audio_source() {
        auto* a = get_component("AudioSource"); if (!a) return;
        (*a)["_is_playing"] = false;
        (*a)["_play_now"] = false;
    }
    // Play a specific clip by path — swaps "clip" on the AudioSource, then
    // triggers playback. Equivalent to assigning AudioSource.clip + Play().
    //   play_sound("sfx/jump.wav");
    void play_sound(const std::string& clipPath) {
        auto* a = get_component("AudioSource"); if (!a) return;
        (*a)["clip"] = clipPath;
        (*a)["_play_now"] = true;
    }
    // Play a one-shot clip (does not interrupt / change the main "clip" field).
    // Mirrors Unity's AudioSource.PlayOneShot. Stores the request under
    // "_oneshot_clip" / "_oneshot_volume"; AudioSystem picks it up each tick.
    void play_one_shot(const std::string& clipPath, float volume = 1.f) {
        auto* a = get_component("AudioSource"); if (!a) return;
        (*a)["_oneshot_clip"]   = clipPath;
        (*a)["_oneshot_volume"] = Mathf::clamp01(volume);
        (*a)["_oneshot_play"]   = true;
    }
    // Volume: 0.0 (silent) .. 1.0 (full). Safe to call every frame.
    void set_audio_volume(float v) { auto* a = get_component("AudioSource"); if (a) (*a)["volume"] = Mathf::clamp01(v); }
    float get_audio_volume()       { auto* a = get_component("AudioSource"); return a ? a->value("volume", 1.f) : 0.f; }
    // Pitch: 1.0 = normal, 0.5 = half speed, 2.0 = double speed.
    void set_audio_pitch(float p) { auto* a = get_component("AudioSource"); if (a) (*a)["pitch"] = p; }
    float get_audio_pitch()       { auto* a = get_component("AudioSource"); return a ? a->value("pitch", 1.f) : 1.f; }
    // Loop on/off.
    void set_audio_loop(bool v) { auto* a = get_component("AudioSource"); if (a) (*a)["loop"] = v; }
    bool is_audio_playing()     { auto* a = get_component("AudioSource"); return a && a->value("_is_playing", false); }

    // ── HealthComponent helpers ──────────────────────────────────────────────────
    // OnDeath()/OnDamageTaken() handlers (registered above) are dispatched
    // automatically by _poll_health_callbacks(), called once per frame from
    // ScriptSystem::update() — see that method's comment for how the
    // once-per-event guarantee is implemented without HealthComponent itself
    // needing to know about script callbacks.
    void take_damage(float amount) {
        auto* h = get_component("HealthComponent");
        if (!h || h->value("invincible",false)) return;
        float cur = std::max(0.f, h->value("current_health",0.f) - amount);
        (*h)["current_health"] = cur;
        // Stash who dealt this hit (if any — see the take_damage(amount,
        // attacker) overload above) so _poll_health_callbacks() can hand it
        // to OnDamageTaken() handlers without HealthComponent's JSON needing
        // a dedicated field for it.
        if (_pending_damage_attacker) _last_damage_attacker = _pending_damage_attacker;
    }
    void heal(float amount) {
        auto* h = get_component("HealthComponent"); if (!h) return;
        float mx = h->value("max_health",100.f);
        (*h)["current_health"] = std::min(mx, h->value("current_health",0.f)+amount);
    }
    bool is_dead() {
        auto* h = get_component("HealthComponent");
        return h && h->value("current_health",1.f) <= 0.f;
    }
    // Read current / max health without touching the raw component pointer.
    float current_health() { auto* h = get_component("HealthComponent"); return h ? h->value("current_health", 0.f) : 0.f; }
    float max_health()     { auto* h = get_component("HealthComponent"); return h ? h->value("max_health", 100.f) : 100.f; }
    // 0..1 ratio — handy for health bars: healthBar.SetFill(health_ratio()).
    float health_ratio()   { float mx = max_health(); return mx > 0.f ? Mathf::clamp01(current_health() / mx) : 0.f; }
    // Restore to full health.
    void full_heal()       { auto* h = get_component("HealthComponent"); if (h) (*h)["current_health"] = h->value("max_health", 100.f); }
    // Toggle invincibility — e.g. during a respawn grace period or i-frames.
    void set_invincible(bool v) { auto* h = get_component("HealthComponent"); if (h) (*h)["invincible"] = v; }
    bool is_invincible()        { auto* h = get_component("HealthComponent"); return h && h->value("invincible", false); }

    // ── HealthComponent events (OnDeath / OnDamageTaken) ─────────────────────
    // Every game currently polls is_dead() in update() to notice death, and
    // has no way at all to react to a hit with context (how much damage,
    // from whom) without manually diffing current_health() itself. These
    // register a callback once (typically in Awake()) and the engine fires
    // it automatically — exactly once per death, and once per take_damage()
    // call that actually reduced health — by comparing this frame's
    // HealthComponent state against last frame's every tick (see
    // _poll_health_callbacks(), called from ScriptSystem::update() right
    // alongside coroutine ticking, so it shares the same once-per-frame
    // cadence and pause/timescale semantics as everything else here).
    //
    //   OnDeath([this]{
    //       play_sound("sfx/death.wav");
    //       Invoke([this]{ Destroy(); }, 0.6f);
    //   });
    //   OnDamageTaken([this](float amount, Entity* attacker){
    //       Flash(255, 0, 0, 0.08f);
    //   });
    //
    // Multiple handlers may be registered; all of them fire. Damage dealt
    // via take_damage() with no explicit attacker fires with attacker=nullptr.
    using DeathHandler  = std::function<void()>;
    using DamageHandler = std::function<void(float amount, Entity* attacker)>;
    void OnDeath(DeathHandler handler) { _death_handlers.push_back(std::move(handler)); }
    void OnDamageTaken(DamageHandler handler) { _damage_handlers.push_back(std::move(handler)); }

    // take_damage() overload that also records who dealt the hit, so a
    // registered OnDamageTaken handler can react to it (knockback direction,
    // kill-credit, etc.). Plain take_damage(amount) still works exactly as
    // before and reports attacker=nullptr to handlers.
    void take_damage(float amount, Entity* attacker) {
        _pending_damage_attacker = attacker;
        take_damage(amount);
        _pending_damage_attacker = nullptr;
    }

    // ── Timer component helpers ──────────────────────────────────────────────────
    void start_timer(std::optional<float> duration = std::nullopt) {
        auto* t = get_component("Timer"); if (!t) return;
        if (duration) (*t)["duration"] = *duration;
        (*t)["elapsed"] = 0.f;
        (*t)["running"] = true;
    }
    void stop_timer() { auto* t = get_component("Timer"); if (t) (*t)["running"]=false; }
    // Read how far the timer has progressed (0..1). Returns 0 if no Timer component.
    float timer_ratio() {
        auto* t = get_component("Timer"); if (!t) return 0.f;
        float dur = t->value("duration", 1.f);
        return dur > 0.f ? Mathf::clamp01(t->value("elapsed", 0.f) / dur) : 0.f;
    }
    bool timer_finished() {
        auto* t = get_component("Timer"); if (!t) return false;
        return !t->value("running", false) && t->value("elapsed", 0.f) >= t->value("duration", 0.f);
    }

    // ── Tween component helpers ──────────────────────────────────────────────────
    void tween_to(float tx, float ty, float duration, const std::string& ease="ease_in_out") {
        auto* tw = get_component("Tween"); auto* t = transform();
        if (!tw || !t) return;
        (*tw)["from_x"] = t->value("x",0.f);
        (*tw)["from_y"] = t->value("y",0.f);
        (*tw)["to_x"] = tx;
        (*tw)["to_y"] = ty;
        (*tw)["duration"] = duration;
        (*tw)["ease"] = ease;
        (*tw)["elapsed"] = 0.f;
        (*tw)["playing"] = true;
        (*tw)["target_property"] = "position";
    }
    // Tween uniform scale from current value to target over `duration` seconds.
    void tween_scale(float to_scale, float duration, const std::string& ease="ease_in_out") {
        auto* tw = get_component("Tween"); auto* t = transform();
        if (!tw || !t) return;
        (*tw)["from_x"] = t->value("scale_x", 1.f);
        (*tw)["from_y"] = t->value("scale_y", 1.f);
        (*tw)["to_x"] = to_scale;
        (*tw)["to_y"] = to_scale;
        (*tw)["duration"] = duration;
        (*tw)["ease"] = ease;
        (*tw)["elapsed"] = 0.f;
        (*tw)["playing"] = true;
        (*tw)["target_property"] = "scale";
    }
    // Fade the SpriteRenderer opacity from its current value to `to_alpha` (0..1).
    void tween_alpha(float to_alpha, float duration, const std::string& ease="linear") {
        auto* tw = get_component("Tween");
        auto* sr = get_component("SpriteRenderer");
        if (!tw || !sr) return;
        (*tw)["from_x"] = sr->value("opacity", 1.f);
        (*tw)["to_x"]   = Mathf::clamp01(to_alpha);
        (*tw)["duration"] = duration;
        (*tw)["ease"]     = ease;
        (*tw)["elapsed"]  = 0.f;
        (*tw)["playing"]  = true;
        (*tw)["target_property"] = "opacity";
    }
    // Convenience: fade out to invisible.
    void fade_out(float duration = 0.5f) { tween_alpha(0.f, duration); }
    // Convenience: fade in to fully visible.
    void fade_in(float duration = 0.5f)  { tween_alpha(1.f, duration); }
    // Stop any running tween.
    void stop_tween() { auto* tw = get_component("Tween"); if (tw) (*tw)["playing"] = false; }
    bool is_tweening() { auto* tw = get_component("Tween"); return tw && tw->value("playing", false); }

    // ── Logging / PlayerPrefs / Debug passthroughs ────────────────────────────────
    void log(const std::string& msg) { Debug::log("[" + name() + "] " + msg); }
    void draw_debug_line(float x1,float y1,float x2,float y2,
                          Uint8 r=255,Uint8 g=255,Uint8 b=0, float duration=0.f) {
        Debug::draw_line(x1,y1,x2,y2,r,g,b,255,duration);
    }

    // ── Input shortcuts ───────────────────────────────────────────────────────
    bool key_down(SDL_Scancode k)  { return input && input->get_key(k); }
    bool key_pressed(SDL_Scancode k){ return input && input->get_key_down(k); }
    bool key_released(SDL_Scancode k){ return input && input->get_key_up(k); }
    float mouse_x(){ return input ? (float)input->mouse_world_x : 0.f; }
    float mouse_y(){ return input ? (float)input->mouse_world_y : 0.f; }
    bool  mouse_btn(int b){ return input && input->get_mouse_button(b); }
    bool  mouse_btn_down(int b){ return input && input->get_mouse_button_down(b); }

    // ── Entity name/tag ───────────────────────────────────────────────────────
    std::string name(){ return entity?entity->value("name",""):""; }
    std::string tag() {
        if (!entity) return "";
        if (entity->contains("tag") && (*entity)["tag"].is_string()) return (*entity)["tag"].get<std::string>();
        if (entity->contains("tags") && (*entity)["tags"].is_array()) {
            for (auto& tg : (*entity)["tags"]) if (tg.is_string()) return tg.get<std::string>();
        }
        return "";
    }
    bool has_tag(const std::string& t){
        if (!entity) return false;
        if (entity->value("tag","")==t) return true;
        if (entity->contains("tags") && (*entity)["tags"].is_array()) {
            for (auto& tg : (*entity)["tags"]) if (tg.is_string() && tg.get<std::string>()==t) return true;
        }
        return false;
    }

    // ── Entity value helpers ────────────────────────────────────────────────────
    // Safe read of a typed value from this script's entity.
    // Replaces `entity ? entity->value("hp", 3) : 3` in Awake()/Update().
    //   int hp = val("hp", 3);
    template <class T>
    T val(const std::string& key, T def) const {
        return entity ? entity->value(key, def) : def;
    }

    // Check if the entity has a given key at root level.
    // Replaces `entity && entity->contains("hp")`.
    bool has(const std::string& key) const {
        return entity && entity->contains(key);
    }

    // Safe write of a typed value to this script's entity.
    // Replaces `if (entity) (*entity)["hp"] = 100;`.
    //   SetValue("hp", 100);
    //   SetValue("active", true);
    template <class T>
    void SetValue(const std::string& key, T val) {
        entity.SetValue(key, val);
    }
    // Static overload for writing to ANY entity.
    //   SetValue(*spawned, "team", 1);
    template <class T>
    static void SetValue(Entity& e, const std::string& key, T val) {
        e[key] = val;
    }

    // ── Clear shorthand helpers ────────────────────────────────────────────────
    // These save typing without sacrificing readability.
    float dt() const { return (float)Time::delta_time; }
    void pos(float px, float py) { set_world_position(px, py); }
    void vel(float vx, float vy) { set_velocity(vx, vy); }
    bool grounded() { return is_grounded(); }
    void play(const std::string& clip) { play_animation(clip); }
    Entity* find(const std::string& name) { return find_entity(name); }
    Entity* comp(const std::string& name) { return get_component(name); }
    bool held(SDL_Scancode k) { return input && input->get_key(k); }
    bool pressed(SDL_Scancode k) { return input && input->get_key_down(k); }
    bool released(SDL_Scancode k) { return input && input->get_key_up(k); }
    float axis(const std::string& n) { return input ? input->get_axis(n) : 0.f; }
    float raw(const std::string& n) { return input ? input->get_axis_raw(n) : 0.f; }
    bool btn(const std::string& n) { return input && input->get_button(n); }
    bool btn_down(const std::string& n) { return input && input->get_button_down(n); }
    bool btn_up(const std::string& n) { return input && input->get_button_up(n); }

    // ── Bulk spawn + configure ──────────────────────────────────────────────────
    // Spawns a template, activates it, and sets any number of key-value pairs.
    // Replaces the 8+ line Instantiate + null-check + activate + field-set pattern.
    //   EntityRef s = CreateEntity(*tpl, ox, oy, "team", 1, "damage", damage, "life_time", 0.16f);
    template <class... Args>
    EntityRef CreateEntity(Entity& tmpl, float x, float y, Args... args) {
        Entity* e = instantiate(tmpl, x, y);
        if (!e) return nullptr;
        (*e)["active"] = true;
        _make_setup(e, args...);
        return e;
    }

    // ── Elapsed time ──────────────────────────────────────────────────────────
    float time_scale = 1.f;

    // ── Coroutines (Unity-style StartCoroutine) ───────────────────────────────
    // See the coro:: namespace + CO_WAIT_* macros above for the body syntax.
    // Coroutines are owned per script-instance and ticked by ScriptSystem
    // right after this instance's update(dt) runs (see _tick_coroutines()
    // call site in ScriptSystem::update), so they share the same dt/pause/
    // time-scale semantics as everything else and are torn down automatically
    // when the instance itself is destroyed (no leaks across script reloads).
    coro::Handle start_coroutine(coro::Body body) {
        auto inst = std::make_shared<coro::Instance>();
        inst->body = std::move(body);
        _coroutines.push_back(inst);
        return inst;
    }

    // Stops one running coroutine. Safe to call with a handle that already
    // finished or was already stopped (no-op). Mirrors Unity's
    // StopCoroutine(Coroutine) overload — there's no by-name lookup here
    // since C++ has no string-keyed IEnumerator methods to match against.
    void stop_coroutine(const coro::Handle& handle) {
        if (handle) handle->stopped = true;
    }

    // Stops every coroutine this script instance currently has running.
    // Mirrors Unity's StopAllCoroutines().
    void stop_all_coroutines() {
        for (auto& c : _coroutines) c->stopped = true;
    }

    // Advances every live coroutine by one logical step. A coroutine resumes
    // this frame if: it's waiting on a frame boundary (always — "one frame"
    // means "the next time this function runs"), its WaitForSeconds timer
    // (scaled by dt, which already has Time.timeScale baked in upstream) has
    // counted down to zero, or its WaitUntil predicate now returns true.
    // Finished/stopped coroutines are swept from the list at the end of the
    // same call so a coroutine that StartCoroutine()s another one doesn't
    // see a half-cleaned-up list.
    void _tick_coroutines(float dt) {
        if (_coroutines.empty()) return;
        for (auto& inst : _coroutines) {
            if (inst->stopped) continue;
            auto& co = inst->co;
            bool ready;
            switch (co.wait_kind) {
                case coro::WaitKind::Seconds:
                    co.wait_timer -= dt;
                    ready = co.wait_timer <= 0.f;
                    break;
                case coro::WaitKind::Until:
                    ready = co.wait_until && co.wait_until();
                    break;
                case coro::WaitKind::EndOfFrame:
                    // Never resumed here — only resume_end_of_frame() (called
                    // once per frame after render.draw() returns) clears this
                    // wait. If we said "ready" here, the coroutine would
                    // resume on the very next ScriptSystem tick instead of
                    // after rendering, defeating the entire point of this
                    // wait kind.
                    ready = false;
                    break;
                case coro::WaitKind::Frame:
                case coro::WaitKind::None: // first-ever resume
                default:
                    ready = true;
                    break;
            }
            if (!ready) continue;
            co.wait_kind = coro::WaitKind::None;
            coro::CoroutineStep step = inst->body(co);
            if (step == coro::CoroutineStep::Done) inst->stopped = true;
        }
        _coroutines.erase(
            std::remove_if(_coroutines.begin(), _coroutines.end(),
                            [](const coro::Handle& h){ return h->stopped; }),
            _coroutines.end());
    }

    // Resumes every coroutine on THIS instance that's currently parked on
    // CO_WAIT_END_OF_FRAME — called once per frame by
    // ScriptSystem::resume_end_of_frame() right after the render pass
    // finishes (see that method's call sites in engine/core.cpp and
    // editor/src/panels.hpp). Mirrors _tick_coroutines' resume/sweep shape
    // but is driven by the renderer instead of by dt.
    void _resume_end_of_frame() {
        if (_coroutines.empty()) return;
        for (auto& inst : _coroutines) {
            if (inst->stopped) continue;
            auto& co = inst->co;
            if (co.wait_kind != coro::WaitKind::EndOfFrame) continue;
            co.wait_kind = coro::WaitKind::None;
            coro::CoroutineStep step = inst->body(co);
            if (step == coro::CoroutineStep::Done) inst->stopped = true;
        }
        _coroutines.erase(
            std::remove_if(_coroutines.begin(), _coroutines.end(),
                            [](const coro::Handle& h){ return h->stopped; }),
            _coroutines.end());
    }

    // ── OnEnable/OnDisable + OnDeath/OnDamageTaken polling ───────────────────
    // Both of these are "noticed by comparing this frame's state to last
    // frame's", run once per script instance per ScriptSystem::update() call
    // (see the call site there for why a simple per-frame edge check is the
    // right approach: entity["active"] and HealthComponent.current_health are
    // each written from several unrelated places — the editor Inspector,
    // ObjectPoolSystem spawn/return, ScriptGraph SetActive nodes, take_damage()
    // itself — so there is no single setter to hook instead).
    //
    // _poll_active_state() fires on_enable()/on_disable() (MonoBehaviour
    // bridges these to OnEnable()/OnDisable()) the first time it observes
    // entity["active"] differ from what it saw last call. The very first
    // call (right after awake()) primes _last_active without firing
    // anything, matching Unity's behavior where OnEnable fires before Start()
    // for an object that starts active, not as a surprise "toggle" event.
    void _poll_active_state() {
        if (!entity) return;
        bool now_active = entity_active(*entity);
        if (!_active_state_primed) {
            _last_active = now_active;
            _active_state_primed = true;
            if (now_active) on_enable();
            return;
        }
        if (now_active != _last_active) {
            _last_active = now_active;
            if (now_active) on_enable(); else on_disable();
        }
    }

    // ── Invoke / InvokeRepeating (Unity-style one-liners) ────────────────────
    // Coroutines are powerful but verbose for "call this after N seconds" or
    // "call this every N seconds forever" — both are just a single
    // CO_WAIT_SECONDS wrapped in a loop, so these are thin convenience
    // wrappers over start_coroutine(), not a new mechanism.
    //   Invoke([this]{ SpawnEnemy(); }, 2.f);
    //   InvokeRepeating([this]{ SpawnEnemy(); }, 0.f, 0.5f);
    //   CancelInvoke(); // stops every pending Invoke/InvokeRepeating on this instance
    using InvokeFn = std::function<void()>;

    coro::Handle Invoke(InvokeFn fn, float delay) {
        auto h = start_coroutine([fn, delay](coro::Coroutine& co) -> coro::CoroutineStep {
            CO_BODY_BEGIN(co);
            CO_WAIT_SECONDS(co, delay);
            if (fn) fn();
            CO_BODY_END();
        });
        _invoke_handles.push_back(h);
        return h;
    }

    // Fires once after `initialDelay`, then every `repeatInterval` seconds
    // forever, until CancelInvoke() or StopCoroutine(handle) is called.
    // Loop is written as a `while` (not `for`) around CO_WAIT_SECONDS — see
    // SpriteRenderer2D::Blink()'s comment for why a for-loop here would fail
    // to compile (switch/case can't jump across a loop variable's init).
    coro::Handle InvokeRepeating(InvokeFn fn, float initialDelay, float repeatInterval) {
        auto h = start_coroutine([fn, initialDelay, repeatInterval](coro::Coroutine& co) -> coro::CoroutineStep {
            CO_BODY_BEGIN(co);
            CO_WAIT_SECONDS(co, initialDelay);
            while (true) {
                if (fn) fn();
                CO_WAIT_SECONDS(co, repeatInterval);
            }
            CO_BODY_END();
        });
        _invoke_handles.push_back(h);
        return h;
    }

    // Cancels every pending Invoke()/InvokeRepeating() started on this
    // instance. Does not touch coroutines started directly via
    // StartCoroutine() — use StopCoroutine()/StopAllCoroutines() for those.
    void CancelInvoke() {
        for (auto& h : _invoke_handles) stop_coroutine(h);
        _invoke_handles.clear();
    }
    // Cancels one specific Invoke/InvokeRepeating by its returned handle.
    void CancelInvoke(const coro::Handle& handle) { stop_coroutine(handle); }

    // Fires OnDeath() once (guarded by _death_fired, exactly like the
    // roadmap's HealthComponent._death_fired note — current_health can sit
    // at/below 0 for many frames, e.g. while a death animation plays, and
    // this must not refire on every one of them), and fires OnDamageTaken()
    // once per frame where current_health dropped versus last frame's
    // observed value. Polling current_health rather than wrapping
    // take_damage() directly means damage from ANY source (script, physics
    // contact damage, status-effect ticks written straight into the
    // component) is still caught, not just calls that went through this
    // class's own take_damage().
    void _poll_health_callbacks() {
        auto* h = get_component("HealthComponent");
        if (!h) return;
        float cur = h->value("current_health", 0.f);
        if (!_health_state_primed) {
            _last_known_health = cur;
            _health_state_primed = true;
            _death_fired = cur <= 0.f; // already dead at spawn — don't fire OnDeath for it
            return;
        }
        if (cur < _last_known_health && !_damage_handlers.empty()) {
            float amount = _last_known_health - cur;
            Entity* attacker = _last_damage_attacker;
            _last_damage_attacker = nullptr;
            for (auto& fn : _damage_handlers) if (fn) fn(amount, attacker);
        }
        _last_known_health = cur;
        if (cur <= 0.f) {
            if (!_death_fired) {
                _death_fired = true;
                for (auto& fn : _death_handlers) if (fn) fn();
            }
        } else {
            _death_fired = false; // revived (full_heal/heal after death) — rearm for next death
        }
    }

private:
    std::vector<coro::Handle> _coroutines;
    std::vector<coro::Handle> _invoke_handles;

    // OnEnable/OnDisable edge-detection state.
    bool _active_state_primed = false;
    bool _last_active = true;

    // OnDeath/OnDamageTaken edge-detection state.
    bool  _health_state_primed  = false;
    bool  _death_fired          = false;
    float _last_known_health    = 0.f;
    Entity* _pending_damage_attacker = nullptr; // set transiently by take_damage(amount, attacker)
    Entity* _last_damage_attacker    = nullptr; // consumed by _poll_health_callbacks()
    std::vector<DeathHandler>  _death_handlers;
    std::vector<DamageHandler> _damage_handlers;
};

#include "unity2d_script_api.hpp"

// ── Cross-DLL-safe script pointer ────────────────────────────────────────────
// ScriptBase instances are created by factories inside hot-reloadable DLLs.
// The DLL's `operator new` allocates the memory; we must use the DLL's
// `operator delete` to free it — calling the host's `delete` on DLL-allocated
// memory is UB when host and DLL link different CRTs (the default for separate
// Visual Studio projects). This custom deleter stores the destroy function
// obtained from the same factory that created the instance, ensuring
// new/delete pair always come from the same module.
struct ScriptDeleter {
    void (*fn)(ScriptBase*) = nullptr;
    void operator()(ScriptBase* p) const noexcept { if (p && fn) fn(p); }
};
using ScriptPtr = std::unique_ptr<ScriptBase, ScriptDeleter>;

// Factory pair: create and destroy both live in the DLL's translation unit
// (set by REGISTER_SCRIPT below), called from the host through function
// pointers. Stateless lambdas with no captures convert to function pointers,
// so both members are raw function pointers — no std::function overhead.
struct ScriptFactory {
    ScriptBase* (*create)();
    void (*destroy)(ScriptBase*);
};

class ScriptRegistry {
public:
    static ScriptRegistry& instance(){
        static ScriptRegistry inst; return inst;
    }

    void set_active_project(std::string project) {
        _active_project = std::move(project);
    }

    void set_active_project_from_scene_path(const std::string& scene_path) {
        _active_project = detail::infer_project_from_scene_path(scene_path);
    }

    const std::string& active_project() const { return _active_project; }

    void reg(const std::string& name, ScriptFactory f){
        _factories[name] = f;
    }

    // Removes exactly one factory. Per-file DLL hot reload uses this when a
    // class was renamed or its source file was deleted, so a registry entry
    // can never keep a function pointer into an unloaded module.
    void unreg(const std::string& name) {
        _factories.erase(name);
    }

    // Drops every registered factory. Called by the host right before a
    // freshly built scripts module re-registers, so a removed/renamed
    // script class doesn't leave its old factory callable (which would
    // point at code from a DLL that's about to be unloaded).
    void clear() { _factories.clear(); }

    // Drops only the factories belonging to one project (keys of the form
    // "<project>::ClassName", per make_script_key/SCRIPT_PROJECT_PREFIX).
    // Used when reloading a single project's hot-reloadable scripts module
    // — unlike clear(), this leaves every OTHER project's registrations
    // intact, so rebuilding game4's scripts can't accidentally unregister
    // game3's. A factory key with no "::" (no project prefix at all) is
    // left alone here too, since it can't belong to any specific project.
    void clear_project(const std::string& project) {
        if (project.empty()) return;
        const std::string prefix = project + "::";
        for (auto it = _factories.begin(); it != _factories.end(); ) {
            if (it->first.compare(0, prefix.size(), prefix) == 0) it = _factories.erase(it);
            else ++it;
        }
    }

    ScriptPtr make(const std::string& name){
        auto try_name = [&](const std::string& key) -> ScriptPtr {
            auto it = _factories.find(key);
            if (it == _factories.end()) return {nullptr, ScriptDeleter{}};
            // A malformed script constructor used to be the one lifecycle
            // path outside ScriptSystem's callback guards: the factory is a
            // raw function pointer into a hot-reload DLL, so an access
            // violation here would take down the whole editor before the
            // instance could be tracked and disabled. Guard just this tiny
            // ABI edge, then report it as an unavailable script for this
            // session. Normal C++ exceptions keep their usual unwinding and
            // are handled below.
            ScriptBase* raw = nullptr;
            try {
#if defined(_WIN32)
                bool access_violation = false;
                raw = _create_guarded(it->second, &access_violation);
                if (access_violation) {
                    Debug::log("[ScriptRegistry] ACCESS VIOLATION constructing " + key + "; script skipped for this Play session");
                    return {nullptr, ScriptDeleter{}};
                }
#else
                raw = it->second.create();
#endif
            } catch (const std::exception& e) {
                Debug::log("[ScriptRegistry] Constructor error in " + key + ": " + e.what());
                return {nullptr, ScriptDeleter{}};
            } catch (...) {
                Debug::log("[ScriptRegistry] Unknown constructor error in " + key);
                return {nullptr, ScriptDeleter{}};
            }
            if (!raw) return {nullptr, ScriptDeleter{}};
            return ScriptPtr(raw, ScriptDeleter{it->second.destroy});
        };

        if (name.find("::") != std::string::npos) {
            return try_name(name);
        }

        std::vector<std::string> candidates;
        for (const auto& n : detail::normalize_script_names(name)) {
            candidates.push_back(detail::make_script_key(_active_project.c_str(), n));
        }
        for (const auto& n : detail::normalize_script_names(name)) {
            candidates.push_back(n);
        }

        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

        for (const auto& key : candidates) {
            if (auto inst = try_name(key)) return inst;
        }

        const std::string suffix = std::string("::") + name;
        std::vector<std::string> matches;
        for (const auto& [k, v] : _factories) {
            if (k == name || (k.size() >= suffix.size() && k.compare(k.size() - suffix.size(), suffix.size(), suffix) == 0))
                matches.push_back(k);
        }
        if (matches.size() == 1) return try_name(matches.front());
        return {nullptr, ScriptDeleter{}};
    }

    bool has(const std::string& name){
        if (name.find("::") != std::string::npos) return _factories.count(name) > 0;
        std::vector<std::string> candidates;
        for (const auto& n : detail::normalize_script_names(name)) {
            candidates.push_back(detail::make_script_key(_active_project.c_str(), n));
        }
        for (const auto& n : detail::normalize_script_names(name)) {
            candidates.push_back(n);
        }
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        for (const auto& key : candidates) if (_factories.count(key)) return true;
        const std::string suffix = std::string("::") + name;
        for (const auto& [k, v] : _factories) {
            if (k == name || (k.size() >= suffix.size() && k.compare(k.size() - suffix.size(), suffix.size(), suffix) == 0))
                return true;
        }
        return false;
    }
    std::vector<std::string> all_names(){
        std::vector<std::string> out;
        for(auto&[k,v]:_factories) out.push_back(k);
        std::sort(out.begin(), out.end());
        return out;
    }
private:
#if defined(_WIN32)
    // Keep __try out of `make()`: MSVC rejects SEH in a function with local
    // C++ objects that need unwinding. We only intercept hard memory faults;
    // compiler-generated C++ exceptions continue to the try/catch above.
    static int _create_fault_filter(unsigned long code) noexcept {
        switch (code) {
            case EXCEPTION_ACCESS_VIOLATION:
            case EXCEPTION_IN_PAGE_ERROR:
            case EXCEPTION_ILLEGAL_INSTRUCTION:
            case EXCEPTION_STACK_OVERFLOW:
                return EXCEPTION_EXECUTE_HANDLER;
            default:
                return EXCEPTION_CONTINUE_SEARCH;
        }
    }
    static ScriptBase* _create_guarded(ScriptFactory factory, bool* access_violation) noexcept {
        if (access_violation) *access_violation = false;
        __try {
            return factory.create ? factory.create() : nullptr;
        } __except(_create_fault_filter(GetExceptionCode())) {
            if (access_violation) *access_violation = true;
            return nullptr;
        }
    }
#endif
    std::string _active_project;
    std::unordered_map<std::string,ScriptFactory> _factories;
};

// ── Deferred registration queue ───────────────────────────────────────────────
// REGISTER_SCRIPT must NOT touch ScriptRegistry::instance() directly when
// compiled into a hot-reloadable module (game_scripts.dll/.so): a DLL does
// not share the host exe's function-local statics, and worse, DLL static
// initializers run automatically during LoadLibrary/dlopen — before the
// host has any chance to tell the DLL which registry to use. Racing the
// host's setup against the loader's own timing is exactly the kind of bug
// that "works" in testing and then silently registers into a throwaway
// registry in release.
//
// So REGISTER_SCRIPT now just appends a (name, factory) pair to a queue
// that's local to whichever binary it was compiled into — no registry
// lookup at all at static-init time. The host then explicitly drains this
// queue into the real ScriptRegistry, at a moment of ITS choosing (right
// after LoadLibrary, inside RegisterAllScripts — see
// script_module_loader.hpp), with explicit project-prefix info already
// baked into the queued name (SCRIPT_PROJECT_PREFIX is applied at queue
// time, same as before).
namespace scriptregistry_detail {
    struct PendingRegistration { std::string name; ScriptFactory factory; };
    inline std::vector<PendingRegistration>& pending_queue() {
        static std::vector<PendingRegistration> q;
        return q;
    }
    // Drains this module's pending queue into `target`. Safe to call once
    // per module load; the queue is empty afterward so a second drain
    // (e.g. accidentally calling RegisterAllScripts twice) is a no-op
    // rather than a duplicate-registration bug.
    inline void drain_into(ScriptRegistry& target, const char* project_override = nullptr) {
        auto& q = pending_queue();
        const std::string project = project_override ? project_override : "";
        for (auto& p : q) {
            std::string key = p.name;
            if (!project.empty()) {
                const std::size_t separator = key.rfind("::");
                const std::string class_name = separator == std::string::npos
                    ? key : key.substr(separator + 2);
                key = detail::make_script_key(project.c_str(), class_name.c_str());
            }
            target.reg(key, p.factory);
        }
        q.clear();
    }
}

// Macro to register a script class by name
// IMPORTANT: `create` and `destroy` lambdas MUST be stateless (no captures)
// so they decay to raw function pointers. The host stores these pointers and
// calls them; if the lambdas had captures, the function-pointer conversion
// would fail at compile time. Both lambdas execute inside the DLL's compiled
// code, so `new`/`delete` use the DLL's operator new/delete — the custom
// ScriptDeleter ensures that `delete` is called through the destroy pointer
// (DLL's operator delete), not the host's, avoiding cross-CRT heap corruption.
#ifndef SCRIPT_PROJECT_PREFIX
#define SCRIPT_PROJECT_PREFIX ""
#endif
#define REGISTER_SCRIPT(ClassName) \
    static bool _reg_##ClassName = []{ \
        ::scriptregistry_detail::pending_queue().push_back({ \
            detail::make_script_key(SCRIPT_PROJECT_PREFIX, #ClassName), \
            ::ScriptFactory{ \
                []() -> ScriptBase* { return new ClassName(); }, \
                [](ScriptBase* p) { delete static_cast<ClassName*>(p); } \
            } \
        }); return true; \
    }()
// ─── ScriptSystem ─────────────────────────────────────────────────────────────
class ScriptSystem {
public:
    // All lifecycle passes must derive script names the same way.  The scene
    // format accepts both the legacy Script component and the editor's
    // ScriptComponent (including its multi-script array).  Previously only
    // update() understood ScriptComponent while fixed/late/gizmo passes only
    // inspected Script.name, producing scripts that were partially alive.
    static std::vector<std::string> script_names_on(const Entity& e) {
        std::vector<std::string> names;
        // Preserve one canonical instance key per attached entry.  The
        // registry itself already tries PascalCase/snake_case variants when
        // resolving a factory; expanding them here created duplicate live
        // controllers for one ScriptComponent entry (for example three menu
        // controllers for "abyss_menu_controller").
        auto add_name = [&](const std::string& raw) {
            if (!raw.empty()) names.push_back(raw);
        };
        for (const char* type : {"Script", "ScriptComponent"}) {
            if (!has_component(e, type)) continue;
            const auto& component = e["components"][type];
            if (component.contains("name") && component["name"].is_string())
                add_name(component["name"].get<std::string>());
            if (component.contains("class_name") && component["class_name"].is_string())
                add_name(component["class_name"].get<std::string>());
            if (component.contains("scripts") && component["scripts"].is_array())
                for (const auto& script : component["scripts"])
                    if (script.is_string()) add_name(script.get<std::string>());
        }
        if (e.contains("scripts") && e["scripts"].is_array())
            for (const auto& script : e["scripts"])
                if (script.is_string()) add_name(script.get<std::string>());
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        return names;
    }

    // Reads Script/ScriptComponent's "field_overrides"[sname] (set by the
    // Inspector, or loaded from a saved scene) and pushes each value into
    // the live instance through its registered EXPOSE_FIELD setter. Safe to
    // call even if the script exposed nothing (no-op) or the component has
    // no overrides yet.
    static void apply_field_overrides(Entity& e, const std::string& sname, ScriptBase* inst) {
        if (!inst) return;
        auto* fields = scriptfields::InstanceRegistry::instance().fields_for(inst);
        if (!fields || fields->empty()) return;

        Entity* overrides = nullptr;
        for (const char* ctype : {"Script", "ScriptComponent"}) {
            if (!has_component(e, ctype)) continue;
            auto& sc = e["components"][ctype];
            if (sc.contains("field_overrides") && sc["field_overrides"].contains(sname)) {
                overrides = &sc["field_overrides"][sname];
                break;
            }
        }
        if (!overrides || !overrides->is_object()) return;

        for (auto& f : *fields) {
            if (overrides->contains(f.name)) {
                try { f.set((*overrides)[f.name]); } catch (...) { /* type mismatch in saved scene — ignore */ }
            }
        }
    }


    void set_input(InputSystem* inp){ _input=inp; Input::Bind(inp); }

    // ── SEH-guarded lifecycle wrappers (Windows) ───────────────────────────
    // These are extracted as static functions so MSVC's C2712 does not fire:
    // __try/__except cannot appear in a function containing C++ objects with
    // non-trivial destructors. The helpers below have NO local C++ objects
    // (all params are raw pointers/trivially-destructible values, and the
    // __except handler only touches bool/int/char[] members). Fault info is
    // propagated via a static trivially-destructible struct.
#if defined(_WIN32)
    struct SehFlag { bool hit = false; };
    inline static SehFlag _seh;

    static void _seh_awake(ScriptBase* inst) {
        __try { inst->awake(); } __except(1) { _seh.hit = true; }
    }
    static void _seh_poll_active(ScriptBase* inst) {
        __try { inst->_poll_active_state(); } __except(1) { _seh.hit = true; }
    }
    static void _seh_tick(ScriptBase* inst, float dt) {
        __try {
            if (!inst->_started) { inst->start(); inst->_started = true; }
            inst->update(dt);
            inst->_tick_coroutines(dt);
            inst->_poll_health_callbacks();
        } __except(1) { _seh.hit = true; }
    }
    static void _seh_unscaled_tick(ScriptBase* inst, float raw_dt) {
        __try { inst->update_unscaled(raw_dt); } __except(1) { _seh.hit = true; }
    }
    static void _seh_fixed(ScriptBase* inst, float dt) {
        __try { inst->fixed_update(dt); } __except(1) { _seh.hit = true; }
    }
    static void _seh_late(ScriptBase* inst, float dt) {
        __try { inst->late_update(dt); } __except(1) { _seh.hit = true; }
    }
    static void _seh_end_of_frame(ScriptBase* inst) {
        __try { inst->_resume_end_of_frame(); } __except(1) { _seh.hit = true; }
    }
    static void _seh_gizmos(ScriptBase* inst) {
        __try { inst->on_draw_gizmos(); } __except(1) { _seh.hit = true; }
    }
    static void _seh_destroy(ScriptBase* inst) {
        __try { inst->on_destroy(); } __except(1) { _seh.hit = true; }
    }
    static void _seh_collision_event(ScriptBase* inst, int kind, Entity* other) {
        __try {
            const EntityRef ref(other);
            if      (kind == 0) inst->on_collision_enter(ref);
            else if (kind == 1) inst->on_collision_stay(ref);
            else if (kind == 2) inst->on_collision_exit(ref);
            else if (kind == 3) inst->on_trigger_enter(ref);
            else if (kind == 4) inst->on_trigger_stay(ref);
            else if (kind == 5) inst->on_trigger_exit(ref);
        } __except(1) { _seh.hit = true; }
    }
#endif

    // Called once per fixed physics tick (1/120 s). Dispatches fixed_update()
    // on every live script instance — mirrors Unity's FixedUpdate() execution
    // order: physics integrator runs AFTER all FixedUpdates in Unity, so here
    // call this before phys::apply_physics() in the game loop.
    void fixed_update(EntityList& entities, float fixed_dt) {
        // A fixed tick is simulation work.  Do not advance physics-facing
        // scripts while a pause owns the time scale; pause menus receive
        // update_unscaled() from update() instead.
        if ((double)Time::time_scale <= 0.00001) return;
        Input::Bind(_input);
        // Scripts are allowed to Instantiate() from any lifecycle callback.
        // Indexing is deliberate: a range-for iterator becomes invalid if a
        // callback grows the EntityList vector.
        for (size_t entity_index = 0; entity_index < entities.size(); ++entity_index) {
            Entity& e = entities[entity_index];
            if (!entity_active(e)) continue;
            int eid = e.value("id",-1);
            for (const auto& sname : script_names_on(e)) {
                auto key = std::make_pair(eid, sname);
                auto it = _instances.find(key);
                if (it == _instances.end() || !it->second || !it->second->_started || _faulted.count(key)) continue;
                it->second->entity = &e;
                ScriptBase::set_current(it->second.get());
#if defined(_WIN32)
                _seh_fixed(it->second.get(), fixed_dt);
                if (_seh.hit) {
                    _seh.hit = false;
                    _faulted.insert(key);
                    Debug::log("[ScriptSys] ACCESS VIOLATION in " + sname + "::fixed_update; disabled for this Play session");
                }
#else
                try { it->second->fixed_update(fixed_dt); }
                catch (...) { _faulted.insert(key); Debug::log("[ScriptSys] Error in " + sname + "::fixed_update; disabled for this Play session"); }
#endif
                ScriptBase::set_current(nullptr);
            }
        }
    }

    // Called after all update()s each frame. Dispatches late_update() on every
    // live script instance — mirrors Unity's LateUpdate() (camera follow, IK, etc.)
    void late_update(EntityList& entities, float dt) {
        if ((double)Time::time_scale <= 0.00001) return;
        Input::Bind(_input);
        for (size_t entity_index = 0; entity_index < entities.size(); ++entity_index) {
            Entity& e = entities[entity_index];
            if (!entity_active(e)) continue;
            int eid = e.value("id",-1);
            for (const auto& sname : script_names_on(e)) {
                auto key = std::make_pair(eid, sname);
                auto it = _instances.find(key);
                if (it == _instances.end() || !it->second || !it->second->_started || _faulted.count(key)) continue;
                it->second->entity = &e;
                ScriptBase::set_current(it->second.get());
#if defined(_WIN32)
                _seh_late(it->second.get(), dt);
                if (_seh.hit) {
                    _seh.hit = false;
                    _faulted.insert(key);
                    Debug::log("[ScriptSys] ACCESS VIOLATION in " + sname + "::late_update; disabled for this Play session");
                }
#else
                try { it->second->late_update(dt); }
                catch (...) { _faulted.insert(key); Debug::log("[ScriptSys] Error in " + sname + "::late_update; disabled for this Play session"); }
#endif
                ScriptBase::set_current(nullptr);
            }
        }
    }

    // Called once per frame right after the render pass finishes (after
    // render.draw() returns, before render.present()/draw_ui — see
    // CO_WAIT_END_OF_FRAME's comment above for exactly why that point).
    // Resumes every coroutine across every script instance that's parked on
    // CO_WAIT_END_OF_FRAME this frame.
    void resume_end_of_frame(EntityList& entities) {
        for (size_t entity_index = 0; entity_index < entities.size(); ++entity_index) {
            Entity& e = entities[entity_index];
            if (!entity_active(e)) continue;
            int eid = e.value("id",-1);
            for (const auto& sname : script_names_on(e)) {
                auto key = std::make_pair(eid, sname);
                auto it = _instances.find(key);
                if (it == _instances.end() || !it->second || !it->second->_started || _faulted.count(key)) continue;
                it->second->entity = &e;
                ScriptBase::set_current(it->second.get());
#if defined(_WIN32)
                _seh_end_of_frame(it->second.get());
                if (_seh.hit) {
                    _seh.hit = false;
                    _faulted.insert(key);
                    Debug::log("[ScriptSys] ACCESS VIOLATION in " + sname + "::end_of_frame; disabled for this Play session");
                }
#else
                try { it->second->_resume_end_of_frame(); }
                catch (...) { _faulted.insert(key); Debug::log("[ScriptSys] Error in " + sname + "::end_of_frame; disabled for this Play session"); }
#endif
                ScriptBase::set_current(nullptr);
            }
        }
    }

    // Collect gizmo draw commands from all scripts that override on_draw_gizmos().
    // Result written into entity["_gizmos"] for the viewport to render as overlays.
    void draw_gizmos(EntityList& entities) {
        for (size_t entity_index = 0; entity_index < entities.size(); ++entity_index) {
            Entity& e = entities[entity_index];
            if (!entity_active(e)) continue;
            int eid = e.value("id",-1);
            for (const auto& sname : script_names_on(e)) {
                auto key = std::make_pair(eid, sname);
                auto it = _instances.find(key);
                if (it == _instances.end() || !it->second || !it->second->_started || _faulted.count(key)) continue;
                it->second->entity = &e;
                ScriptBase::set_current(it->second.get());
                e["_gizmos"] = Entity::array();
#if defined(_WIN32)
                _seh_gizmos(it->second.get());
                if (_seh.hit) {
                    _seh.hit = false;
                    _faulted.insert(key);
                    Debug::log("[ScriptSys] ACCESS VIOLATION in " + sname + "::on_draw_gizmos; disabled for this Play session");
                }
#else
                try { it->second->on_draw_gizmos(); }
                catch (...) { _faulted.insert(key); Debug::log("[ScriptSys] Error in " + sname + "::on_draw_gizmos; disabled for this Play session"); }
#endif
                ScriptBase::set_current(nullptr);
            }
        }
    }

    void update(EntityList& entities, float dt){
        Input::Bind(_input);

        // Build a one-shot id→Entity* map for this frame so all the
        // "find entity by id" loops below are O(1) instead of O(N).
        // Rebuilt here (not cached) because Instantiate() can grow/
        // reallocate `entities` mid-update, invalidating old pointers.
        std::unordered_map<int, Entity*> _eid_map;
        // Script callbacks can add entities. Centralising this rebuild avoids
        // leaving a pointer into the old vector buffer live after any callback.
        Entity* _buffer_start = nullptr;
        auto rebuild_entity_index = [&] {
            _eid_map.clear();
            _eid_map.reserve(entities.size());
            int largest_entity_id = 0;
            for (auto& e : entities) {
                const int entity_id = e.value("id", 0);
                _eid_map[entity_id] = &e;
                largest_entity_id = std::max(largest_entity_id, entity_id);
            }
            // Keep the process-wide runtime allocator ahead of IDs loaded
            // from the current scene. This is deliberately part of the map
            // rebuild so it costs no extra scene traversal and remains true
            // after a graph/script appends new entities.
            reserve_entity_id_through(largest_entity_id);
            _buffer_start = entities.data();
        };
        // A vector relocation invalidates every ScriptBase::entity raw
        // pointer, not just the script currently spawning an object.  The
        // latter used to be repaired while an EventBus callback on another
        // script could still dereference its old pointer and crash during
        // combat/FX-heavy scenes.  Rebind the complete live instance set
        // whenever the entity buffer changes.
        auto rebind_all_instances = [&] {
            for (auto& entry : _instances) {
                ScriptPtr& instance = entry.second;
                if (!instance) continue;
                const auto found = _eid_map.find(entry.first.first);
                instance->entity = found != _eid_map.end() ? EntityRef(found->second) : EntityRef(nullptr);
                instance->all_entities = &entities;
                instance->input = _input;
            }
        };
        auto refresh_entity_index = [&] {
            if (_eid_map.size() == entities.size() && entities.data() == _buffer_start) return false;
            rebuild_entity_index();
            rebind_all_instances();
            return true;
        };
        rebuild_entity_index();
        rebind_all_instances();

        // Instantiate scripts for any entity that has a Script component
        for (size_t _ei = 0; _ei < entities.size(); ++_ei){
            Entity* ep = _eid_map[entities[_ei].value("id", 0)];
            if (!ep || !entity_active(*ep)) continue;
            int eid = ep->value("id",0);

            // One scene entry maps to one ScriptBase instance.  Factory-name
            // normalization belongs inside ScriptRegistry::make(), not here.
            const std::vector<std::string> script_names = script_names_on(*ep);
            if (script_names.empty()) continue;

            // Instantiate if needed
            for (auto& sname : script_names){
                auto key = std::make_pair(eid, sname);
                if (!_instances.count(key)){
                    auto inst = ScriptRegistry::instance().make(sname);
                    if (!inst){
                        if (!_warned.count(key)){
                            _warned.insert(key);
                        }
                        continue;
                    }
                    inst->entity = ep;
                    inst->all_entities = &entities;
                    inst->input = _input;
                    ScriptBase::set_current(inst.get());
#if defined(_WIN32)
                    _seh_awake(inst.get());
                    if (_seh.hit) { _seh.hit = false;
                        Debug::log("[ScriptSys] ACCESS VIOLATION in " + sname + "::awake()"); }
#else
                    try { inst->awake(); } catch (std::exception& ex) {
                        Debug::log("[ScriptSys] Awake error in " + sname + ": " + ex.what());
                    } catch (...) {
                        Debug::log("[ScriptSys] Unknown error in " + sname + "::awake()");
                    }
#endif
                    ScriptBase::set_current(nullptr);
                    // awake() may have called Instantiate() which could have
                    // reallocated entities. If so, rebuild the id map and
                    // re-resolve the entity pointer before apply_field_overrides.
                    refresh_entity_index();
                    ep = _eid_map[eid];
                    if (ep) {
                        apply_field_overrides(*ep, sname, inst.get());
                        inst->entity = ep;
                    }
                    _instances[key] = std::move(inst);
                }
            }
        }

        // Rebuild entity map after instance creation — awake() above may have
        // reallocated the entities buffer, invalidating all prior pointers.
        rebuild_entity_index();

        // Physics queues one event list per entity, while Unity delivers that
        // list to every Script component on the entity. Snapshot it once
        // before invoking any user code: the old per-instance consumption
        // meant whichever script happened to be first in the unordered map
        // swallowed events for all its siblings. The same snapshot is copied
        // to VisualScript so native and graph behaviours observe one event.
        std::unordered_map<int, Entity> pending_events_by_entity;
        pending_events_by_entity.reserve(entities.size());
        for (auto& e : entities) {
            if (!e.contains("_pending_events") || !e["_pending_events"].is_array() ||
                e["_pending_events"].empty()) continue;
            const int eid = e.value("id", 0);
            Entity snapshot = e["_pending_events"].deep_clone();
            pending_events_by_entity.emplace(eid, snapshot);
            e["_pending_events"] = Entity::array();

            Entity graph_events = (e.contains("_pending_graph_events") &&
                                   e["_pending_graph_events"].is_array())
                ? e["_pending_graph_events"].deep_clone() : Entity::array();
            for (const auto& event : snapshot) graph_events.push_back(event);
            e["_pending_graph_events"] = std::move(graph_events);
        }

        // Update all instances
        for (auto& [key, inst] : _instances){
            if (!inst) continue;
            if (_faulted.count(key)) continue;
            auto [eid, sname] = key;

            // Re-bind entity pointer (vector may have reallocated)
            // O(1) lookup via the per-frame id map
            auto _it = _eid_map.find(eid);
            Entity* ep = (_it != _eid_map.end()) ? _it->second : nullptr;
            if (!ep) continue;
            if (ep->value("_destroyed",false)) {
                ScriptBase::set_current(inst.get());
#if defined(_WIN32)
                _seh_destroy(inst.get());
                if (_seh.hit) {
                    _seh.hit = false;
                    Debug::log("[ScriptSys] ACCESS VIOLATION in " + sname + "::on_destroy");
                }
#else
                try { inst->on_destroy(); }
                catch (...) { Debug::log("[ScriptSys] Error in " + sname + "::on_destroy"); }
#endif
                refresh_entity_index();
                ScriptBase::set_current(nullptr);
                continue;
            }

            // Rebind BEFORE the active-gate below and BEFORE the OnEnable/
            // OnDisable poll, since on_enable()/on_disable() (and any script
            // logic they run) need a valid entity/all_entities/input even on
            // the exact frame an entity's "active" flag flips. This also
            // means _poll_active_state() runs even while the entity is
            // inactive, which is the whole point — entity["active"] can be
            // written from the editor Inspector, ObjectPoolSystem, or a
            // ScriptGraph SetActive node on any frame, and OnDisable must
            // still fire for the frame it goes false, not be silently
            // skipped because the rest of this loop body is about to
            // early-continue for exactly that reason.
            inst->entity = ep;
            inst->all_entities = &entities;
            inst->input = _input;
            ScriptBase::set_current(inst.get());
#if defined(_WIN32)
            _seh_poll_active(inst.get());
            if (_seh.hit) { _seh.hit = false;
                _faulted.insert(key);
                Debug::log("[ScriptSys] ACCESS VIOLATION in " + sname + "::on_enable/on_disable; disabled for this Play session");
                ScriptBase::set_current(nullptr);
                continue;
            }
#else
            try { inst->_poll_active_state(); } catch (std::exception& ex) {
                Debug::log("[ScriptSys] _poll_active_state error in " + sname + ": " + ex.what());
            } catch (...) {
                Debug::log("[ScriptSys] Unknown error in " + sname + "::on_enable/on_disable");
            }
#endif

            // on_enable/on_disable are user callbacks too: either can
            // instantiate an entity and relocate the EntityList buffer.
            refresh_entity_index();
            _it = _eid_map.find(eid);
            ep = (_it != _eid_map.end()) ? _it->second : nullptr;
            if (!ep) { ScriptBase::set_current(nullptr); continue; }
            inst->entity = ep;
            if (!entity_active(*ep)) { ScriptBase::set_current(nullptr); continue; }

#if defined(_WIN32)
            apply_field_overrides(*ep, sname, inst.get());
            if ((double)Time::time_scale <= 0.00001) {
                if (inst->update_while_paused()) _seh_unscaled_tick(inst.get(), dt);
            } else {
                _seh_tick(inst.get(), (float)(dt * (double)Time::time_scale));
            }
            if (_seh.hit) { _seh.hit = false;
                _faulted.insert(key);
                Debug::log("[ScriptSys] ACCESS VIOLATION in " + sname +
                           " (eid=" + std::to_string(eid) + "); disabled for this Play session");
                ScriptBase::set_current(nullptr);
                continue;
            }
#else
            try {
                apply_field_overrides(*ep, sname, inst.get());

                if ((double)Time::time_scale <= 0.00001) {
                    if (inst->update_while_paused()) inst->update_unscaled(dt);
                } else {
                    const float scaled_dt = (float)(dt * (double)Time::time_scale);
                    if (!inst->_started){ inst->start(); inst->_started=true; }
                    inst->update(scaled_dt);
                    inst->_tick_coroutines(scaled_dt);
                    inst->_poll_health_callbacks();
                }
            } catch (std::exception& ex) {
                Debug::log("[ScriptSys] Error in " + sname + " (eid=" + std::to_string(eid) + "): " + ex.what());
            } catch (...) {
                Debug::log("[ScriptSys] Unknown error in " + sname + " (eid=" + std::to_string(eid) + ")");
            }
#endif

            // inst->update(dt) may itself have called Instantiate() (fire a
            // bullet, spawn an enemy, etc.), which can grow/reallocate
            // `entities`. If that happened, `ep` above is now dangling —
            // re-resolve it by id before touching it again. (The capacity
            // reserved after load_scene() means this should rarely actually
            // reallocate in practice, but re-resolving here is what makes
            // that safe even when it does, e.g. after a very long session.)
            // Re-resolve after update() in case Instantiate() reallocated entities.
            // Rebuild the map if entities grew OR the buffer was reallocated
            // (same-size reallocation won't change size, so check buffer addr).
            refresh_entity_index();
            _it = _eid_map.find(eid);
            ep = (_it != _eid_map.end()) ? _it->second : nullptr;
            if (!ep) continue;

            // Dispatch pending contact / animation events
            ScriptBase::set_current(inst.get());
            if (auto pending_it = pending_events_by_entity.find(eid);
                pending_it != pending_events_by_entity.end()) {
                // Do not keep a reference into the entity while calling user
                // code. A collision handler can Instantiate(), relocating
                // EntityList and invalidating both `ep` and the event array.
                const Entity& pending_events = pending_it->second;
                // VisualScript received its independent mirror during the
                // pre-dispatch snapshot, before any native callback ran.
                for (const auto& ev : pending_events){
                    std::string method = ev.value("method","");
                    if (method == "on_animation_event") {
                        const std::string event_name = ev.value("event_name", std::string());
                        try { inst->on_animation_event(event_name); }
                        catch (...) { _faulted.insert(key); Debug::log("[ScriptSys] Error in " + sname + "::on_animation_event; disabled for this Play session"); }
                        refresh_entity_index();
                        _it = _eid_map.find(eid);
                        ep = (_it != _eid_map.end()) ? _it->second : nullptr;
                        if (!ep || _faulted.count(key)) break;
                        inst->entity = ep;
                        continue;
                    }
                    int oid = ev.value("other_id",0);
                    Entity* other = nullptr;
                    { auto _oit = _eid_map.find(oid); other = (_oit != _eid_map.end()) ? _oit->second : nullptr; }
#if defined(_WIN32)
                    const int event_kind = method == "on_collision_enter" ? 0 :
                                           method == "on_collision_stay"  ? 1 :
                                           method == "on_collision_exit"  ? 2 :
                                           method == "on_trigger_enter"   ? 3 :
                                           method == "on_trigger_stay"    ? 4 :
                                           method == "on_trigger_exit"    ? 5 : -1;
                    if (event_kind >= 0) {
                        _seh_collision_event(inst.get(), event_kind, other);
                        if (_seh.hit) {
                            _seh.hit = false;
                            _faulted.insert(key);
                            Debug::log("[ScriptSys] ACCESS VIOLATION in " + sname + "::" + method + "; disabled for this Play session");
                        }
                    }
#else
                    try { dispatch_event(*inst, method, other); }
                    catch (...) { _faulted.insert(key); Debug::log("[ScriptSys] Error in " + sname + "::" + method + "; disabled for this Play session"); }
#endif
                    refresh_entity_index();
                    _it = _eid_map.find(eid);
                    ep = (_it != _eid_map.end()) ? _it->second : nullptr;
                    if (!ep || _faulted.count(key)) break;
                    inst->entity = ep;
                }
            }
            ScriptBase::set_current(nullptr);
        }

        // Clean up destroyed instances
        for (auto it = _instances.begin(); it != _instances.end();){
            auto [eid, sname] = it->first;
            auto _dit = _eid_map.find(eid);
            bool dead = (_dit == _eid_map.end() || _dit->second->value("_destroyed", false));
            if (dead) it = _instances.erase(it);
            else ++it;
        }

        // Remove destroyed entities
        entities.erase(std::remove_if(entities.begin(), entities.end(),
            [](const Entity& e){ return e.value("_destroyed",false); }),
            entities.end());
        transform::mark_structure_dirty();
    }

    // Destroys every live script instance and forgets which entities have
    // already been instantiated/warned-about, so the next update() call
    // re-instantiates everything fresh against whatever is currently
    // registered. MUST be called before the scripts module that created
    // these instances is unloaded (FreeLibrary/dlclose) — every instance's
    // vtable lives in that module's code, so calling on_destroy() or the
    // destructor on it AFTER unload is a dangling-pointer call into freed
    // memory. See ScriptModuleLoader::load() in script_module_loader.hpp,
    // which is what actually triggers this during a script rebuild.
    void reset_all_instances() {
        for (auto& [key, inst] : _instances) {
            if (!inst) continue;
            ScriptBase::set_current(inst.get());
#if defined(_WIN32)
            _seh_destroy(inst.get());
            if (_seh.hit) {
                _seh.hit = false;
                Debug::log("[ScriptSys] ACCESS VIOLATION in " + key.second +
                           "::on_destroy while resetting the script runtime");
            }
#else
            try { inst->on_destroy(); }
            catch (...) {
                Debug::log("[ScriptSys] Error in " + key.second +
                           "::on_destroy while resetting the script runtime");
            }
#endif
            ScriptBase::set_current(nullptr);
        }
        _instances.clear();
        _warned.clear();
        _faulted.clear();
    }

private:
    InputSystem* _input = nullptr;

    struct PairHash {
        size_t operator()(const std::pair<int,std::string>& p) const {
            return std::hash<int>()(p.first) ^ (std::hash<std::string>()(p.second)<<16);
        }
    };
    std::unordered_map<std::pair<int,std::string>, ScriptPtr, PairHash> _instances;
    std::unordered_set<std::pair<int,std::string>, PairHash> _warned;
    // A bad user callback must not take down the editor process. Entries are
    // retried only after Play is restarted or scripts have been reloaded.
    std::unordered_set<std::pair<int,std::string>, PairHash> _faulted;

    void dispatch_event(ScriptBase& s, const std::string& method, Entity* other){
        if      (method=="on_collision_enter") s.on_collision_enter(other);
        else if (method=="on_collision_stay")  s.on_collision_stay(other);
        else if (method=="on_collision_exit")  s.on_collision_exit(other);
        else if (method=="on_trigger_enter")   s.on_trigger_enter(other);
        else if (method=="on_trigger_stay")    s.on_trigger_stay(other);
        else if (method=="on_trigger_exit")    s.on_trigger_exit(other);
    }
};
