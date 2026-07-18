#pragma once
#include <SDL2/SDL.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <cmath>
#include <algorithm>
#include <vector>
#include <memory>

// ─── Gamepad support (Unity2D "Input Manager" joystick parity + more) ───────
// Unity's classic Input Manager exposes joystick buttons as "joystick button
// 0".."19" and joystick axes as "X axis".."10th axis". We mirror that with a
// typed enum instead (nicer from script code) while still letting axes/buttons
// bind to a gamepad control by name, same as keys. Up to kMaxPads controllers
// are tracked simultaneously with hot-plug add/remove handling.
enum class GamepadButton {
    South = 0, East, West, North,      // A/B/X/Y (Xbox) ~ Cross/Circle/Square/Triangle (PS)
    LeftBumper, RightBumper,
    LeftTrigger, RightTrigger,         // digital click of the analog triggers
    Back, Start, Guide,
    LeftStick, RightStick,             // stick-click (L3/R3)
    DPadUp, DPadDown, DPadLeft, DPadRight,
    Count
};

enum class GamepadAxis {
    LeftX = 0, LeftY, RightX, RightY, LeftTrigger, RightTrigger, Count
};

struct GamepadState {
    SDL_GameController* handle = nullptr;
    SDL_JoystickID      instance_id = -1;
    SDL_Haptic*          haptic = nullptr;
    std::string          name;
    bool   connected = false;
    std::array<bool, (size_t)GamepadButton::Count> buttons_down{};
    std::array<bool, (size_t)GamepadButton::Count> buttons_pressed{};
    std::array<bool, (size_t)GamepadButton::Count> buttons_released{};
    std::array<float,(size_t)GamepadAxis::Count>   axes{};
    float rumble_time_left = 0.f;
};

inline SDL_GameControllerButton to_sdl_button(GamepadButton b) {
    switch (b) {
        case GamepadButton::South:       return SDL_CONTROLLER_BUTTON_A;
        case GamepadButton::East:        return SDL_CONTROLLER_BUTTON_B;
        case GamepadButton::West:        return SDL_CONTROLLER_BUTTON_X;
        case GamepadButton::North:       return SDL_CONTROLLER_BUTTON_Y;
        case GamepadButton::LeftBumper:  return SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
        case GamepadButton::RightBumper: return SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
        case GamepadButton::Back:        return SDL_CONTROLLER_BUTTON_BACK;
        case GamepadButton::Start:       return SDL_CONTROLLER_BUTTON_START;
        case GamepadButton::Guide:       return SDL_CONTROLLER_BUTTON_GUIDE;
        case GamepadButton::LeftStick:   return SDL_CONTROLLER_BUTTON_LEFTSTICK;
        case GamepadButton::RightStick:  return SDL_CONTROLLER_BUTTON_RIGHTSTICK;
        case GamepadButton::DPadUp:      return SDL_CONTROLLER_BUTTON_DPAD_UP;
        case GamepadButton::DPadDown:    return SDL_CONTROLLER_BUTTON_DPAD_DOWN;
        case GamepadButton::DPadLeft:    return SDL_CONTROLLER_BUTTON_DPAD_LEFT;
        case GamepadButton::DPadRight:   return SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
        default:                         return SDL_CONTROLLER_BUTTON_INVALID;
    }
}

inline SDL_GameControllerAxis to_sdl_axis(GamepadAxis a) {
    switch (a) {
        case GamepadAxis::LeftX:        return SDL_CONTROLLER_AXIS_LEFTX;
        case GamepadAxis::LeftY:        return SDL_CONTROLLER_AXIS_LEFTY;
        case GamepadAxis::RightX:       return SDL_CONTROLLER_AXIS_RIGHTX;
        case GamepadAxis::RightY:       return SDL_CONTROLLER_AXIS_RIGHTY;
        case GamepadAxis::LeftTrigger:  return SDL_CONTROLLER_AXIS_TRIGGERLEFT;
        case GamepadAxis::RightTrigger: return SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
        default:                        return SDL_CONTROLLER_AXIS_INVALID;
    }
}

