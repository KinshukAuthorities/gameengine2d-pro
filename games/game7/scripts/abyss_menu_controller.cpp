#include "../../../engine_cpp/script_system.hpp"
#include "../../../engine_cpp/unity2d_script_api.hpp"
#include "abyss_shared.hpp"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// AbyssMenuController
//
// Reads button clicks via the engine's UIButton on_click / _ui_clicked system
// (set by RenderSystem::draw_ui when a button is hovered+clicked). This works
// correctly both in the editor (where ImGui owns the mouse) and in standalone
// (where SDL owns the mouse). The old manual ButtonClicked() hit-test used
// Input::GetMouseButtonDown which only works in standalone — in the editor
// ImGui intercepts SDL mouse events before the InputSystem sees them, so
// clicks were silently dropped.
// ─────────────────────────────────────────────────────────────────────────────
class AbyssMenuController : public MonoBehaviour {
public:
    void Awake() override {
        AbyssGame::EnsureDefaults();
        ApplyResponsiveMenuLayout();
        UpdateContinueText();
        ApplyScreen(ScreenState::Title);
    }

    void Start() override {
        ApplyResponsiveMenuLayout();
        UpdateContinueText();
        ApplyScreen(ScreenState::Title);
    }

    void Update(float /*dt*/) override {
        // Keyboard-first menu flow for a judge/demo machine with no mouse.
        if (state == ScreenState::Title) {
            if (Input::GetKeyDown(Key::Enter) || Input::GetKeyDown(Key::Space)) {
                StartRun(PlayerPrefs::get_string("abyss_last_scene_path", "scene_home.json"),
                         PlayerPrefs::get_float("abyss_spawn_x", 620.0f),
                         PlayerPrefs::get_float("abyss_spawn_y", 1000.0f),
                         PlayerPrefs::get_string("abyss_spawn_name", "Home Hollow"), false);
                return;
            }
            if (Input::GetKeyDown(Key::N)) {
                StartRun("scene_home.json", 620.0f, 1000.0f, "Home Hollow", true);
                return;
            }
            if (Input::GetKeyDown(Key::S)) { ApplyScreen(ScreenState::Settings); return; }
        } else if (state == ScreenState::Settings && (Input::GetKeyDown(Key::Enter) || Input::GetKeyDown(Key::Escape))) {
            ApplyScreen(ScreenState::Title);
            return;
        }
        if (Input::GetKeyDown(Key::Escape)) {
            if (state == ScreenState::Game || state == ScreenState::Settings) {
                ApplyScreen(ScreenState::Title);
                return;
            }
        }

        if (state == ScreenState::Game) return;

        HandleClicks();
        UpdateContinueText();
        UpdateStatusText();
        UpdateSettingTexts();
    }

private:
    enum class ScreenState { Title, Settings, Game };
    ScreenState state = ScreenState::Title;

    // Menu assets are scene-authored against the same 1280x720 canvas as the
    // gameplay HUD.  Stamp one shared responsive contract on every visual
    // and button component so the renderer, word wrapping, and button
    // hit-testing use identical geometry after a window/viewport resize.
    void ApplyResponsiveMenuLayout() {
        for (auto& node : entities()) {
            const string name = node.value("name", string());
            if (name.rfind("Menu_", 0) != 0 || !node.contains("components")) continue;
            for (auto& [kind, component] : node["components"].items()) {
                if (kind.rfind("UI", 0) != 0 || !component.is_object()) continue;
                component["responsive"] = true;
                component["responsive_fit"] = true;
                component["reference_width"] = 1280;
                component["reference_height"] = 720;
                component["max_scale"] = 1.35f;
                if (kind == "UIText") {
                    component["word_wrap"] = true;
                    component["padding_left"] = 6;
                    component["padding_right"] = 6;
                }
            }
        }
    }

    // ── Click detection via engine on_click / _ui_clicked ────────────────────
    // draw_ui() sets e["_ui_clicked"] = e["components"]["UIButton"]["on_click"]
    // on the frame the button is clicked. We drain it here and clear it so it
    // only fires once per click.
    string ConsumeClick(string entity_name) {
        auto e = Find(entity_name);
        if (!e || !e.Value("active", true)) return "";
        if (!e.Contains("_ui_clicked")) return "";
        string action = e["_ui_clicked"].get<string>();
        e->erase("_ui_clicked");
        return action;
    }

