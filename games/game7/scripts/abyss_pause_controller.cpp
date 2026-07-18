#include "../../../engine_cpp/script_system.hpp"
#include "../../../engine_cpp/unity2d_script_api.hpp"
#include "abyss_shared.hpp"
#include <algorithm>
#include <string>
#include <utility>

// A pause-safe presentation controller.  Gameplay simulation is frozen by
// AbyssGame::SetPaused(), but this behaviour continues through
// UpdateUnscaled so keyboard navigation, accessibility settings, and return
// actions never depend on scaled time.
class AbyssPauseController : public MonoBehaviour {
public:
    bool UpdateWhilePaused() const override { return true; }

    void Start() override { EnsureUi(); }

    void Update(float /*dt*/) override {
        EnsureUi();
        // This is the missing entry point in the previous pause contract:
        // UpdateUnscaled only runs *after* time has been paused, so Escape
        // could never open the menu in the first place.  Handle the first
        // edge on the normal gameplay clock, then the unscaled branch below
        // owns the menu until Resume is selected.
        if (Input::GetKeyDown(Key::Escape)) {
            if (AbyssGame::MapOpen()) AbyssGame::ToggleMap();
            if (AbyssGame::RelicCaseOpen()) AbyssGame::ToggleRelicCase();
            AbyssGame::SetPaused(true);
            Show(true);
            return;
        }
        Show(false);
    }

    void UpdateUnscaled(float /*raw_dt*/) override {
        EnsureUi();
        Show(true);

        if (Input::GetKeyDown(Key::Escape)) {
            if (detail_mode != DetailMode::Dashboard) {
                detail_mode = DetailMode::Dashboard;
                confirm_reset = false;
                return;
            }
            Resume();
            return;
        }
        if (Input::GetKeyDown(Key::Up) || Input::GetKeyDown(Key::W)) {
            selection = (selection + kEntryCount - 1) % kEntryCount;
            detail_mode = DetailMode::Dashboard;
            confirm_reset = false;
        }
        if (Input::GetKeyDown(Key::Down) || Input::GetKeyDown(Key::S)) {
            selection = (selection + 1) % kEntryCount;
            detail_mode = DetailMode::Dashboard;
            confirm_reset = false;
        }
        if (confirm_reset) {
            if (Input::GetKeyDown(Key::Y) || Input::GetKeyDown(Key::Enter)) { ResetProfile(); return; }
            if (Input::GetKeyDown(Key::N)) { confirm_reset = false; return; }
        }
        if (Input::GetKeyDown(Key::Enter) || Input::GetKeyDown(Key::Space)) Activate();
    }

private:
    static constexpr int kEntryCount = 8;
    enum class DetailMode { Dashboard, Controls, Accessibility };

    int selection = 0;
    bool confirm_reset = false;
    bool ui_ready = false;
    DetailMode detail_mode = DetailMode::Dashboard;

    static Entity Color(int r, int g, int b, int a = 255) {
        Entity c = Entity::array(); c.push_back(r); c.push_back(g); c.push_back(b); c.push_back(a); return c;
    }

    EntityRef ByName(const string& name) { return Find(name); }

    int NextId() const {
        int id = 90000;
        for (const auto& e : entities()) id = std::max(id, e.value("id", 0) + 1);
        return id;
    }

    Entity ResponsiveLayout(float anchor_x, float anchor_y, float pivot_x, float pivot_y,
                            float x, float y, float width, float height) const {
        Entity layout = Entity::object();
        layout["anchor_x"] = anchor_x; layout["anchor_y"] = anchor_y;
        layout["pivot_x"] = pivot_x; layout["pivot_y"] = pivot_y;
        layout["pos_x"] = x; layout["pos_y"] = y;
        layout["width"] = width; layout["height"] = height;
        layout["responsive"] = true;
        layout["responsive_fit"] = true;
        layout["reference_width"] = 1280;
        layout["reference_height"] = 720;
        layout["min_scale"] = 0.55;
        layout["max_scale"] = 1.35;
        return layout;
    }

    void AddPanel(const string& name, float x, float y, float width, float height,
                  Entity color, Entity border, int& id, bool responsive = true) {
        Entity e = Entity::object();
        e["id"] = id++; e["name"] = name; e["active"] = false; e["children"] = Entity::array();
        e["components"] = Entity::object();
        e["components"]["Transform"] = {{"x",0.0},{"y",0.0},{"rotation",0.0},{"scale_x",1.0},{"scale_y",1.0}};
        e["components"]["UICanvas"] = Entity::object();
        Entity c = ResponsiveLayout(0.5f, 0.5f, 0.5f, 0.5f, x, y, width, height);
        if (!responsive) c["responsive"] = false;
        c["color"] = color; c["border_color"] = border; c["border_width"] = 2;
        e["components"]["UIPanel"] = std::move(c);
        entities().push_back(std::move(e));
    }