// ─── Axis config (mirrors _AxisConfig in input_system.py) ───────────────────
struct AxisConfig {
    SDL_Scancode neg_key    = SDL_SCANCODE_UNKNOWN;
    SDL_Scancode pos_key    = SDL_SCANCODE_UNKNOWN;
    SDL_Scancode alt_neg    = SDL_SCANCODE_UNKNOWN;
    SDL_Scancode alt_pos    = SDL_SCANCODE_UNKNOWN;
    float dead_zone    = 0.1f;
    float sensitivity  = 3.0f;
    float gravity      = 3.0f;
    bool  snap         = true;
    float _value       = 0.f;

    // Optional gamepad binding: when use_gamepad is set, this axis also reads
    // a stick/trigger from a connected pad and combines it with the
    // keyboard-driven value — whichever has the larger magnitude wins, so
    // either input device can drive the same logical axis seamlessly.
    bool        use_gamepad     = false;
    GamepadAxis gamepad_axis    = GamepadAxis::LeftX;
    int         gamepad_index   = -1; // -1 = any connected pad
    bool        gamepad_invert  = false;

    float update(const std::unordered_set<SDL_Scancode>& keys, float dt, float gamepad_raw = 0.f) {
        float target = 0.f;
        if (neg_key != SDL_SCANCODE_UNKNOWN && keys.count(neg_key)) target -= 1.f;
        if (pos_key != SDL_SCANCODE_UNKNOWN && keys.count(pos_key)) target += 1.f;
        if (alt_neg != SDL_SCANCODE_UNKNOWN && keys.count(alt_neg)) target -= 1.f;
        if (alt_pos != SDL_SCANCODE_UNKNOWN && keys.count(alt_pos)) target += 1.f;
        target = std::max(-1.f, std::min(1.f, target));

        // Gamepad sticks are already smoothed/deadzoned upstream, so when the
        // stick is pushed further than the keyboard's digital target, let it
        // drive the axis directly instead of running it back through the
        // digital smoothing model below.
        if (use_gamepad && std::abs(gamepad_raw) > std::abs(target)) {
            _value = gamepad_raw;
            return (std::abs(_value) < dead_zone) ? 0.f : _value;
        }

        if (target == 0.f) {
            float step = gravity * dt;
            if (_value > 0) _value = std::max(0.f, _value - step);
            else            _value = std::min(0.f, _value + step);
        } else {
            if (snap && _value * target < 0) _value = 0.f;
            float step = sensitivity * dt;
            _value += (target - _value) * std::min(1.f, step);
            _value = std::max(-1.f, std::min(1.f, _value));
        }
        return (std::abs(_value) < dead_zone) ? 0.f : _value;
    }
};

// ─── InputSystem singleton (mirrors InputSystem class in input_system.py) ───
class InputSystem {
public:
    static constexpr int kMaxPads = 4;

    // Key state
    std::unordered_set<SDL_Scancode> keys_down;
    std::unordered_set<SDL_Scancode> keys_pressed;
    std::unordered_set<SDL_Scancode> keys_released;

    // Mouse state
    std::array<bool,6> mouse_buttons{};
    std::array<bool,6> mouse_down{};
    std::array<bool,6> mouse_up{};
    // SDL events are the source for text and precise edges, but Windows can
    // occasionally drop an event while a Vulkan window is being activated or
    // moved between DPI-aware monitors. Keep a polled snapshot as a safety
    // net so a standalone game never loses all keyboard/mouse control.
    std::array<Uint8, SDL_NUM_SCANCODES> _keyboard_snapshot{};
    std::array<bool,6> _mouse_snapshot{};
    int   mouse_x = 0, mouse_y = 0;
    float mouse_world_x = 0.f, mouse_world_y = 0.f;
    int   mouse_scroll = 0;
    std::string text_input;