    // Returns the on_click action of any active menu button that was clicked
    // this frame, or empty string if none.
    string DrainAnyClick() {
        static const vector<string> all_buttons = {
            "Menu_Continue","Menu_Start","Menu_Verdant","Menu_Crystal",
            "Menu_Flooded","Menu_Deep","Menu_Boss","Menu_Settings",
            "Menu_NewGame","Menu_Reset","Menu_ScreenShake","Menu_CombatAssist","Menu_Back"
        };
        for (auto n : all_buttons) {
            string action = ConsumeClick(n);
            if (!action.empty()) return action;
        }
        return "";
    }

    void HandleClicks() {
        string action = DrainAnyClick();
        if (action.empty()) return;

        if (state == ScreenState::Title) {
            if (action == "continue") {
                StartRun(PlayerPrefs::get_string("abyss_last_scene_path", "scene_home.json"),
                         PlayerPrefs::get_float("abyss_spawn_x", 620.0f),
                         PlayerPrefs::get_float("abyss_spawn_y", 1000.0f),
                         PlayerPrefs::get_string("abyss_spawn_name", "Home Hollow"),
                         false);
            } else if (action == "start") {
                StartRun("scene_home.json", 620.0f, 1000.0f, "Home Hollow", true);
            } else if (action == "verdant") {
                StartRun("scene_verdant.json", 620.0f, 1000.0f, "Verdant Hollow", true);
            } else if (action == "crystal") {
                StartRun("scene_crystal.json", 620.0f, 1000.0f, "Crystal Hall", true);
            } else if (action == "flooded") {
                StartRun("scene_flooded.json", 620.0f, 1020.0f, "Flooded Ruins", true);
            } else if (action == "deep") {
                StartRun("scene_deep.json", 620.0f, 1045.0f, "Deep Mines", true);
            } else if (action == "boss") {
                StartRun("scene_boss.json", 620.0f, 1015.0f, "Boss Sanctum", true);
            } else if (action == "settings") {
                ApplyScreen(ScreenState::Settings);
            } else if (action == "newgame") {
                PlayerPrefs::delete_all();
                AbyssGame::EnsureDefaults();
                StartRun("scene_home.json", 620.0f, 1000.0f, "Home Hollow", true);
            } else if (action == "reset") {
                PlayerPrefs::delete_all();
                AbyssGame::EnsureDefaults();
                ApplyScreen(ScreenState::Title);
            }
        } else if (state == ScreenState::Settings) {
            if (action == "screenshake") {
                ToggleSetting("abyss_screen_shake");
            } else if (action == "combatassist") {
                ToggleSetting("abyss_combat_assist");
            } else if (action == "back") {
                ApplyScreen(ScreenState::Title);
            } else if (action == "reset") {
                PlayerPrefs::delete_all();
                AbyssGame::EnsureDefaults();
                ApplyScreen(ScreenState::Title);
            }
        }
    }

    // ── Scene / state management ──────────────────────────────────────────────
    void StartRun(string scene_path, float x, float y,
                  string name, bool fresh_run) {
        if (fresh_run) {
            // A fresh run must not inherit a previous run's abilities, map,
            // checkpoint, or cleared boss.  Preserve accessibility choices.
            const int keep_shake = PlayerPrefs::get_int("abyss_screen_shake", 1);
            const int keep_assist = PlayerPrefs::get_int("abyss_combat_assist", 0);
            PlayerPrefs::delete_all();
            AbyssGame::EnsureDefaults();
            PlayerPrefs::set_int("abyss_screen_shake", keep_shake);
            PlayerPrefs::set_int("abyss_combat_assist", keep_assist);
        }
        AbyssGame::SetSpawn(x, y, name);
        PlayerPrefs::set_string("abyss_last_scene_path", scene_path);
        AbyssGame::SetGameplayEnabled(true);
        SceneManager::LoadScene(scene_path);
    }

    void ToggleSetting(string key) {
        bool on = PlayerPrefs::get_int(key, key == "abyss_screen_shake" ? 1 : 0) != 0;
        PlayerPrefs::set_int(key, on ? 0 : 1);
    }

    // ── Visibility management ─────────────────────────────────────────────────
    static bool HasUi(EntityRef e) {
        return AbyssGame::HasUi(e);
    }
    static bool StartsWith(string s, string prefix) {
        return s.rfind(prefix, 0) == 0;
    }