    void AddText(const string& name, float x, float y, float width, float height,
                 int font, Entity color, int& id, const string& align = "left") {
        Entity e = Entity::object();
        e["id"] = id++; e["name"] = name; e["active"] = false; e["children"] = Entity::array();
        e["components"] = Entity::object();
        e["components"]["Transform"] = {{"x",0.0},{"y",0.0},{"rotation",0.0},{"scale_x",1.0},{"scale_y",1.0}};
        e["components"]["UICanvas"] = Entity::object();
        Entity c = ResponsiveLayout(0.5f, 0.5f, 0.5f, 0.5f, x, y, width, height);
        c["text"] = ""; c["font_size"] = font; c["bold"] = true; c["shadow"] = true; c["word_wrap"] = true;
        c["align"] = align; c["v_align"] = "middle"; c["color"] = color;
        e["components"]["UIText"] = std::move(c);
        entities().push_back(std::move(e));
    }

    void EnsureUi() {
        if (ui_ready || ByName("AbyssPausePanel")) { ui_ready = true; return; }
        int id = NextId();

        // The shade is intentionally full-screen and not reference-scaled.
        // The card inside it is responsive; this keeps every edge covered on
        // ultrawide displays and small docked editor viewports alike.
        AddPanel("AbyssPauseShade", 0.0f, 0.0f, 4096.0f, 4096.0f,
                 Color(3,5,12,196), Color(3,5,12,0), id, false);
        AddPanel("AbyssPausePanel", 0.0f, 0.0f, 640.0f, 474.0f,
                 Color(12,17,31,250), Color(218,173,92,238), id);
        AddPanel("AbyssPauseAccent", -286.0f, 0.0f, 7.0f, 394.0f,
                 Color(229,168,70,245), Color(255,220,145,255), id);
        AddPanel("AbyssPauseDivider", 0.0f, -108.0f, 552.0f, 2.0f,
                 Color(83,102,134,220), Color(83,102,134,0), id);
        AddPanel("AbyssPauseMenuSelection", -126.0f, -76.0f, 292.0f, 25.0f,
                 Color(52,77,112,230), Color(121,171,225,240), id);
        AddPanel("AbyssPauseDetailFrame", 180.0f, 20.0f, 220.0f, 220.0f,
                 Color(17,27,47,238), Color(75,105,143,230), id);
        AddPanel("AbyssPauseFooter", 0.0f, 170.0f, 552.0f, 56.0f,
                 Color(18,28,48,225), Color(76,98,132,220), id);

        AddText("AbyssPauseKicker", -30.0f, -188.0f, 490.0f, 22.0f, 12,
                Color(167,190,226), id);
        AddText("AbyssPauseTitle", -30.0f, -158.0f, 490.0f, 34.0f, 27,
                Color(250,224,151), id);
        AddText("AbyssPauseSubTitle", -30.0f, -120.0f, 490.0f, 24.0f, 13,
                Color(168,184,210), id);
        AddText("AbyssPauseMenu", -125.0f, 8.0f, 300.0f, 208.0f, 19,
                Color(234,240,252), id);
        AddText("AbyssPauseDetailTitle", 180.0f, -82.0f, 214.0f, 28.0f, 15,
                Color(247,211,134), id, "center");
        AddText("AbyssPauseDetail", 180.0f, 20.0f, 214.0f, 200.0f, 13,
                Color(190,204,225), id, "left");
        AddText("AbyssPauseFooterText", 0.0f, 170.0f, 510.0f, 42.0f, 13,
                Color(190,205,228), id, "center");
        ui_ready = true;
    }

    void SetActive(const string& name, bool on) {
        if (auto node = ByName(name)) node["active"] = on;
    }

    void SetText(const string& name, const string& text) {
        if (auto node = ByName(name)) {
            if (node.Contains("components") && node["components"].contains("UIText"))
                node["components"]["UIText"]["text"] = text;
        }
    }

    const char* EntryName(int index) const {
        static const char* entries[kEntryCount] = {
            "Resume expedition", "Open discovery map", "Open relic case", "Controls", "Combat assist", "Screen shake", "Return to title", "Reset profile"
        };
        return entries[index];
    }

    string DetailForSelection() const {
        if (confirm_reset)
            return "This permanently erases the local campaign profile.\n\nY / Enter  confirm\nN  cancel";
        if (detail_mode == DetailMode::Controls)
            return "MOVE  WASD / arrows\nJUMP  Space / C\nBLADE  Z\nARC  X (hold to charge)\nDASH  Shift\nPARRY  Q\nFOCUS  E\nINTERACT  F\nMAP  Tab\nRELICS  I";
        if (detail_mode == DetailMode::Accessibility)
            return string("Combat Assist: ") + (AbyssGame::CombatAssist() ? "ON" : "OFF") +
                   "\nScreen Shake: " + (AbyssGame::ScreenShake() ? "ON" : "OFF") +
                   "\n\nAssist adds a little recovery, Focus regeneration, and kinder timing windows.";
        switch (selection) {
            case 0: return "Return to your current room. The game remains paused until you resume.";
            case 1: return "Review discovered regions, current route, and unexplored chambers.";
            case 2: return "Inspect equipped Blade, Arc, Mobility, and Ward relics.";
            case 3: return "Open the full keyboard reference. Escape returns to this dashboard.";
            case 4: return "Toggle forgiving combat timing and recovery. This can be changed any time.";
            case 5: return "Toggle camera impact response while preserving all gameplay feedback.";
            case 6: return "Leave this run and return to the title screen. Your progress is saved.";
            default: return "Erase the local campaign profile after a separate confirmation.";
        }
    }