    // Axis/button maps
    std::unordered_map<std::string, AxisConfig>               axes;
    std::unordered_map<std::string, std::vector<SDL_Scancode>> buttons;
    // Optional gamepad-button binding layered on top of the keyboard binding
    // above (same logical name, e.g. "Jump" → Space OR pad South button).
    std::unordered_map<std::string, std::vector<GamepadButton>> gamepad_buttons;
    std::unordered_map<std::string, float>                    axis_values;

    // Connected controllers, indexed 0..kMaxPads-1 in connection order — this
    // is the "player index" scripts use (Input.GetButton(name, padIndex) /
    // Input.GetGamepadButton(padIndex, ...)), mirroring Unity's per-player
    // gamepad slots in local multiplayer.
    std::array<GamepadState, kMaxPads> gamepads;
    float gamepad_dead_zone = 0.15f;

    bool quit_requested = false;

    InputSystem() { init_defaults(); init_gamepads(); }
    ~InputSystem() { shutdown_gamepads(); }

    void init_defaults() {
        axes["Horizontal"] = { SDL_SCANCODE_A, SDL_SCANCODE_D, SDL_SCANCODE_LEFT,  SDL_SCANCODE_RIGHT };
        axes["Horizontal"].use_gamepad  = true;
        axes["Horizontal"].gamepad_axis = GamepadAxis::LeftX;
        axes["Vertical"]   = { SDL_SCANCODE_S, SDL_SCANCODE_W, SDL_SCANCODE_DOWN,  SDL_SCANCODE_UP   };
        axes["Vertical"].use_gamepad  = true;
        axes["Vertical"].gamepad_axis = GamepadAxis::LeftY;
        axes["Vertical"].gamepad_invert = true; // stick-down is positive Y on the controller, but "Vertical" follows Unity's screen-up-is-positive convention
        for (auto& [n, _] : axes) axis_values[n] = 0.f;

        buttons["Jump"]     = { SDL_SCANCODE_SPACE };
        buttons["Fire1"]    = { SDL_SCANCODE_LCTRL };
        buttons["Fire2"]    = { SDL_SCANCODE_LALT  };
        buttons["Fire3"]    = { SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT };
        buttons["Submit"]   = { SDL_SCANCODE_RETURN };
        buttons["Cancel"]   = { SDL_SCANCODE_ESCAPE };
        buttons["Interact"] = { SDL_SCANCODE_E };
        // Both physical Shift keys are a single gameplay action.  Treating
        // only LSHIFT as Dash made a perfectly normal keyboard input appear
        // broken in exported builds.
        buttons["Dash"]     = { SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT };

        // Default pad bindings layered onto the same logical names above —
        // a script written purely against GetButton("Jump") gets gamepad
        // support for free with zero changes.
        gamepad_buttons["Jump"]     = { GamepadButton::South };
        gamepad_buttons["Fire1"]    = { GamepadButton::West, GamepadButton::RightTrigger };
        gamepad_buttons["Fire2"]    = { GamepadButton::East };
        gamepad_buttons["Fire3"]    = { GamepadButton::LeftTrigger };
        gamepad_buttons["Submit"]   = { GamepadButton::South, GamepadButton::Start };
        gamepad_buttons["Cancel"]   = { GamepadButton::East, GamepadButton::Back };
        gamepad_buttons["Interact"] = { GamepadButton::North };
        gamepad_buttons["Dash"]     = { GamepadButton::LeftBumper };
    }

    // ── Gamepad lifecycle ───────────────────────────────────────────────────
    // Opens every controller already plugged in at startup. SDL must already
    // have SDL_INIT_GAMECONTROLLER (and ideally SDL_INIT_HAPTIC for rumble)
    // initialized by the caller before constructing InputSystem.
    void init_gamepads() {
        int n = SDL_NumJoysticks();
        for (int i = 0; i < n && i < kMaxPads; ++i) {
            if (SDL_IsGameController(i)) open_gamepad(i);
        }
    }

    void shutdown_gamepads() {
        for (auto& pad : gamepads) close_gamepad(pad);
    }

    int find_pad_slot_by_instance(SDL_JoystickID id) const {
        for (int i = 0; i < kMaxPads; ++i)
            if (gamepads[i].connected && gamepads[i].instance_id == id) return i;
        return -1;
    }