    void SetGameEntitiesActive(bool game_on) {
        for (EntityRef e : entities()) {
            string n = e.value("name", "");
            if (n == "Camera" || n == "AbyssMenuController" || n == "MenuController") continue;
            bool ui = HasUi(e);
            if (ui) {
                if (StartsWith(n, "Menu_"))   e["active"] = !game_on;
                else                          e["active"] = game_on;
            } else {
                e["active"] = game_on;
            }
        }
    }

    void HideAllMenuGroups() {
        static const vector<string> names = {
            "Menu_Backdrop","Menu_BackdropMid","Menu_BackdropFront","Menu_Mist",
            "Menu_Logo","Menu_Title","Menu_Subtitle","Menu_Status",
            "Menu_Continue","Menu_Start","Menu_Verdant","Menu_Crystal",
            "Menu_Flooded","Menu_Deep","Menu_Boss","Menu_Settings",
            "Menu_NewGame","Menu_Reset","Menu_Help",
            "Menu_SettingsPanel","Menu_ScreenShake","Menu_CombatAssist","Menu_Back"
        };
        for (auto n : names)
            if (auto e = Find(n)) e["active"] = false;
    }

    void ShowMenuGroups(bool settings_open) {
        static const vector<string> chrome = {
            "Menu_Backdrop","Menu_BackdropMid","Menu_BackdropFront","Menu_Mist",
            "Menu_Logo","Menu_Title","Menu_Subtitle","Menu_Status","Menu_Help"
        };
        for (auto n : chrome)
            if (auto e = Find(n)) e["active"] = true;

        static const vector<string> main_btns = {
            "Menu_Continue","Menu_Start","Menu_Verdant","Menu_Crystal",
            "Menu_Flooded","Menu_Deep","Menu_Boss","Menu_Settings",
            "Menu_NewGame","Menu_Reset"
        };
        for (auto n : main_btns)
            if (auto e = Find(n)) e["active"] = !settings_open;

        static const vector<string> settings_btns = {
            "Menu_SettingsPanel","Menu_ScreenShake","Menu_CombatAssist","Menu_Back"
        };
        for (auto n : settings_btns)
            if (auto e = Find(n)) e["active"] = settings_open;
    }

    void ApplyScreen(ScreenState next) {
        state = next;
        AbyssGame::SetGameplayEnabled(state == ScreenState::Game);
        if (state == ScreenState::Game) {
            SetGameEntitiesActive(true);
            HideAllMenuGroups();
        } else if (state == ScreenState::Settings) {
            SetGameEntitiesActive(false);
            ShowMenuGroups(true);
        } else {
            SetGameEntitiesActive(false);
            ShowMenuGroups(false);
        }
        Time::time_scale = 1.0f;
    }

    // ── HUD text updates ──────────────────────────────────────────────────────
    void UpdateContinueText() {
        string name = PlayerPrefs::get_string("abyss_spawn_name", "Home Hollow");
        if (auto e = Find("Menu_Continue"))
            if (e.Contains("components") && e["components"].contains("UIButton"))
                e["components"]["UIButton"]["text"] = "Continue: " + name;
    }

    void UpdateStatusText() {
        string status;
        if (state == ScreenState::Title)    status = "Enter: continue  •  N: new run  •  S: settings";
        else if (state == ScreenState::Settings) status = "Settings — tune the run before you begin.";
        else                                status = "Exploring the abyss.";
        if (auto e = Find("Menu_Status"))
            if (e.Contains("components") && e["components"].contains("UIText"))
                e["components"]["UIText"]["text"] = status;
    }

    void UpdateSettingTexts() {
        if (auto e = Find("Menu_ScreenShake"))
            if (e.Contains("components") && e["components"].contains("UIButton")) {
                bool on = AbyssGame::ScreenShake();
                e["components"]["UIButton"]["text"] = string("Screen Shake: ") + (on ? "ON" : "OFF");
            }
        if (auto e = Find("Menu_CombatAssist"))
            if (e.Contains("components") && e["components"].contains("UIButton")) {
                bool on = AbyssGame::CombatAssist();
                e["components"]["UIButton"]["text"] = string("Combat Assist: ") + (on ? "ON" : "OFF");
            }
    }

};