    void Show(bool on) {
        static const char* nodes[] = {
            "AbyssPauseShade", "AbyssPausePanel", "AbyssPauseAccent", "AbyssPauseDivider", "AbyssPauseMenuSelection", "AbyssPauseDetailFrame", "AbyssPauseFooter",
            "AbyssPauseKicker", "AbyssPauseTitle", "AbyssPauseSubTitle", "AbyssPauseMenu",
            "AbyssPauseDetailTitle", "AbyssPauseDetail", "AbyssPauseFooterText"
        };
        for (const char* node : nodes) SetActive(node, on);
        if (!on) return;

        SetText("AbyssPauseKicker", "ABYSS OF HOLLOWS  /  EXPEDITION PAUSED");
        SetText("AbyssPauseTitle", confirm_reset ? "Reset this profile?" : "Take a breath.");
        SetText("AbyssPauseSubTitle", confirm_reset
            ? "Your map, relics, checkpoints, and cleared rooms will be removed."
            : "Progress is saved at checkpoints and room transitions.");

        string menu;
        for (int i = 0; i < kEntryCount; ++i) {
            const bool selected = i == selection;
            menu += selected ? "  >  " : "     ";
            menu += EntryName(i);
            if (i == 4) menu += AbyssGame::CombatAssist() ? "   [ON]" : "   [OFF]";
            if (i == 5) menu += AbyssGame::ScreenShake() ? "   [ON]" : "   [OFF]";
            menu += "\n";
        }
        if (auto highlight = ByName("AbyssPauseMenuSelection")) {
            // The text block is vertically centered inside its 208px lane;
            // move a real highlighted row behind the selected entry instead
            // of trying to communicate selection using a fragile ASCII arrow.
            const float first_row_y = -76.0f;
            highlight["components"]["UIPanel"]["pos_y"] = first_row_y + selection * 25.0f;
            highlight["components"]["UIPanel"]["color"] = confirm_reset
                ? Color(105, 45, 52, 236) : Color(52, 77, 112, 230);
            highlight["components"]["UIPanel"]["border_color"] = confirm_reset
                ? Color(248, 126, 126, 250) : Color(121, 171, 225, 240);
        }
        SetText("AbyssPauseMenu", menu);
        SetText("AbyssPauseDetailTitle", confirm_reset ? "CONFIRMATION" : (detail_mode == DetailMode::Controls ? "CONTROL REFERENCE" : "EXPEDITION NOTES"));
        SetText("AbyssPauseDetail", DetailForSelection());
        SetText("AbyssPauseFooterText", confirm_reset
            ? "Y / Enter  confirm reset    •    N  keep profile"
            : "W / S or arrows  select     •     Enter  confirm     •     Esc  resume");
    }

    void Resume() {
        confirm_reset = false;
        detail_mode = DetailMode::Dashboard;
        AbyssGame::ClearPauseState();
        Show(false);
    }

    void ResetProfile() {
        PlayerPrefs::delete_all();
        AbyssGame::EnsureDefaults();
        AbyssGame::SetGameplayEnabled(false);
        AbyssGame::ClearPauseState();
        SceneManager::LoadScene("scene.json");
    }

    void Activate() {
        if (confirm_reset) return;
        switch (selection) {
            case 0: Resume(); break;
            case 1:
                AbyssGame::ClearPauseState();
                if (AbyssGame::RelicCaseOpen()) AbyssGame::ToggleRelicCase();
                if (!AbyssGame::MapOpen()) AbyssGame::ToggleMap();
                break;
            case 2:
                AbyssGame::ClearPauseState();
                if (AbyssGame::MapOpen()) AbyssGame::ToggleMap();
                if (!AbyssGame::RelicCaseOpen()) AbyssGame::ToggleRelicCase();
                break;
            case 3: detail_mode = DetailMode::Controls; break;
            case 4:
                PlayerPrefs::set_int("abyss_combat_assist", AbyssGame::CombatAssist() ? 0 : 1);
                detail_mode = DetailMode::Accessibility;
                break;
            case 5:
                PlayerPrefs::set_int("abyss_screen_shake", AbyssGame::ScreenShake() ? 0 : 1);
                detail_mode = DetailMode::Accessibility;
                break;
            case 6:
                AbyssGame::SetGameplayEnabled(false);
                AbyssGame::ClearPauseState();
                SceneManager::LoadScene("scene.json");
                break;
            case 7: confirm_reset = true; break;
        }
    }
};