    int find_free_pad_slot() const {
        for (int i = 0; i < kMaxPads; ++i) if (!gamepads[i].connected) return i;
        return -1;
    }

    void open_gamepad(int joystick_index) {
        int slot = find_free_pad_slot();
        if (slot < 0) return; // already at kMaxPads connected controllers
        SDL_GameController* c = SDL_GameControllerOpen(joystick_index);
        if (!c) return;
        GamepadState& pad = gamepads[slot];
        pad.handle      = c;
        pad.connected   = true;
        const char* nm  = SDL_GameControllerName(c);
        pad.name        = nm ? nm : "Gamepad";
        SDL_Joystick* j = SDL_GameControllerGetJoystick(c);
        pad.instance_id = SDL_JoystickInstanceID(j);
        if (SDL_JoystickIsHaptic(j)) {
            pad.haptic = SDL_HapticOpenFromJoystick(j);
            if (pad.haptic && SDL_HapticRumbleSupported(pad.haptic))
                SDL_HapticRumbleInit(pad.haptic);
        }
        pad.buttons_down.fill(false);
        pad.buttons_pressed.fill(false);
        pad.buttons_released.fill(false);
        pad.axes.fill(0.f);
    }

    void close_gamepad(GamepadState& pad) {
        if (pad.haptic) { SDL_HapticClose(pad.haptic); pad.haptic = nullptr; }
        if (pad.handle) { SDL_GameControllerClose(pad.handle); pad.handle = nullptr; }
        pad = GamepadState{};
    }

    void close_gamepad_by_instance(SDL_JoystickID id) {
        int slot = find_pad_slot_by_instance(id);
        if (slot >= 0) close_gamepad(gamepads[slot]);
    }

    void _gamepad_begin_frame() {
        for (auto& pad : gamepads) {
            pad.buttons_pressed.fill(false);
            pad.buttons_released.fill(false);
        }
    }

    void _gamepad_process_event(const SDL_Event& ev) {
        switch (ev.type) {
            case SDL_CONTROLLERDEVICEADDED:
                open_gamepad(ev.cdevice.which);
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                close_gamepad_by_instance(ev.cdevice.which);
                break;
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP: {
                int slot = find_pad_slot_by_instance(ev.cbutton.which);
                if (slot < 0) break;
                for (int b = 0; b < (int)GamepadButton::Count; ++b) {
                    if (to_sdl_button((GamepadButton)b) != (SDL_GameControllerButton)ev.cbutton.button) continue;
                    bool down = (ev.type == SDL_CONTROLLERBUTTONDOWN);
                    GamepadState& pad = gamepads[slot];
                    if (down && !pad.buttons_down[b]) pad.buttons_pressed[b] = true;
                    if (!down && pad.buttons_down[b]) pad.buttons_released[b] = true;
                    pad.buttons_down[b] = down;
                    break;
                }
                break;
            }
            default: break;
        }
    }

    // Polls analog axis values directly (SDL keeps these current without
    // needing an event) and applies the shared deadzone. Call once per frame
    // after events are drained.
    void _gamepad_poll_axes() {
        for (auto& pad : gamepads) {
            if (!pad.connected) continue;
            for (int a = 0; a < (int)GamepadAxis::Count; ++a) {
                Sint16 raw = SDL_GameControllerGetAxis(pad.handle, to_sdl_axis((GamepadAxis)a));
                bool is_trigger = (a == (int)GamepadAxis::LeftTrigger || a == (int)GamepadAxis::RightTrigger);
                float v = is_trigger ? (float)raw / 32767.f : (float)raw / (raw < 0 ? 32768.f : 32767.f);
                if (std::abs(v) < gamepad_dead_zone) v = 0.f;
                pad.axes[a] = std::max(-1.f, std::min(1.f, v));
            }
            // Digital trigger-click buttons aren't always reported as real
            // SDL controller buttons on every pad, so derive them from the
            // analog trigger axis crossing a threshold, matching how most
            // games treat L2/R2 "click" for menu navigation.
            bool lt = pad.axes[(int)GamepadAxis::LeftTrigger]  > 0.5f;
            bool rt = pad.axes[(int)GamepadAxis::RightTrigger] > 0.5f;
            auto edge = [&](GamepadButton b, bool down) {
                int idx = (int)b;
                if (down && !pad.buttons_down[idx]) pad.buttons_pressed[idx] = true;
                if (!down && pad.buttons_down[idx]) pad.buttons_released[idx] = true;
                pad.buttons_down[idx] = down;
            };
            edge(GamepadButton::LeftTrigger,  lt);
            edge(GamepadButton::RightTrigger, rt);
        }
    }

