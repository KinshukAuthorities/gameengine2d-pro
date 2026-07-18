#pragma once

// Time singleton — mirrors engine/scripting.py class Time
// Game scripts read Time::dt, Time::elapsed, etc.
//
// Same cross-DLL state-sharing pattern as Screen/Input/SceneManager (see
// unity2d_script_api.hpp's Screen:: comment for the full explanation): this
// used to be plain `static inline` members, which a DLL does NOT share with
// the host exe — the game_scripts_<project> module got its own independent
// copy, permanently stuck at its compiled-in defaults (elapsed_time = 0,
// frame_count = 0) no matter how many frames the host's own Time::update()
// advanced. Any script logic that gates on Time::elapsed_time/frame_count
// (e.g. "destroy this entity once now >= some deadline") would compare
// against a `now` that never moved and so never fire, while everything
// reading per-call dt parameters (those ARE passed in fresh each call, not
// read from a global) kept working — which is why this bug could look like
// only some timers/cleanups were broken while normal gameplay was fine.
struct Time {
    struct State {
        double delta_time       = 0.016;
        double fixed_delta_time = 1.0 / 120.0;
        double elapsed_time     = 0.0;
        double time_scale       = 1.0;
        int    frame_count      = 0;
    };

    static State& _local_state() { static State s; return s; }
    static State*& _state_ptr() { static State* p = nullptr; return p; }

    // Called by a DLL right after load (see RegisterAllScripts) so this
    // module's Time:: calls redirect to the host's real state. The host
    // itself never calls this — its own _state_ptr() stays null, so
    // _state() falls through to _local_state(), which becomes the one true
    // copy every DLL gets pointed at.
    static void bind_state(State* host) { _state_ptr() = host; }
    static State& _state() { return _state_ptr() ? *_state_ptr() : _local_state(); }

    static void update(double raw_dt) {
        auto& s = _state();
        s.delta_time    = raw_dt * s.time_scale;
        s.elapsed_time += s.delta_time;
        ++s.frame_count;
    }

    // Convenience aliases used by some scripts
    static double get_time()  { return _state().elapsed_time; }
    static double get_delta() { return _state().delta_time;   }

    // Property-style access so existing call sites (Time::elapsed_time,
    // Time::delta_time, Time::frame_count, Time::time_scale) keep compiling
    // unchanged across both the host and every DLL.
    struct _ElapsedProxy { operator double() const { return _state().elapsed_time; } };
    struct _DeltaProxy   { operator double() const { return _state().delta_time; } };
    struct _FrameProxy   { operator int()    const { return _state().frame_count; } };
    struct _ScaleProxy {
        operator double() const { return _state().time_scale; }
        _ScaleProxy& operator=(double v) { _state().time_scale = v; return *this; }
    };
    static inline _ElapsedProxy elapsed_time;
    static inline _DeltaProxy   delta_time;
    static inline _FrameProxy   frame_count;
    static inline _ScaleProxy   time_scale;
    static inline double fixed_delta_time = 1.0 / 120.0;
};