    // Advances rumble timers and stops haptics when the timer expires.
    // Call once per frame with the current delta time (seconds).
    void _gamepad_tick_rumble(float dt) {
        for (auto& pad : gamepads) {
            if (!pad.connected || !pad.haptic) continue;
            if (pad.rumble_time_left > 0.f) {
                pad.rumble_time_left -= dt;
                if (pad.rumble_time_left <= 0.f) {
                    pad.rumble_time_left = 0.f;
                    SDL_HapticRumbleStop(pad.haptic);
                }
            }
        }
    }

    // Reads the combined raw value Axis bindings should blend with the
    // keyboard target for the given logical axis name, across whichever pad
    // index the AxisConfig specifies (or the first connected pad).
    float _gamepad_axis_raw(const AxisConfig& ax) const {
        if (!ax.use_gamepad) return 0.f;
        float best = 0.f;
        for (int i = 0; i < kMaxPads; ++i) {
            if (ax.gamepad_index >= 0 && ax.gamepad_index != i) continue;
            const GamepadState& pad = gamepads[i];
            if (!pad.connected) continue;
            float v = pad.axes[(int)ax.gamepad_axis];
            if (ax.gamepad_invert) v = -v;
            if (std::abs(v) > std::abs(best)) best = v;
            if (ax.gamepad_index >= 0) break;
        }
        return best;
    }

    // ── Event-driven API ────────────────────────────────────────────────────
    // For callers that already own the SDL event loop (e.g. the editor, which
    // must also feed events to Dear ImGui) and can't let InputSystem call
    // SDL_PollEvent itself without stealing events from other consumers.
    // Usage per frame:
    //   input.begin_frame();
    //   while (SDL_PollEvent(&ev)) { ImGui_ImplSDL2_ProcessEvent(&ev); input.process_event(ev); }
    //   input.end_frame(dt);
    void begin_frame() {
        keys_pressed.clear();
        keys_released.clear();
        mouse_down.fill(false);
        mouse_up.fill(false);
        mouse_scroll = 0;
        text_input.clear();
        _gamepad_begin_frame();
    }

    // SDL does not promise to deliver a KEYUP/MOUSEBUTTONUP when focus moves
    // to another application. Leaving those states latched can make a Play
    // session keep moving, firing, or dashing in the background and can feed
    // a surprising amount of work into gameplay systems after focus returns.
    // Clear the local device state immediately on focus loss; the next normal
    // input event repopulates it when the window is active again.
    void clear_focus_lost_input() {
        keys_down.clear();
        keys_pressed.clear();
        keys_released.clear();
        mouse_buttons.fill(false);
        mouse_down.fill(false);
        mouse_up.fill(false);
        _keyboard_snapshot.fill(0);
        _mouse_snapshot.fill(false);
        mouse_scroll = 0;
        text_input.clear();
        for (auto& [name, axis] : axes) {
            axis._value = 0.f;
            axis_values[name] = 0.f;
        }
    }

    void process_event(const SDL_Event& ev) {
        switch (ev.type) {
            case SDL_QUIT:
                quit_requested = true;
                break;
            case SDL_KEYDOWN:
                if (!ev.key.repeat) {
                    keys_down.insert(ev.key.keysym.scancode);
                    keys_pressed.insert(ev.key.keysym.scancode);
                }
                break;
            case SDL_KEYUP:
                keys_down.erase(ev.key.keysym.scancode);
                keys_released.insert(ev.key.keysym.scancode);
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button < 6) {
                    mouse_buttons[ev.button.button] = true;
                    mouse_down[ev.button.button]    = true;
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button < 6) {
                    mouse_buttons[ev.button.button] = false;
                    mouse_up[ev.button.button]      = true;
                }
                break;
            case SDL_MOUSEWHEEL:
                mouse_scroll = ev.wheel.y;
                break;
            case SDL_TEXTINPUT:
                text_input += ev.text.text;
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                    clear_focus_lost_input();
                break;
            case SDL_APP_WILLENTERBACKGROUND:
                clear_focus_lost_input();
                break;
            // Forward all controller events to the gamepad subsystem.
            case SDL_CONTROLLERDEVICEADDED:
            case SDL_CONTROLLERDEVICEREMOVED:
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP:
                _gamepad_process_event(ev);
                break;
        }
    }

    void end_frame(float dt) {
        _sync_polled_device_state();
        _gamepad_poll_axes();
        _gamepad_tick_rumble(dt);
        for (auto& [name, ax] : axes)
            axis_values[name] = ax.update(keys_down, dt, _gamepad_axis_raw(ax));
    }

    // ── Self-contained poll loop (runtime / headless) ────────────────────
    // Returns false when SDL_QUIT received.
    bool poll_events(float dt = 0.016f) {
        keys_pressed.clear();
        keys_released.clear();
        mouse_down.fill(false);
        mouse_up.fill(false);
        mouse_scroll = 0;
        text_input.clear();
        _gamepad_begin_frame();

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) process_event(ev);

        _sync_polled_device_state();
        _gamepad_poll_axes();
        _gamepad_tick_rumble(dt);

        // Update named axes — now passing the gamepad raw value so gamepad
        // sticks actually drive the smoothed axis result.
        for (auto& [name, ax] : axes)
            axis_values[name] = ax.update(keys_down, dt, _gamepad_axis_raw(ax));

        return !quit_requested;
    }

    void set_mouse_world_pos(float wx, float wy) {
        mouse_world_x = wx;
        mouse_world_y = wy;
    }

private:
    // Reconcile event-driven state with SDL's authoritative live device
    // snapshot.  Events still provide the normal edge path; the snapshot only
    // fills gaps, making it safe to use in both standalone and editor-owned
    // event loops without double-triggering an action.
    void _sync_polled_device_state() {
        SDL_PumpEvents();
        int count = 0;
        const Uint8* keys = SDL_GetKeyboardState(&count);
        const int limit = std::min(count, static_cast<int>(SDL_NUM_SCANCODES));
        for (int scancode = 0; scancode < limit; ++scancode) {
            const bool now = keys && keys[scancode] != 0;
            const bool was = _keyboard_snapshot[scancode] != 0;
            if (now) keys_down.insert((SDL_Scancode)scancode);
            else     keys_down.erase((SDL_Scancode)scancode);
            if (now && !was) keys_pressed.insert((SDL_Scancode)scancode);
            if (!now && was) keys_released.insert((SDL_Scancode)scancode);
            _keyboard_snapshot[scancode] = now ? 1 : 0;
        }

        int x = 0, y = 0;
        const Uint32 buttons = SDL_GetMouseState(&x, &y);
        mouse_x = x; mouse_y = y;
        for (int button = 1; button < (int)mouse_buttons.size(); ++button) {
            const bool now = (buttons & SDL_BUTTON(button)) != 0;
            const bool was = _mouse_snapshot[button];
            mouse_buttons[button] = now;
            if (now && !was) mouse_down[button] = true;
            if (!now && was) mouse_up[button] = true;
            _mouse_snapshot[button] = now;
        }
    }

public:

    // ── Query API (matches Python InputSystem class methods) ──────────────
    bool get_key(SDL_Scancode k)      const { return keys_down.count(k) > 0; }
    bool get_key_down(SDL_Scancode k) const { return keys_pressed.count(k) > 0; }
    bool get_key_up(SDL_Scancode k)   const { return keys_released.count(k) > 0; }

    float get_axis(const std::string& name) const {
        auto it = axis_values.find(name);
        return (it != axis_values.end()) ? it->second : 0.f;
    }

    float get_axis_raw(const std::string& name) const {
        auto it = axes.find(name);
        if (it == axes.end()) return 0.f;
        const auto& ax = it->second;
        float v = 0.f;
        if (ax.neg_key != SDL_SCANCODE_UNKNOWN && keys_down.count(ax.neg_key)) v -= 1.f;
        if (ax.pos_key != SDL_SCANCODE_UNKNOWN && keys_down.count(ax.pos_key)) v += 1.f;
        if (ax.alt_neg != SDL_SCANCODE_UNKNOWN && keys_down.count(ax.alt_neg)) v -= 1.f;
        if (ax.alt_pos != SDL_SCANCODE_UNKNOWN && keys_down.count(ax.alt_pos)) v += 1.f;
        // Also include gamepad raw contribution — whichever is larger magnitude.
        if (ax.use_gamepad) {
            float gv = _gamepad_axis_raw(ax);
            if (std::abs(gv) > std::abs(v)) v = gv;
        }
        return std::max(-1.f, std::min(1.f, v));
    }

    // ── Named-button query — keyboard OR gamepad binding ──────────────────
    // Each of get_button / get_button_down / get_button_up checks both the
    // keyboard scancode list and the gamepad_buttons list for the same name,
    // so scripts never need to branch on input device.
    bool get_button(const std::string& name) const {
        auto kit = buttons.find(name);
        if (kit != buttons.end())
            for (auto k : kit->second)
                if (keys_down.count(k)) return true;
        auto git = gamepad_buttons.find(name);
        if (git != gamepad_buttons.end())
            for (const auto& pad : gamepads)
                if (pad.connected)
                    for (auto b : git->second)
                        if (pad.buttons_down[(int)b]) return true;
        return false;
    }

    bool get_button_down(const std::string& name) const {
        auto kit = buttons.find(name);
        if (kit != buttons.end())
            for (auto k : kit->second)
                if (keys_pressed.count(k)) return true;
        auto git = gamepad_buttons.find(name);
        if (git != gamepad_buttons.end())
            for (const auto& pad : gamepads)
                if (pad.connected)
                    for (auto b : git->second)
                        if (pad.buttons_pressed[(int)b]) return true;
        return false;
    }

    bool get_button_up(const std::string& name) const {
        auto kit = buttons.find(name);
        if (kit != buttons.end())
            for (auto k : kit->second)
                if (keys_released.count(k)) return true;
        auto git = gamepad_buttons.find(name);
        if (git != gamepad_buttons.end())
            for (const auto& pad : gamepads)
                if (pad.connected)
                    for (auto b : git->second)
                        if (pad.buttons_released[(int)b]) return true;
        return false;
    }

    // ── Direct gamepad query by pad-index and button/axis ─────────────────
    // For local-multiplayer scripts that need to address a specific player's
    // controller rather than the merged "any pad" result above.
    bool get_gamepad_button(int pad_index, GamepadButton b) const {
        if (pad_index < 0 || pad_index >= kMaxPads) return false;
        const auto& pad = gamepads[pad_index];
        return pad.connected && pad.buttons_down[(int)b];
    }

    bool get_gamepad_button_down(int pad_index, GamepadButton b) const {
        if (pad_index < 0 || pad_index >= kMaxPads) return false;
        const auto& pad = gamepads[pad_index];
        return pad.connected && pad.buttons_pressed[(int)b];
    }

    bool get_gamepad_button_up(int pad_index, GamepadButton b) const {
        if (pad_index < 0 || pad_index >= kMaxPads) return false;
        const auto& pad = gamepads[pad_index];
        return pad.connected && pad.buttons_released[(int)b];
    }

    float get_gamepad_axis(int pad_index, GamepadAxis a) const {
        if (pad_index < 0 || pad_index >= kMaxPads) return 0.f;
        const auto& pad = gamepads[pad_index];
        return pad.connected ? pad.axes[(int)a] : 0.f;
    }

    bool is_gamepad_connected(int pad_index) const {
        return pad_index >= 0 && pad_index < kMaxPads && gamepads[pad_index].connected;
    }

    const std::string& get_gamepad_name(int pad_index) const {
        static const std::string empty;
        if (pad_index < 0 || pad_index >= kMaxPads) return empty;
        return gamepads[pad_index].name;
    }

    int gamepad_count() const {
        int n = 0;
        for (const auto& pad : gamepads) if (pad.connected) ++n;
        return n;
    }

    // ── Rumble / haptic feedback ───────────────────────────────────────────
    // strength is 0..1 (clamped); duration_seconds is wall-clock time.
    // Silently ignored when the pad has no haptic or doesn't support simple
    // rumble — callers should never need to guard against absent hardware.
    void set_rumble(int pad_index, float strength, float duration_seconds) {
        if (pad_index < 0 || pad_index >= kMaxPads) return;
        GamepadState& pad = gamepads[pad_index];
        if (!pad.connected || !pad.haptic) return;
        strength = std::max(0.f, std::min(1.f, strength));
        if (strength <= 0.f) {
            SDL_HapticRumbleStop(pad.haptic);
            pad.rumble_time_left = 0.f;
            return;
        }
        // SDL_HapticRumblePlay takes a duration in milliseconds for its own
        // hardware timer, but we manage the stop ourselves via rumble_time_left
        // so we can be interrupted or overridden next frame. Pass UINT32_MAX
        // (infinite) and let _gamepad_tick_rumble() call Stop at the right time.
        if (SDL_HapticRumblePlay(pad.haptic, strength, SDL_HAPTIC_INFINITY) == 0)
            pad.rumble_time_left = duration_seconds;
    }

    void stop_rumble(int pad_index) {
        if (pad_index < 0 || pad_index >= kMaxPads) return;
        GamepadState& pad = gamepads[pad_index];
        if (pad.haptic) SDL_HapticRumbleStop(pad.haptic);
        pad.rumble_time_left = 0.f;
    }

    // ── Convenience helpers ────────────────────────────────────────────────
    bool get_mouse_button(int btn)      const { return btn >= 0 && btn < 6 && mouse_buttons[btn]; }
    bool get_mouse_button_down(int btn) const { return btn >= 0 && btn < 6 && mouse_down[btn]; }
    bool get_mouse_button_up(int btn)   const { return btn >= 0 && btn < 6 && mouse_up[btn]; }

    bool any_key()      const { return !keys_down.empty(); }
    bool any_key_down() const { return !keys_pressed.empty(); }

    // Returns true if any gamepad button is currently held across all pads.
    bool any_gamepad_button() const {
        for (const auto& pad : gamepads) {
            if (!pad.connected) continue;
            for (bool b : pad.buttons_down) if (b) return true;
        }
        return false;
    }

    // Returns true if any input source (keyboard or gamepad) had activity
    // this frame — mirrors Unity's Input.anyKey / Input.anyKeyDown.
    bool any_input()      const { return any_key()      || any_gamepad_button(); }
    bool any_input_down() const {
        if (any_key_down()) return true;
        for (const auto& pad : gamepads) {
            if (!pad.connected) continue;
            for (bool b : pad.buttons_pressed) if (b) return true;
        }
        return false;
    }

    void register_button(const std::string& name, std::vector<SDL_Scancode> keys) {
        buttons[name] = std::move(keys);
    }

    void register_axis(const std::string& name, AxisConfig cfg) {
        axes[name]       = cfg;
        axis_values[name] = 0.f;
    }

    // Register a gamepad-button binding for a named logical button.
    // Additive: calls stack, they don't replace the keyboard binding.
    void register_gamepad_button(const std::string& name, std::vector<GamepadButton> btns) {
        gamepad_buttons[name] = std::move(btns);
    }
};
