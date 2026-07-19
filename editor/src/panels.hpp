#pragma once
/*
 * panels.hpp — All editor panel implementations.
 *
 * Hierarchy, Inspector, Console, Assets use Dear ImGui.
 * Viewport renders the scene into an offscreen vkr::RenderTarget and blits
 * it as an ImGui image via ImGui_ImplVulkan_AddTexture(). Gizmos, the grid,
 * and the rect-select overlay are drawn with ImGui::GetWindowDrawList()
 * directly over that image (screen-space), since there's no more immediate-
 * mode 2D draw API (SDL_RenderDrawLine/SDL_RenderFillRect) once the
 * SDL_Renderer is gone — see _draw_grid / _draw_gizmos / _draw_rect_select_overlay.
 *
 * Matches the Python panels: hierarchy_panel.py, inspector_panel.py,
 * console_panel.py, assets_panel.py, viewport.py
 */

#include "editor_state.hpp"
#include "scene_io.hpp"
#include "../../engine_cpp/prefab_system.hpp"
#include "component_defs.hpp"
#include "component_registry.hpp"
#include "../../engine_cpp/entity.hpp"
#include "../../engine_cpp/transform_system.hpp"
#include "../../engine_cpp/camera.hpp"
#include "../../engine_cpp/render_system_vk.hpp"
#include "../../engine_cpp/material_system.hpp"
#include "../../engine_cpp/physics.hpp"
#include "../../engine_cpp/systems.hpp"
#include "../../engine_cpp/feature_systems.hpp"
#include "../../engine_cpp/script_system.hpp"
#include "../../engine_cpp/script_graph_system.hpp"
#include "../../engine_cpp/crash_reporter.hpp"
#include "../../engine_cpp/net/matchmaking.hpp"
#include "../../engine_cpp/net/network.hpp"
#include "../../engine_cpp/net/player_spawn.hpp"
#include "../../engine_cpp/net/replication.hpp"
#include "../../engine_cpp/net/net_predict.hpp"
#include "script_module_loader.hpp"
#include "auto_hot_reload.hpp"
#include "toolchain_discovery.hpp"

#include "vk_render/vk_renderer_backend.hpp"
#include "vk_render/vk_render_target.hpp"
#include "vk_render/vk_texture.hpp"
#include "../../engine_cpp/third_party/stb_image.h" // asset-thumbnail decoding — see Entry::get() below

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_vulkan.h>
#include <SDL2/SDL.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shellapi.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <string>
#include <vector>
#include <regex>
#include <sstream>
#include <functional>
#include <initializer_list>
#include <thread>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <cstdlib>
#include <chrono>

// Tilemap collider caching is updated from the viewport panel every frame.
// Keep the system in a shared accessor so it is always available even if
// the panel code is compiled in a context where the member instance is not
// visible (for example, after incremental edits that shift the member block).
inline TilemapColliderSystem& viewport_tilemap_sys() {
    static TilemapColliderSystem sys;
    return sys;
}

inline Grid2DSystem& viewport_grid2d_sys() {
    static Grid2DSystem sys;
    return sys;
}

// UILayoutGroup arrangement (Unity Horizontal/Vertical/Grid Layout Group
// equivalent — see feature_systems.hpp) is likewise updated every frame in
// BOTH edit and play mode, same reasoning as the tilemap collider cache
// above: a designer dragging children into/out of a layout group needs to
// see the arrangement update live without having to press Play first.
inline UILayoutSystem& viewport_ui_layout_sys() {
    static UILayoutSystem sys;
    return sys;
}

#include <cstdlib>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <commdlg.h>
#  include <shellapi.h>
#endif

namespace fs = std::filesystem;

// ─── Theme helper ─────────────────────────────────────────────────────────────
// Unity Pro dark theme — calibrated for a VK_FORMAT_B8G8R8A8_SRGB swapchain.
// The GPU auto-applies sRGB gamma when writing to the swapchain, so ImGui
// receives values in LINEAR space. We specify colours as sRGB (what we want
// to see on screen) and convert them to linear here with s2l() before passing
// to ImGui — otherwise every colour gets gamma-expanded and panels look wrong.
// All sRGB values below are chosen to match the reference screenshot exactly.
inline void apply_unity_theme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 0.f;
    s.ChildRounding     = 0.f;
    s.FrameRounding     = 2.f;
    s.PopupRounding     = 2.f;
    s.GrabRounding      = 2.f;
    s.TabRounding       = 2.f;
    s.ScrollbarRounding = 2.f;
    s.WindowBorderSize  = 1.f;
    s.FrameBorderSize   = 0.f;   // no border around input fields — cleaner look
    s.ChildBorderSize   = 0.f;   // kills the dark edge lines on child panels
    s.FramePadding      = {4, 3};
    s.ItemSpacing       = {6, 3};
    s.WindowPadding     = {6, 6};
    s.IndentSpacing     = 14.f;
    s.ScrollbarSize     = 12.f;
    s.GrabMinSize       = 8.f;
    s.TabBorderSize     = 0.f;

    ImVec4* c = s.Colors;
    // sRGB → linear: required because the swapchain is VK_FORMAT_B8G8R8A8_SRGB.
    // The GPU gamma-expands linear→sRGB on output, so ImGui must supply LINEAR.
    auto s2l = [](float v) -> float {
        return (v <= 0.04045f) ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
    };
    auto col = [&](int r, int g, int b, int a = 255) -> ImVec4 {
        return ImVec4(s2l(r/255.f), s2l(g/255.f), s2l(b/255.f), a/255.f);
    };

    // ── Base surfaces — sRGB 70: slightly darker than before, matches clear color
    // All panel backgrounds unified to one tone: zero dark edges anywhere.
    c[ImGuiCol_WindowBg]             = col( 70,  70,  70);
    c[ImGuiCol_ChildBg]              = col( 70,  70,  70);   // MUST match WindowBg exactly
    c[ImGuiCol_PopupBg]              = col( 78,  78,  78);

    // ── Borders — subtle, not harsh black lines ────────────────────────────
    c[ImGuiCol_Border]               = col( 40,  40,  40);
    c[ImGuiCol_BorderShadow]         = col(  0,   0,   0,   0);
    c[ImGuiCol_ModalWindowDimBg]     = col(  0,   0,   0,  80);

    // ── Text — pure white, max contrast ───────────────────────────────────
    c[ImGuiCol_Text]                 = col(255, 255, 255);
    c[ImGuiCol_TextDisabled]         = col(148, 148, 148);

    // ── Input fields — clearly darker inset, visually distinct ────────────
    c[ImGuiCol_FrameBg]              = col( 42,  42,  42);
    c[ImGuiCol_FrameBgHovered]       = col( 52,  52,  52);
    c[ImGuiCol_FrameBgActive]        = col( 60,  60,  60);

    // ── Title bars — match WindowBg exactly so panels feel seamless ───────
    c[ImGuiCol_TitleBg]              = col( 60,  60,  60);
    c[ImGuiCol_TitleBgActive]        = col( 70,  70,  70);   // = WindowBg
    c[ImGuiCol_TitleBgCollapsed]     = col( 52,  52,  52);

    // ── Menu bar ──────────────────────────────────────────────────────────
    c[ImGuiCol_MenuBarBg]            = col( 60,  60,  60);

    // ── Scrollbar ─────────────────────────────────────────────────────────
    c[ImGuiCol_ScrollbarBg]          = col( 55,  55,  55);
    c[ImGuiCol_ScrollbarGrab]        = col( 95,  95,  95);
    c[ImGuiCol_ScrollbarGrabHovered] = col(112, 112, 112);
    c[ImGuiCol_ScrollbarGrabActive]  = col(130, 130, 130);

    // ── Accent (Unity blue) ───────────────────────────────────────────────
    c[ImGuiCol_CheckMark]            = col( 78, 138, 245);
    c[ImGuiCol_SliderGrab]           = col( 78, 138, 245);
    c[ImGuiCol_SliderGrabActive]     = col(100, 158, 255);

    // ── Buttons — slightly lighter than bg, blue on hover ─────────────────
    c[ImGuiCol_Button]               = col( 90,  90,  90);
    c[ImGuiCol_ButtonHovered]        = col( 82, 122, 195);
    c[ImGuiCol_ButtonActive]         = col( 52,  96, 170);

    // ── Headers (hierarchy selection, tree nodes, collapsing) ─────────────
    c[ImGuiCol_Header]               = col( 52,  96, 170,  90);
    c[ImGuiCol_HeaderHovered]        = col( 52,  96, 170, 160);
    c[ImGuiCol_HeaderActive]         = col( 52,  96, 170);

    // ── Separator — subtle, not a harsh black slash ────────────────────────
    c[ImGuiCol_Separator]            = col( 50,  50,  50);
    c[ImGuiCol_SeparatorHovered]     = col( 78, 138, 245);
    c[ImGuiCol_SeparatorActive]      = col( 52,  96, 170);

    // ── Resize grip ───────────────────────────────────────────────────────
    c[ImGuiCol_ResizeGrip]           = col( 90,  90,  90,  50);
    c[ImGuiCol_ResizeGripHovered]    = col( 78, 138, 245, 150);
    c[ImGuiCol_ResizeGripActive]     = col( 52,  96, 170);

    // ── Tabs — inactive slightly darker, active = WindowBg (seamless) ─────
    c[ImGuiCol_Tab]                  = col( 54,  54,  54);
    c[ImGuiCol_TabHovered]           = col( 66, 110, 185);
    c[ImGuiCol_TabActive]            = col( 70,  70,  70);   // = WindowBg
    c[ImGuiCol_TabUnfocused]         = col( 54,  54,  54);
    c[ImGuiCol_TabUnfocusedActive]   = col( 64,  64,  64);

    // ── Docking ───────────────────────────────────────────────────────────
    c[ImGuiCol_DockingPreview]       = col( 78, 138, 245, 150);
    c[ImGuiCol_DockingEmptyBg]       = col( 70,  70,  70);   // = WindowBg — no dark patches

    // ── Table / misc ──────────────────────────────────────────────────────
    c[ImGuiCol_TableHeaderBg]        = col( 60,  60,  60);
    c[ImGuiCol_TableBorderStrong]    = col( 38,  38,  38);
    c[ImGuiCol_TableBorderLight]     = col( 55,  55,  55);
    c[ImGuiCol_TableRowBg]           = col(  0,   0,   0,   0);
    c[ImGuiCol_TableRowBgAlt]        = col(255, 255, 255,   8);
    c[ImGuiCol_TextSelectedBg]       = col( 78, 138, 245,  75);
    c[ImGuiCol_DragDropTarget]       = col( 78, 138, 245);
    c[ImGuiCol_NavHighlight]         = col( 78, 138, 245);
    c[ImGuiCol_NavWindowingHighlight]= col(255, 255, 255, 180);
    c[ImGuiCol_NavWindowingDimBg]    = col(  0,   0,   0,  80);
    c[ImGuiCol_PlotLines]            = col(155, 155, 155);
    c[ImGuiCol_PlotLinesHovered]     = col( 78, 138, 245);
    c[ImGuiCol_PlotHistogram]        = col( 78, 138, 245);
    c[ImGuiCol_PlotHistogramHovered] = col(100, 158, 255);
}

// ─── Script rebuild state (shared across the process, ONE PER PROJECT) ─────
// Unity locks the editor with a "Compiling" overlay while it recompiles
// scripts so the user can't edit/play against a half-built assembly. We do
// the same thing here, but the build itself runs on a background thread —
// std::system() blocks the calling thread, and this engine has no other
// process to hand the work off to, so running it inline on the main/render
// thread would freeze the whole UI (no overlay could even be drawn) for
// however long the compile takes. A background thread + an atomic flag lets
// editor_main.cpp draw a real blocking modal every frame while the actual
// compiler runs underneath it.
//
// This is also what fixes LNK1104 "cannot open editor.exe": that error
// happens because the build was linking directly on top of the .exe this
// very process is currently running from — Windows keeps an executable
// file locked for writing for as long as it's mapped into a running
// process, no matter which thread tries to overwrite it. The fix is to
// never link onto the running binary's path at all: each rebuild now links
// to a fresh, timestamped filename, so the file being written is never the
// one currently loaded in memory. The old exe is deleted by its successor
// after relaunch, once the old process (which held the lock) has actually
// exited.
//
// Keyed by project name (the raw games/<project> folder name): each
// project's build runs independently of every other project's, with its
// own in_progress/done/message/pending_module_path — rebuilding game4
// while game3's build happens to also be running (or already finished, or
// has never been touched) is fully isolated, both in what gets compiled
// (see editor/scripts_module/CMakeLists.txt's SCRIPTS_TARGET_PROJECT) and
// in what UI state reflects it.
struct ScriptRebuildState {
    std::atomic<bool> in_progress{false};
    std::atomic<bool> done{false};
    std::atomic<bool> success{false};
    std::mutex        msg_mutex;
    std::string       message;     // human-readable status, guarded by msg_mutex
    std::thread       worker;

    // Set by the worker thread on a successful build; read+cleared by
    // ViewportPanel::poll_script_rebuild() on the main thread, which is the
    // only place that actually calls ScriptModuleLoader::load() (must run
    // on the same thread that owns _script_sys — see rebuild_scripts_module
    // above for why the worker thread can't do this itself).
    //
    // Two modes:
    //  * pending_module_path — single monolithic DLL (legacy)
    //  * pending_module_dir  — directory of per-script DLLs (fast rebuild)
    std::mutex   module_path_mutex;
    fs::path     pending_module_path;
    fs::path     pending_module_dir;
    std::atomic<bool> old_modules_unloaded{false}; // set by poll_script_rebuild when unloading before build

    void set_message(const std::string& m) {
        std::lock_guard<std::mutex> lk(msg_mutex);
        message = m;
    }
    std::string get_message() {
        std::lock_guard<std::mutex> lk(msg_mutex);
        return message;
    }
    void set_pending_module_path(const fs::path& p) {
        std::lock_guard<std::mutex> lk(module_path_mutex);
        pending_module_path = p;
    }
    // Returns the pending path and clears it, so a second poll in the same
    // frame (or a stale leftover from a previous cycle) can't trigger a
    // second load.
    fs::path take_pending_module_path() {
        std::lock_guard<std::mutex> lk(module_path_mutex);
        fs::path p = pending_module_path;
        pending_module_path.clear();
        return p;
    }
    void set_pending_module_dir(const fs::path& d) {
        std::lock_guard<std::mutex> lk(module_path_mutex);
        pending_module_dir = d;
    }
    fs::path take_pending_module_dir() {
        std::lock_guard<std::mutex> lk(module_path_mutex);
        fs::path d = pending_module_dir;
        pending_module_dir.clear();
        return d;
    }
    ~ScriptRebuildState() {
        if (worker.joinable()) worker.join();
    }
};

// Standalone export uses the same contract as script compilation: the heavy
// compiler work lives on a worker thread so ImGui can continue presenting a
// modal, but the editor itself is locked until that work reaches a terminal
// state.  Keeping this state outside ViewportPanel lets editor_main render the
// blocking modal after every panel, exactly as it does for script rebuilds.
struct StandaloneBuildState {
    std::atomic<bool> in_progress{false};
    std::atomic<bool> done{false};
    std::atomic<bool> success{false};
    std::atomic<uint64_t> generation{0};
    std::mutex        msg_mutex;
    std::string       message;
    std::string       project;

    void set_status(const std::string& new_project, const std::string& new_message) {
        std::lock_guard<std::mutex> lk(msg_mutex);
        project = new_project;
        message = new_message;
    }
    void set_message(const std::string& new_message) {
        std::lock_guard<std::mutex> lk(msg_mutex);
        message = new_message;
    }
    std::string get_message() {
        std::lock_guard<std::mutex> lk(msg_mutex);
        return message;
    }
    std::string get_project() {
        std::lock_guard<std::mutex> lk(msg_mutex);
        return project;
    }
};

inline StandaloneBuildState& standalone_build_state() {
    static StandaloneBuildState state;
    return state;
}

// One ScriptRebuildState per project, created on first access. A
// std::unordered_map of a non-movable type (std::mutex, std::thread members
// make ScriptRebuildState non-movable/non-copyable) works fine here because
// we only ever return references into the map and never erase/rehash-cause
// relocation of existing entries — unordered_map nodes are stable across
// insertion of NEW keys.
inline std::unordered_map<std::string, std::unique_ptr<ScriptRebuildState>>& _all_script_rebuild_states() {
    static std::unordered_map<std::string, std::unique_ptr<ScriptRebuildState>> states;
    return states;
}
inline ScriptRebuildState& script_rebuild_state(const std::string& project) {
    auto& states = _all_script_rebuild_states();
    auto it = states.find(project);
    if (it == states.end()) {
        it = states.emplace(project, std::make_unique<ScriptRebuildState>()).first;
    }
    return *it->second;
}

// The main loop owns the Console callback and panel data referenced while a
// rebuild runs. Join all workers before that state begins teardown; detached
// workers used to race a late status update against destructed editor state.
inline void shutdown_script_rebuild_workers() {
    for (auto& [project, state] : _all_script_rebuild_states()) {
        (void)project;
        if (state && state->worker.joinable()) state->worker.join();
    }
}

inline fs::path find_engine_project_root() {
    // Since editor_main.cpp already chdir'd to the project root before any
    // panel code runs, the current working directory IS the project root.
    // We verify this and walk up as a safety net.
    fs::path cur = fs::current_path();
    for (int i = 0; i < 10 && !cur.empty(); ++i) {
        if (fs::exists(cur / "engine_cpp" / "CMakeLists.txt") &&
            fs::exists(cur / "editor" / "CMakeLists.txt")) {
            return cur;
        }
        cur = cur.parent_path();
    }
    return fs::current_path();
}

inline fs::path find_built_exe_named(const fs::path& build_dir, const std::string& stem) {
    std::error_code ec;
    if (!fs::exists(build_dir, ec)) return {};
#if defined(_WIN32)
    const std::string wanted = stem + ".exe";
#else
    const std::string wanted = stem;
#endif
    for (auto it = fs::recursive_directory_iterator(build_dir, ec); !ec && it != fs::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file(ec)) continue;
        if (it->path().filename().string() == wanted) return it->path();
    }
    return {};
}

// Mirrors editor/scripts_module/CMakeLists.txt's `_ns` derivation EXACTLY:
// non-alphanumeric characters become '_', and a leading digit gets a
// "project_" prefix (CMake target names and cache variable names can't
// start with a digit the same way C identifiers can't). This is the
// namespace used in target names (game_scripts_<ns>) and cache variable
// names (SCRIPTS_MODULE_OUTPUT_NAME_<ns>) — NOT the same string as the raw
// project folder name, which is what ScriptRegistry/SCRIPT_PROJECT_PREFIX
// and ScriptModuleLoader use as their key. Both are derived from the same
// project folder name but serve different purposes, so callers need to be
// careful which one a given API expects.
inline std::string _cmake_namespace_for_project(const std::string& project) {
    std::string ns;
    ns.reserve(project.size());
    for (char c : project) {
        ns.push_back((std::isalnum((unsigned char)c) || c == '_') ? c : '_');
    }
    if (!ns.empty() && std::isdigit((unsigned char)ns[0])) {
        ns = "project_" + ns;
    }
    return ns;
}

// Seconds the script set must be unchanged before the auto-trigger (see
// AssetsPanel::draw) fires a rebuild on its own — long enough that a text
// editor's autosave or an OS file-copy has finished writing, short enough
// that it still feels immediate.
constexpr double AUTO_REBUILD_DEBOUNCE_SECONDS = 1.5;

// Extracts every class name in `text` that derives from ScriptBase (or one
// of its aliases) — i.e. every class a REGISTER_SCRIPT(...) would be
// emitted for. Shared by _write_script_header (which actually emits the
// registrations), the "browse for script" file picker (which needs the
// class name to reference an existing script), and
// _script_file_is_registered (which uses it to check whether a given
// .cpp's classes are already in ScriptRegistry).
inline std::vector<std::string> extract_script_class_names(const std::string& text) {
    std::vector<std::string> names;
    static const std::regex re(
        R"(class[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]*:[ \t]*public[ \t]+(?:ScriptBase|MonoBehaviour|Behaviour2D|Behaviour))");
    for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) {
        if (it->size() > 1) names.push_back((*it)[1].str());
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

// True if `cpp_path` defines at least one ScriptBase-derived class AND
// every such class is already present in the live ScriptRegistry UNDER
// `project` — i.e. this file's code is truly compiled into project's
// currently-loaded module, not just a same-named stub sitting on disk (or,
// now that modules are per-project, registered under some OTHER project
// entirely, which would be a bug if it ever happened but is worth being
// explicit isn't what this checks — it specifically checks the
// "<project>::ClassName" key). A file with zero recognized classes (e.g. a
// stray .cpp, or one that doesn't parse with our regex) is treated as NOT
// registered, so it's surfaced for a rebuild rather than silently ignored —
// better to rebuild once unnecessarily than to miss a real script.
inline bool _script_file_is_registered(const std::string& project, const fs::path& cpp_path) {
    std::ifstream in(cpp_path);
    if (!in) return false;
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto classes = extract_script_class_names(src);
    if (classes.empty()) return false;
    for (auto& cls : classes) {
        if (!ScriptRegistry::instance().has(project + "::" + cls)) return false;
    }
    return true;
}

// Set to true by ViewportPanel::_load_initial_scripts_modules() once it
// has finished loading (or attempting to load) every project's pre-built
// module. The AssetsPanel latch gates on this before running
// _script_project_needs_initial_rebuild() — without it, the latch check
// fires on the very first draw() call, before the ScriptRegistry has been
// populated, so every class looks unregistered and a spurious rebuild
// kicks off even though the module is about to be (or was just) loaded.
inline bool& _initial_modules_loaded() {
    static bool flag = false;
    return flag;
}

// One-shot per-project auto-rebuild latch. Deliberately replaces a more
// elaborate "track every path, debounce, compare known-vs-seen every frame"
// staleness tracker that kept finding ways to re-fire mid-build no matter
// how its timing was patched (it re-derives state from live filesystem +
// registry status on every single frame, which cannot be made to agree
// with a build that itself takes tens of seconds to finish). This is
// intentionally dumb: check once, attempt at most once, never again
// automatically. A user can always force another rebuild explicitly via
// the toolbar button or Play.
struct AutoRebuildLatch { bool attempted = false; };
inline AutoRebuildLatch& _auto_rebuild_latch(const std::string& project) {
    static std::unordered_map<std::string, AutoRebuildLatch> latches;
    return latches[project];
}

// Every *.cpp under ONE project's games/<project>/scripts/ folder. Shared
// by the per-project staleness check and by mark_scripts_registered() so
// both agree on exactly what "all of this project's current script files"
// means.
inline std::unordered_set<std::string> _collect_project_script_cpp_paths(const std::string& project) {
    fs::path root = find_engine_project_root();
    fs::path scripts_dir = root / "games" / project / "scripts";
    std::error_code ec;
    std::unordered_set<std::string> out;
    if (fs::exists(scripts_dir, ec) && fs::is_directory(scripts_dir, ec)) {
        for (auto sit = fs::directory_iterator(scripts_dir, ec); !ec && sit != fs::directory_iterator(); ++sit) {
            if (sit->path().extension() != ".cpp") continue;
            out.insert(sit->path().string());
        }
    }
    return out;
}

// True if ANY script file under `project`'s scripts/ folder has at least
// one ScriptBase-derived class not yet present in the live ScriptRegistry.
// Pure read — no mutation, no debounce state, callable as many times as
// wanted with no side effects (unlike the old scripts_changed_since_build).
inline bool _script_project_needs_initial_rebuild(const std::string& project) {
    for (auto& path : _collect_project_script_cpp_paths(project)) {
        if (!_script_file_is_registered(project, path)) return true;
    }
    return false;
}

// Every project name (raw games/<project> folder name) that has a
// scripts/ subfolder — used by code that needs to act on "every project",
// like establishing the staleness baseline for all of them at startup.
inline std::vector<std::string> _all_script_project_names() {
    fs::path root = find_engine_project_root();
    fs::path games_dir = root / "games";
    std::error_code ec;
    std::vector<std::string> out;
    if (fs::exists(games_dir, ec)) {
        for (auto pit = fs::directory_iterator(games_dir, ec); !ec && pit != fs::directory_iterator(); ++pit) {
            if (!pit->is_directory(ec)) continue;
            std::error_code ec2;
            if (fs::exists(pit->path() / "scripts", ec2) && fs::is_directory(pit->path() / "scripts", ec2)) {
                out.push_back(pit->path().filename().string());
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Tracks which script-source files this process has already seen
// registered for ONE project (i.e. included in a successful rebuild of
// THAT project's module), shared across every caller for that project
// (Play, the toolbar button, the Assets panel auto-trigger) so they all
// agree on what "needs a rebuild" means relative to what THIS running
// editor binary actually has compiled in for that project — not relative
// to each other, and not relative to any OTHER project's state.
//
// Deliberately keyed on PATH ONLY, not mtime: editing the body of a script
// that's already registered is not a reason to auto-rebuild (that used to
// be the behavior, and meant every save triggered a full module rebuild +
// hot-reload even when nothing new was being registered). Auto-rebuild
// should fire only for a script file the editor hasn't registered yet —
// freshly created, dropped in from the OS, or renamed. An already-known
// path is left alone here even if its contents changed; the user can still
// force a rebuild any time via the "Rebuild Scripts" button or Play, which
// always pick up on-disk edits regardless of this tracker.
struct ScriptStalenessTracker {
    std::unordered_set<std::string> known_paths;
    std::unordered_set<std::string> observed_new_paths;
    bool pending_change = false;
    std::chrono::steady_clock::time_point last_change_seen_at{};
    bool baseline_established = false;
    // Set right after kicking off an auto-rebuild that then FAILS (configure
    // or compile error — see poll_script_rebuild). Without this, a script
    // with a genuine compile error retriggers the auto-rebuild every single
    // AUTO_REBUILD_DEBOUNCE_SECONDS forever: the failed build never advances
    // known_paths (only a successful one does, via mark_scripts_registered),
    // so the file looks perpetually "new" and scripts_ready_for_auto_rebuild
    // keeps firing. This timestamp makes scripts_ready_for_auto_rebuild back
    // off for a while after a failure instead of hammering the compiler
    // every few seconds with the same broken script.
    std::chrono::steady_clock::time_point last_failed_at{};
    bool has_failed_once = false;
    // Stamped every time poll_script_rebuild finishes handling a build for
    // this project (success OR failure). A rebuild can take many seconds;
    // scripts_changed_since_build() runs every single frame the whole time
    // and keeps re-observing the same still-uncompiled paths, which is fine
    // while in_progress is true (the trigger is gated on that). But the
    // instant in_progress flips false post-build, idle time since
    // last_change_seen_at is already large (it's been sitting unchanged for
    // the whole build), so scripts_ready_for_auto_rebuild can return true on
    // the very next frame — re-triggering for files that were JUST handled,
    // before this project's own bookkeeping (known_paths / mark_scripts_
    // registered) has had a chance to be the thing callers actually observe.
    // This grace period forces at least a brief pause after any reload
    // before auto-rebuild is allowed to fire again for this project.
    std::chrono::steady_clock::time_point last_reload_handled_at{};
    bool has_reloaded_once = false;
    // Must be called once a rebuild has actually been kicked off for the
    // current pending change, otherwise scripts_ready_for_auto_rebuild()
    // keeps returning true forever and AssetsPanel::draw() re-triggers a
    // rebuild on every frame where in_progress happens to be false.
    void clear_pending() { pending_change = false; observed_new_paths.clear(); }
};
// One tracker per project, so editing game4's scripts can never reset
// game3's debounce timer or staleness state — each project's "is anything
// new" question is answered purely from that project's own scripts/
// folder and that project's own ScriptRegistry entries.
inline ScriptStalenessTracker& script_staleness_tracker(const std::string& project) {
    static std::unordered_map<std::string, ScriptStalenessTracker> trackers;
    return trackers[project];
}

// How long to back off retrying an auto-rebuild after it fails, before
// trying again on its own. Long enough that it isn't spamming the compiler
// (and the Console) every ~debounce-interval forever on a genuine compile
// error; short enough that a transient failure (e.g. file briefly locked
// by another process) self-heals without requiring the user to do anything.
constexpr double AUTO_REBUILD_FAILURE_COOLDOWN_SECONDS = 30.0;
constexpr double AUTO_REBUILD_POST_RELOAD_GRACE_SECONDS = 3.0;

// Minimum time after ANY rebuild (success or failure) finishes for a
// project before auto-rebuild is allowed to fire again for that SAME
// project. Guards against scripts_changed_since_build()'s debounce clock
// having gone stale during a long build (it only resets when a path is
// first newly observed, which can be the moment the build starts) — without
// this, the instant in_progress flips false the idle time can already
// exceed AUTO_REBUILD_DEBOUNCE_SECONDS, re-firing immediately for the same
// files that were just compiled in, before that project's own known_paths
// bookkeeping is what every caller is actually relying on to settle.

// Lightweight check: does `project`'s scripts/ folder contain a *.cpp file
// this process hasn't already registered for THAT project? Cheap enough to
// call every frame — just a directory listing + set lookup, no file reads,
// no mtime comparisons (so editing an already-registered script's contents
// does NOT count as "changed" here — only a script file the editor has
// never seen registered before does). Also stamps last_change_seen_at so
// callers can debounce (wait for writes to settle before auto-triggering a
// rebuild on their own) — but only on the frame a new path is FIRST
// observed, not on every subsequent frame it's still pending (otherwise
// the debounce timer would keep getting pushed back forever and never
// actually elapse, since known_paths isn't advanced until a rebuild
// succeeds — see mark_scripts_registered() below).
inline bool scripts_changed_since_build(const std::string& project) {
    auto& t = script_staleness_tracker(project);
    auto seen = _collect_project_script_cpp_paths(project);

    // The very first call for this project establishes its baseline — but
    // it must check what's ACTUALLY registered in the live ScriptRegistry
    // under this project's key prefix (populated by
    // _load_initial_scripts_module() before this first call ever runs for
    // a given project), not just assume every file on disk is already
    // built in. A script created while the editor was closed (or copied
    // in, or pulled from version control) sits on disk identically to one
    // that really is compiled into the loaded module — the only way to
    // tell them apart is to ask the registry whether its class is
    // actually there.
    if (!t.baseline_established) {
        std::unordered_set<std::string> known;
        bool any_unregistered = false;
        for (auto& path : seen) {
            if (_script_file_is_registered(project, path)) {
                known.insert(path);
            } else {
                any_unregistered = true;
            }
        }
        t.known_paths = std::move(known);
        t.baseline_established = true;
        t.observed_new_paths.clear();
        if (any_unregistered) {
            t.pending_change = true;
            t.last_change_seen_at = std::chrono::steady_clock::now();
            for (auto& path : seen) {
                if (!t.known_paths.count(path)) t.observed_new_paths.insert(path);
            }
        }
        return t.pending_change;
    }

    std::unordered_set<std::string> new_paths;
    for (auto& path : seen) {
        if (!t.known_paths.count(path)) new_paths.insert(path);
    }

    if (new_paths.empty()) {
        // Nothing in `seen` is missing from known_paths anymore — e.g.
        // mark_scripts_registered() just ran after a successful rebuild.
        // This MUST clear pending_change, or it stays stuck true forever:
        // it was set true on some earlier frame (while a path was still
        // unregistered) and nothing else in this function ever clears it
        // back to false. Without this, scripts_ready_for_auto_rebuild()
        // keeps reporting "ready" indefinitely after every reload, even
        // though every script is genuinely already registered — which is
        // exactly the "rebuilds forever, every file flagged, nothing
        // actually changed" loop.
        t.pending_change = false;
        t.observed_new_paths.clear();
        return false;
    }

    t.pending_change = true;
    // Only reset the debounce clock if this is a path we haven't
    // already started waiting on — a brand new file (or an additional
    // one added while already debouncing) extends the wait, but
    // re-seeing the same still-unregistered file on every frame must
    // NOT keep pushing the timer back.
    bool any_truly_new = false;
    for (auto& p : new_paths) {
        if (!t.observed_new_paths.count(p)) { any_truly_new = true; break; }
    }
    if (any_truly_new) {
        t.last_change_seen_at = std::chrono::steady_clock::now();
    }
    t.observed_new_paths = std::move(new_paths);
    // Note: known_paths is intentionally NOT advanced to include the new
    // file here. It's only advanced once a rebuild actually succeeds (see
    // mark_scripts_registered() below, called from poll_script_rebuild),
    // so a script that's new but not yet compiled in keeps correctly
    // reporting "needs a rebuild" on every check until it really is one.
    return t.pending_change;
}

// Called once `project`'s rebuild has actually succeeded and its new
// module is loaded, so that project's staleness tracker's notion of
// "already registered" stays in sync with what's truly compiled in.
// Without this, a script file would stay marked "new" forever after being
// created (known_paths is deliberately not advanced by
// scripts_changed_since_build() itself), and every subsequent check would
// needlessly re-trigger a rebuild even though the file is now actually
// registered.
inline void mark_scripts_registered(const std::string& project) {
    auto& t = script_staleness_tracker(project);
    t.known_paths = _collect_project_script_cpp_paths(project);
    t.observed_new_paths.clear();
    t.baseline_established = true;
}

// Read-only variant for display purposes (toolbar button color/label/
// tooltip): true if `project` has any script file not in known_paths yet,
// with NO side effects on that project's tracker pending/debounce state.
// Deliberately separate from scripts_changed_since_build() — that function
// is the single place allowed to mutate pending_change/
// last_change_seen_at/observed_new_paths, since multiple call sites invoke
// it every frame for different reasons (the actual auto-rebuild decision
// in AssetsPanel::draw(), and previously also the toolbar's cosmetic
// "stale" label) and having more than one of them mutate shared debounce
// state caused the auto-rebuild logic to misfire — re-arming or
// double-firing depending on call order within the same frame.
inline bool scripts_are_stale(const std::string& project) {
    auto& t = script_staleness_tracker(project);
    if (!t.baseline_established) return false; // baseline not established yet; nothing to compare against
    auto seen = _collect_project_script_cpp_paths(project);
    for (auto& path : seen) {
        if (!t.known_paths.count(path)) return true;
    }
    return false;
}

// True once `project`'s script set has been stale (per
// scripts_changed_since_build) AND stable/unwritten for
// AUTO_REBUILD_DEBOUNCE_SECONDS — the signal AssetsPanel::draw uses to
// kick off a rebuild of THAT project on its own, with no button press
// required, regardless of whether the script was created in-editor or
// dropped in from the OS file browser. Other projects' debounce timers are
// completely unaffected.
inline bool scripts_ready_for_auto_rebuild(const std::string& project) {
    auto& t = script_staleness_tracker(project);
    if (!t.pending_change) return false;
    double idle = std::chrono::duration<double>(std::chrono::steady_clock::now() - t.last_change_seen_at).count();
    if (idle < AUTO_REBUILD_DEBOUNCE_SECONDS) return false;
    if (t.has_failed_once) {
        double since_failure = std::chrono::duration<double>(std::chrono::steady_clock::now() - t.last_failed_at).count();
        if (since_failure < AUTO_REBUILD_FAILURE_COOLDOWN_SECONDS) return false;
    }
    if (t.has_reloaded_once) {
        double since_reload = std::chrono::duration<double>(std::chrono::steady_clock::now() - t.last_reload_handled_at).count();
        if (since_reload < AUTO_REBUILD_POST_RELOAD_GRACE_SECONDS) return false;
    }
    return true;
}

// ── Hot-reloadable scripts module rebuild (Unreal-style, no editor restart) ──
// Scripts in this engine are real compiled C++ classes — there is no
// Mono/Roslyn equivalent that can JIT a brand-new class into an already-
// running process. What makes a true "no restart" possible without a
// managed-runtime rewrite is the same trick Unreal Engine uses for its own
// C++ hot reload: gameplay code is NOT linked into the host executable at
// all. It lives in a separate shared library per PROJECT
// (game_scripts_<ns>.dll/.so, see editor/scripts_module/CMakeLists.txt)
// that the editor loads at runtime and can reload by swapping which
// library is loaded for that project — the editor.exe process itself is
// untouched by a script rebuild now, so it never exits, and OTHER
// projects' already-loaded modules are untouched too.
//
//   1. CMake re-globs project's scripts/*.cpp with CONFIGURE_DEPENDS (same
//      mechanism as before) — new/changed scripts are picked up the moment
//      a reconfigure runs. The reconfigure is now pointed at an ISOLATED
//      build tree rooted at editor/scripts_module/ (which declares its own
//      project(...) and needs nothing from the repo-root CMakeLists.txt)
//      instead of the shared build/ tree the full editor+engine build
//      uses. CMake has no notion of "reconfigure just one subdirectory" of
//      a tree that was configured as a whole, so the only way to stop a
//      script rebuild from re-walking engine_cpp's and editor's
//      CMakeLists.txt too (SDL2_image/SDL2_ttf probing, ImGui presence
//      checks, none of which scripts need) is to give scripts_module a
//      separate build directory of its own. The normal full build
//      (`cmake -S . -B build`) is untouched and still configures
//      everything together via add_subdirectory, as before — this
//      isolated tree exists ONLY for the editor's fast auto-rebuild path.
//   2. Only the `game_scripts_<ns>` target for THIS project gets rebuilt —
//      not `editor`, not any other project's scripts target — so this
//      compiles a handful of small script files belonging to one project,
//      not the whole editor and not every other project's scripts too.
//   3. The build links to a freshly timestamped library filename (via
//      SCRIPTS_MODULE_OUTPUT_NAME_<ns>) rather than the path the
//      currently-loaded module for THIS project has open — the same
//      lock-avoidance the old exe rebuild needed (Windows keeps a loaded
//      DLL's backing file open for as long as any process has it mapped,
//      same as a running .exe). Other projects' output-name cache vars are
//      untouched.
//   4. The actual cmake configure+build runs on a background thread, same
//      as before, so the render thread keeps drawing the blocking
//      "Compiling Scripts" modal while the compiler runs underneath.
//   5. On success, this only records the new module's path — it does NOT
//      call ScriptModuleLoader::load() itself. That swap (destroy every
//      live ScriptBase instance from the OLD module, LoadLibrary the new
//      one, re-register) must happen on the main/render thread, since it
//      touches ViewportPanel::_script_sys, which the render loop also
//      touches every frame — doing it from this worker thread would be a
//      data race. See ViewportPanel::poll_script_rebuild(), called once per
//      frame, which performs the actual swap once rb.done is observed true.
inline void rebuild_scripts_module(const std::string& project) {
    // An export uses the generated script header and its own CMake tree. Do
    // not let a watcher/manual script rebuild race those files while the
    // editor is intentionally locked behind the standalone-build modal.
    if (standalone_build_state().in_progress.load()) return;
    auto& rb = script_rebuild_state(project);
    // Atomic claim: only the caller that successfully flips in_progress
    // false->true gets to actually start a build. This used to be a plain
    // load()-then-assign, which is safe against the worker thread (which
    // only ever sets in_progress back to false) but was observed to still
    // allow overlapping rebuild attempts in practice — switching to a CAS
    // removes any possible window between the check and the set, however
    // that window was being hit.
    bool expected = false;
    if (!rb.in_progress.compare_exchange_strong(expected, true)) {
        return; // someone else already claimed this project's rebuild slot
    }
    if (rb.worker.joinable()) rb.worker.join(); // reap a finished previous run

    rb.done = false;
    rb.old_modules_unloaded = false;
    rb.success = false;
    rb.set_message("Starting rebuild (" + project + ")...");
    // Do not unload here. A previous Play session can still own ScriptBase
    // instances whose vtables and custom deleters live in this DLL. The main
    // thread's poll_script_rebuild() first destroys those instances and only
    // then unloads the module. Doing it here created a use-after-FreeLibrary
    // window while the background compiler was running.

    fs::path root  = find_engine_project_root();
    std::string ns = _cmake_namespace_for_project(project);

    rb.worker = std::thread([&rb, root, project, ns]() {
        auto quote = [](const fs::path& p) { return std::string("\"") + p.string() + "\""; };
#if defined(_WIN32)
        // The watcher and the explicit toolbar command can coexist in two
        // editor processes.  Use the same project-scoped OS mutex concept as
        // AutoHotReload so neither can compile the same shared physics
        // library while the other holds its .obj file open.
        HANDLE build_mutex = ::CreateMutexA(nullptr, FALSE,
            ("Local\\GameEngine2D_AutoHotReload_" + ns).c_str());
        const DWORD lock_result = build_mutex ? ::WaitForSingleObject(build_mutex, 60000) : WAIT_FAILED;
        if (lock_result != WAIT_OBJECT_0 && lock_result != WAIT_ABANDONED) {
            if (build_mutex) ::CloseHandle(build_mutex);
            rb.set_message("Timed out waiting for another editor's script build for '" + project + "'. Try again after it finishes.");
            rb.success = false; rb.done = true; rb.in_progress = false;
            return;
        }
        struct MutexRelease {
            HANDLE handle = nullptr;
            ~MutexRelease() { if (handle) { ::ReleaseMutex(handle); ::CloseHandle(handle); } }
        } release{build_mutex};
#endif
#if defined(_WIN32)
        // Query registered Visual Studio / Build Tools installations instead
        // of assuming a Community edition in one fixed folder.
        const std::string msvc_x64_env = gameengine::toolchain::host_x64_environment_prefix();
#else
        const std::string msvc_x64_env;
#endif

        fs::path scripts_src       = root / "editor" / "scripts_module";
        // Keep toolbar rebuilds in the same user-local cache as the watcher.
        // The source project may be in OneDrive, but compiler intermediates
        // must not be: a sync-provider handle on physics.obj or a PCH causes
        // the C1083 permission-denied error reported during script creation.
        fs::path scripts_build_dir = script_module_build_dir(root, ns);

        // Skip cmake reconfigure if the meta-target already exists — only
        // needed when a brand-new script file has appeared (CMake's GLOB
        // must pick it up and generate a new per-script target). For the
        // common case of editing an existing script, the target already
        // exists and `cmake --build` handles timestamps on its own.
        //
        // Configure against an ISOLATED build tree rooted at
        // editor/scripts_module itself (which declares its own
        // project(...) and is fully self-contained — own find_package
        // calls, no dependency on anything the root CMakeLists.txt sets
        // up), instead of the shared build/ tree the full editor+engine
        // build uses. This is the actual fix for "don't pay full-tree
        // reconfigure cost on every script rebuild": cmake -S has no
        // notion of reconfiguring "just one subdirectory" of a tree that
        // was configured as a whole, so the only way to make a reconfigure
        // touch ONLY scripts_module's CMakeLists.txt (not engine_cpp's or
        // editor's, which do their own SDL2_image/SDL2_ttf find_package
        // probing, ImGui presence checks, etc. — none of which scripts
        // need) is to give it a separate build directory of its own,
        // configured from editor/scripts_module/ as the source root
        // instead of the repo root.
        // An existing meta project says only that a graph was generated once;
        // it does not prove its script list is current. Always reconcile the
        // explicit "Scripts" rebuild with disk so deleted files cannot leave
        // stale wrapper targets behind.
        bool needs_configure = true;
        if (needs_configure) {
            rb.set_message("Reconfiguring " + project + "...");
            std::string configure = msvc_x64_env + "cmake -S " + quote(scripts_src) + " -B " + quote(scripts_build_dir) +
                " -DSCRIPTS_TARGET_PROJECT=" + project;
            if (std::system(configure.c_str()) != 0) {
                rb.set_message("Reconfigure failed — check a new/edited script in '" + project + "' for syntax errors, or the console window this editor was launched from.");
                rb.success = false; rb.done = true; rb.in_progress = false;
                return;
            }
        }

        rb.set_message("Compiling scripts (" + project + ")...");
        // Build per-script DLLs via the scripts_project_${ns} meta-target.
        // CMake tracks each per-script target independently — only NEW or
        // CHANGED scripts actually recompile. Adding one new .cpp file
        // builds exactly one new DLL (1-3 seconds) instead of the old
        // monolithic target which recompiled every script in one TU.
        // The DLLs are written to the FIXED output directory (Release/)
        // because we already unloaded old DLLs above — no file-lock conflict.
        std::string target = "scripts_project_" + ns;
        std::string build = msvc_x64_env + "cmake --build " + quote(scripts_build_dir) + " --target " + target;
#if defined(_WIN32)
        build += " --config Release -- /p:PreferredToolArchitecture=x64";
        // NO /m — per-script PCH (each target compiles its own PCH) exhausts
        // disk when 40+ cl.exe instances each write a multi-MB .pch in
        // parallel. Sequential builds conserve disk space (~1.5GB free).
        // Per-script hot-reload (AHR) already builds single targets without
        // /m and has no disk issue (one PCH at a time).
#else
        build += " --parallel";
#endif
        if (std::system(build.c_str()) != 0) {
            rb.set_message("Build failed — likely a compile error in a new/edited script in '" + project + "'. Check the console window this editor was launched from.");
            rb.success = false; rb.done = true; rb.in_progress = false;
            return;
        }

        fs::path out_dir = scripts_build_dir / "Release";
        rb.set_message("Done — reloading scripts (" + project + ")...");
        rb.set_pending_module_dir(out_dir);
        rb.success = true;
        rb.done = true;
        rb.in_progress = false;
        // No relaunch, no std::exit — the editor process keeps running.
        // ViewportPanel::poll_script_rebuild() picks up pending_module_path
        // on the main thread next frame and performs the actual load for
        // THIS project specifically.
    });
}

// ─── ConsolePanel ─────────────────────────────────────────────────────────────
#define CONSOLE_PANEL_DEFINED
class ConsolePanel {
    bool _show_info    = true;
    bool _show_warn    = true;
    bool _show_error   = true;
    bool _show_engine  = true;
    bool _show_success = true;
    int  _info_count   = 0;
    int  _warn_count   = 0;
    int  _error_count  = 0;

public:
    void draw(EditorState& st) {
        ImGui::SetNextWindowSize({600,140}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Console##win")) { ImGui::End(); return; }

        // Snapshot under lock ONCE per frame instead of iterating st.logs
        // directly. st.logs can be appended to from background threads
        // (AutoHotReload's file-watcher/build threads route through
        // Debug::set_log_callback -> st.log*), so a live range-for over the
        // deque here would race against those push_back()/pop_front() calls.
        // See the comment on EditorState::logs for details.
        const std::deque<LogEntry> log_snapshot = st.logs_snapshot();

        // Count logs by level
        _info_count = _warn_count = _error_count = 0;
        for (auto& e : log_snapshot) {
            if (e.level == LogLevel::Info || e.level == LogLevel::Success || e.level == LogLevel::Engine) ++_info_count;
            else if (e.level == LogLevel::Warn) ++_warn_count;
            else if (e.level == LogLevel::Error) ++_error_count;
        }

        // Toolbar
        if (ImGui::Button("Clear")) st.logs_clear();
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &st.con_autoscroll);
        ImGui::SameLine(0, 12);

        // Log-level filter toggles (Unity style)
        {
            char lbl[32];
            // Info/Engine/Success
            if (!_show_info) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f,0.25f,0.25f,1.f));
            else             ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f,0.35f,0.38f,1.f));
            snprintf(lbl, sizeof(lbl), "Info %d", _info_count);
            if (ImGui::SmallButton(lbl)) _show_info = !_show_info;
            ImGui::PopStyleColor();
            ImGui::SameLine(0,2);

            if (!_show_warn) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f,0.25f,0.25f,1.f));
            else             ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.50f,0.38f,0.05f,1.f));
            snprintf(lbl, sizeof(lbl), "Warn %d", _warn_count);
            if (ImGui::SmallButton(lbl)) _show_warn = !_show_warn;
            ImGui::PopStyleColor();
            ImGui::SameLine(0,2);

            if (!_show_error) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f,0.25f,0.25f,1.f));
            else              ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f,0.15f,0.15f,1.f));
            snprintf(lbl, sizeof(lbl), "Error %d", _error_count);
            if (ImGui::SmallButton(lbl)) _show_error = !_show_error;
            ImGui::PopStyleColor();
        }

        ImGui::SameLine(0, 12);
        ImGui::SetNextItemWidth(160);
        static char fbuf[128] = {};
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f,0.7f,0.7f,1.f));
        if (ImGui::InputTextWithHint("##confilt", "Search logs...", fbuf, sizeof(fbuf))) {
            st.con_filter = fbuf;
            std::transform(st.con_filter.begin(), st.con_filter.end(), st.con_filter.begin(), ::tolower);
        }
        ImGui::PopStyleColor();

        ImGui::Separator();
        ImGui::BeginChild("##logchild", {0,0}, false, ImGuiWindowFlags_HorizontalScrollbar);

        for (auto& entry : log_snapshot) {
            // Level filter
            bool level_visible = true;
            switch (entry.level) {
                case LogLevel::Info:    level_visible = _show_info; break;
                case LogLevel::Success: level_visible = _show_info && _show_success; break;
                case LogLevel::Engine:  level_visible = _show_info && _show_engine; break;
                case LogLevel::Warn:    level_visible = _show_warn; break;
                case LogLevel::Error:   level_visible = _show_error; break;
            }
            if (!level_visible) continue;

            if (!st.con_filter.empty()) {
                std::string lo = entry.message;
                std::transform(lo.begin(),lo.end(),lo.begin(),::tolower);
                if (lo.find(st.con_filter)==std::string::npos) continue;
            }
            ImVec4 col = ImVec4(0.78f,0.78f,0.78f,1.f);
            const char* prefix = "";
            switch (entry.level) {
                case LogLevel::Warn:    col={1.f,0.78f,0.24f,1.f}; prefix="[WARN] "; break;
                case LogLevel::Error:   col={1.f,0.36f,0.36f,1.f}; prefix="[ERR]  "; break;
                case LogLevel::Success: col={0.31f,0.86f,0.47f,1.f}; break;
                case LogLevel::Engine:  col={0.47f,0.71f,1.f,1.f}; prefix="[ENG]  "; break;
                default: break;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f,0.45f,0.45f,1.f));
            ImGui::TextUnformatted(entry.timestamp.c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine(0, 4);
            ImGui::TextColored(col, "%s%s", prefix, entry.message.c_str());
            // Right-click to copy
            if (ImGui::BeginPopupContextItem(("##logctx"+entry.timestamp+entry.message).c_str())) {
                if (ImGui::MenuItem("Copy")) ImGui::SetClipboardText(entry.message.c_str());
                ImGui::EndPopup();
            }
        }

        if (st.con_autoscroll) ImGui::SetScrollHereY(1.f);
        ImGui::EndChild();
        ImGui::End();
    }
};

// ─── Hierarchy entity icon helper ─────────────────────────────────────────────
// Draws a tiny Unity-style inline icon (16×16) for an entity based on its
// components, then advances the cursor so text follows naturally.
namespace hier_icons {

// Returns a tag string based on which components the entity has.
inline std::string entity_kind(const Entity& e) {
    if (!e.contains("components")) return "entity";
    const auto& comps = e["components"];
    if (comps.contains("Camera2D") || comps.contains("Cinemachine2D")) return "camera";
    if (comps.contains("Light2D"))     return "light";
    if (comps.contains("AudioSource")) return "audio";
    if (comps.contains("Rigidbody2D") || comps.contains("BoxCollider2D")
     || comps.contains("CircleCollider2D") || comps.contains("CapsuleCollider2D")
     || comps.contains("PolygonCollider2D") || comps.contains("EdgeCollider2D")
     || comps.contains("CompositeCollider2D")) return "physics";
    if (comps.contains("Script") || comps.contains("ScriptComponent")) return "script";
    if (comps.contains("SpriteRenderer")) return "sprite";
    if (comps.contains("Tilemap"))     return "tilemap";
    if (comps.contains("ParticleEmitter")) return "particles";
    return "entity";
}

// Draw a 16×16 icon at `p` using ImDrawList primitives.
inline void draw(ImDrawList* dl, ImVec2 p, const std::string& kind) {
    const float S = 14.f;  // icon outer size
    ImVec2 mn = p, mx{p.x + S, p.y + S};
    float cx = p.x + S * 0.5f, cy = p.y + S * 0.5f;

    if (kind == "camera") {
        // Dark body + lens circle
        ImU32 body = IM_COL32(72, 150, 220, 255);
        ImU32 lens = IM_COL32(180, 220, 255, 255);
        dl->AddRectFilled({p.x+1, p.y+3}, {p.x+S-3, p.y+S-3}, body, 2.f);
        // Triangle "viewfinder" notch on top-right
        dl->AddTriangleFilled({p.x+S-3, p.y+3}, {mx.x+1, p.y+1}, {mx.x+1, p.y+S-4}, IM_COL32(100,175,240,255));
        dl->AddCircleFilled({p.x+S*0.38f, cy}, S*0.2f, lens);
    } else if (kind == "light") {
        // Yellow circle with rays
        ImU32 col = IM_COL32(255, 220, 60, 255);
        dl->AddCircleFilled({cx, cy}, S*0.28f, col);
        for (int i = 0; i < 8; ++i) {
            float a = i * 3.14159f * 2.f / 8.f;
            float r0 = S*0.34f, r1 = S*0.48f;
            dl->AddLine({cx + cosf(a)*r0, cy + sinf(a)*r0},
                        {cx + cosf(a)*r1, cy + sinf(a)*r1}, col, 1.5f);
        }
    } else if (kind == "audio") {
        // Simple speaker shape
        ImU32 col = IM_COL32(120, 200, 120, 255);
        ImVec2 pts[4] = {{p.x+2, p.y+5}, {p.x+5, p.y+5}, {p.x+8, p.y+3}, {p.x+8, p.y+S-3}};
        pts[3] = {p.x+8, p.y+S-3};
        dl->AddQuadFilled(pts[0], pts[1], {p.x+5, p.y+S-5}, {p.x+2, p.y+S-5}, col);
        dl->AddTriangleFilled({p.x+5, p.y+5}, {p.x+8, p.y+3}, {p.x+8, p.y+S-3}, col);
        // wave arcs
        ImU32 wc = IM_COL32(160, 230, 160, 255);
        dl->AddCircle({p.x+8, cy}, S*0.22f, wc, 12, 1.2f);
        dl->AddCircle({p.x+8, cy}, S*0.38f, wc, 12, 1.0f);
    } else if (kind == "physics") {
        // Orange hexagon-ish collision indicator
        ImU32 col = IM_COL32(240, 150, 50, 255);
        ImVec2 pts[6];
        for (int i = 0; i < 6; ++i) {
            float a = i * 3.14159f * 2.f / 6.f - 3.14159f/6.f;
            pts[i] = {cx + cosf(a)*S*0.44f, cy + sinf(a)*S*0.44f};
        }
        dl->AddPolyline(pts, 6, col, ImDrawFlags_Closed, 1.8f);
        dl->AddLine({cx-S*0.2f, cy}, {cx+S*0.2f, cy}, col, 1.5f);
        dl->AddLine({cx, cy-S*0.2f}, {cx, cy+S*0.2f}, col, 1.5f);
    } else if (kind == "script") {
        // Blue code brackets < >
        ImU32 col = IM_COL32(100, 170, 255, 255);
        dl->AddLine({p.x+4, cy-S*0.25f}, {p.x+1, cy}, col, 1.8f);
        dl->AddLine({p.x+1, cy}, {p.x+4, cy+S*0.25f}, col, 1.8f);
        dl->AddLine({p.x+S-4, cy-S*0.25f}, {p.x+S-1, cy}, col, 1.8f);
        dl->AddLine({p.x+S-1, cy}, {p.x+S-4, cy+S*0.25f}, col, 1.8f);
        dl->AddLine({p.x+S*0.38f, cy+S*0.3f}, {p.x+S*0.62f, cy-S*0.3f}, IM_COL32(160,200,255,200), 1.3f);
    } else if (kind == "sprite") {
        // Teal image frame with mountain
        ImU32 frame = IM_COL32(60, 170, 140, 255);
        ImU32 mtn   = IM_COL32(90, 200, 160, 255);
        dl->AddRectFilled({p.x+1, p.y+2}, {p.x+S-1, p.y+S-2}, frame, 2.f);
        dl->AddTriangleFilled({p.x+2, p.y+S-3}, {p.x+S*0.5f, p.y+5}, {p.x+S-2, p.y+S-3}, mtn);
    } else if (kind == "tilemap") {
        // Grid of small squares
        ImU32 col = IM_COL32(200, 160, 90, 255);
        float gs = S / 3.2f;
        for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            ImVec2 tl{p.x + 1 + c*(gs+1), p.y + 1 + r*(gs+1)};
            dl->AddRect(tl, {tl.x+gs, tl.y+gs}, col, 0.5f, 0, 1.0f);
        }
    } else if (kind == "particles") {
        // Several small dots scattered
        ImU32 col = IM_COL32(200, 140, 220, 255);
        float dots[5][2] = {{.5f,.2f},{.2f,.55f},{.8f,.45f},{.45f,.8f},{.72f,.72f}};
        for (auto& d : dots)
            dl->AddCircleFilled({p.x+S*d[0], p.y+S*d[1]}, 1.6f, col);
    } else {
        // Generic entity: grey cube outline
        ImU32 col = IM_COL32(180, 180, 185, 255);
        dl->AddRect({p.x+2, p.y+3}, {p.x+S-2, p.y+S-3}, col, 1.5f, 0, 1.3f);
        dl->AddLine({p.x+2, p.y+3}, {p.x+S*0.5f, p.y+1}, col, 1.1f);
        dl->AddLine({p.x+S-2, p.y+3}, {p.x+S*0.5f, p.y+1}, col, 1.1f);
    }
}

// Draw icon inline + advance cursor so text follows with proper spacing.
// The icon occupies a 16×16 slot that is vertically centered on the text line.
inline void draw_inline(ImDrawList* dl, const std::string& kind) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    p.y += (ImGui::GetTextLineHeight() - 14.f) * 0.5f; // center vertically
    draw(dl, p, kind);
    ImGui::Dummy({16.f, ImGui::GetTextLineHeight()});
    ImGui::SameLine(0.f, 4.f);
}

} // namespace hier_icons

// ─── HierarchyPanel ───────────────────────────────────────────────────────────
// Renders entities as a tree (root objects, each with their children nested
// underneath, recursively) instead of a flat list. Supports drag-and-drop
// reparenting exactly like Unity's Hierarchy window: drag one or more
// selected entities onto another entity to parent them under it (world
// position is preserved), or drop onto the "Scene Root" zone at the bottom
// to un-parent them back to root.
class HierarchyPanel {
    char _search[128] = {};
    char _new_name[128] = "Entity";
    char _rename_buf[128] = {};
    int  _rename_target = -1;      // entity id being inline-renamed (-1 = none)
    bool _rename_just_opened = false;
    bool _rename_pending = false;  // deferred modal open
    bool _show_ctx = false;
    // Duplicate/Delete clicked from an item's right-click menu must NOT run
    // immediately: we're still nested inside BeginPopupContextItem(), which
    // is itself nested inside this entity's TreeNodeEx/_draw_node call. Erasing
    // or appending to st.entities right then invalidates the Entity* and the
    // children list that the still-unwinding call stack is about to use,
    // which crashed the app. So these just request the action; draw() runs it
    // once, after the whole tree has finished drawing for the frame.
    enum class PendingAction { None, Duplicate, Delete };
    PendingAction _pending_action = PendingAction::None;
    static constexpr const char* DRAG_PAYLOAD = "HIER_ENTITY_IDS";

public:
    void draw(EditorState& st) {
        ImGui::SetNextWindowSize({220,400}, ImGuiCond_FirstUseEver);
        // Show entity count in window title
        char title_buf[64];
        int visible = 0;
        for (auto& e : st.entities) if (!e.value("_runtime_only",false)) ++visible;
        snprintf(title_buf, sizeof(title_buf), "Hierarchy (%d)###win", visible);
        if (!ImGui::Begin(title_buf)) { ImGui::End(); return; }

        // ── Toolbar ───────────────────────────────────────────────────────────
        // Create button (+) with dropdown style
        if (ImGui::Button("+")) {
            auto e = SceneIO::make_entity(_new_name, st);
            st.entities.push_back(e);
            transform::mark_structure_dirty();
            st.select(e.value("id",0));
            st.log("Created: " + std::string(_new_name));
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Create entity named below");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::InputText("##newname", _new_name, sizeof(_new_name));
        ImGui::SameLine(0, 4);
        if (ImGui::SmallButton("Dup")) { _duplicate_selected(st); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Duplicate selected (Ctrl+D)");
        ImGui::SameLine(0, 2);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f,0.18f,0.18f,1.f));
        if (ImGui::SmallButton("Del")) { _delete_selected(st); }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete selected (Delete)");

        // Search bar
        ImGui::SetNextItemWidth(-1);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f,0.7f,0.7f,1.f));
        ImGui::InputTextWithHint("##search", "Search...", _search, sizeof(_search));
        ImGui::PopStyleColor();

        ImGui::Separator();
        ImGui::BeginChild("##hierlist", {0,0}, false);

        std::string search_lo = _search;
        std::transform(search_lo.begin(),search_lo.end(),search_lo.begin(),::tolower);
        bool filtering = !search_lo.empty();

        if (!filtering) {
            // ── Tree mode: only walk roots, recurse into children ───────────────
            for (auto& e : st.entities) {
                if (e.value("_runtime_only",false)) continue;
                if (EditorState::parent_of(e) >= 0) continue; // not a root, drawn by its parent
                _draw_node(st, e.value("id",0), 0);
            }
        } else {
            // ── Search mode: flat filtered list (tree structure isn't useful
            //    once you're hunting for a name across the whole scene) ────────
            for (auto& e : st.entities) {
                if (e.value("_runtime_only",false)) continue;
                std::string name = e.value("name","Entity");
                std::string lo = name;
                std::transform(lo.begin(),lo.end(),lo.begin(),::tolower);
                if (lo.find(search_lo)==std::string::npos) continue;
                _draw_row(st, e.value("id",0), name, false);
            }
        }

        ImGui::Spacing();
        ImGui::Separator();

        // ── Scene Root drop zone — drag here to un-parent back to root,
        //    or drag a .prefab asset here to instantiate it at the root ──────
        ImGui::Selectable("(Scene Root)", false, ImGuiSelectableFlags_Disabled);
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(DRAG_PAYLOAD)) {
                _drop_onto(st, -1, payload, DropPlacement::Inside);
            }
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                const char* payload_str = static_cast<const char*>(pl->Data);
                prefab_ui::handle_hierarchy_prefab_drop(payload_str, -1, st);
            }
            ImGui::EndDragDropTarget();
        }

        _draw_rename_dialog(st);

        // Empty-area right-click: only fires when NOT hovering over an item
        if (ImGui::BeginPopupContextWindow("##hierctx", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
            _draw_create_menu_items(st);
            ImGui::Separator();
            if (ImGui::MenuItem("Select All")) {
                st.selected_ids.clear();
                for (auto& e : st.entities)
                    if (!e.value("_runtime_only",false))
                        st.selected_ids.push_back(e.value("id",0));
                if (!st.selected_ids.empty()) { st.selected_id=st.selected_ids.back(); st.asset_selection_is_newer=false; }
            }
            ImGui::EndPopup();
        }

        ImGui::EndChild();
        ImGui::End();

        // Apply any Duplicate/Delete requested from an item's right-click menu
        // now that the tree (and the popup that requested it) has fully
        // finished drawing — see PendingAction comment above for why this
        // can't run immediately, mid-popup.
        if (_pending_action == PendingAction::Duplicate) { _pending_action = PendingAction::None; _duplicate_selected(st); }
        else if (_pending_action == PendingAction::Delete) { _pending_action = PendingAction::None; _delete_selected(st); }
    }


    void _open_rename_dialog(EditorState& st, int eid) {
        if (Entity* e = st.find_entity(eid)) {
            snprintf(_rename_buf, sizeof(_rename_buf), "%s", e->value("name", "Entity").c_str());
            // _draw_node()/_draw_row() check _rename_target first each frame
            // and will route this into the inline (in-place) rename box
            // rather than the modal below. That inline path treats "not just
            // opened and not active" as a cancel, so without this flag it
            // would immediately close itself the same frame it opens.
            _rename_target = eid;
            _rename_pending = true;
            _rename_just_opened = true;
        }
    }

    void _draw_rename_dialog(EditorState& st) {
        if (_rename_pending) {
            ImGui::OpenPopup("##hier_rename");
            _rename_pending = false;
        }
        if (_rename_target < 0) return;
        if (ImGui::BeginPopupModal("##hier_rename", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Rename Entity");
            ImGui::SetNextItemWidth(240.f);
            ImGui::InputText("##rename_name", _rename_buf, sizeof(_rename_buf));
            if (ImGui::Button("Rename") && _rename_buf[0] != '\0') {
                if (Entity* e = st.find_entity(_rename_target)) {
                    st.undo.push_deep(st.entities);
                    (*e)["name"] = std::string(_rename_buf);
                }
                _rename_target = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                _rename_target = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // Shared "Create ..." menu items — used both by the empty-area hierarchy
    // context menu and (combined, Unity-style) at the bottom of an item's
    // context menu, so new objects can be spawned without right-clicking
    // empty space first.
    void _draw_create_menu_items(EditorState& st) {
        if (ImGui::MenuItem("Create Empty")) {
            auto e = SceneIO::make_entity("GameObject", st);
            st.entities.push_back(e); transform::mark_structure_dirty(); st.select(e.value("id",0));
        }
        if (ImGui::MenuItem("Create Sprite")) {
            auto e = SceneIO::make_entity("Sprite", st);
            e["components"]["SpriteRenderer"] = component_defaults()["SpriteRenderer"];
            st.entities.push_back(e); transform::mark_structure_dirty(); st.select(e.value("id",0));
        }
        if (ImGui::MenuItem("Create Camera")) {
            auto e = SceneIO::make_entity("Camera", st);
            e["components"]["Camera2D"] = component_defaults()["Camera2D"];
            st.entities.push_back(e); transform::mark_structure_dirty(); st.select(e.value("id",0));
        }
        if (ImGui::MenuItem("Create Particle System")) {
            auto e = SceneIO::make_entity("Particles", st);
            e["components"]["ParticleEmitter"] = component_defaults()["ParticleEmitter"];
            st.entities.push_back(e); transform::mark_structure_dirty(); st.select(e.value("id",0));
        }
        if (ImGui::MenuItem("Create Tilemap")) {
            auto e = SceneIO::make_entity("Tilemap", st);
            e["components"]["Tilemap"] = component_defaults()["Tilemap"];
            st.entities.push_back(e); transform::mark_structure_dirty(); st.select(e.value("id",0));
        }
    }

    void _draw_item_context(EditorState& st, int eid, bool can_unparent) {
        Entity* e = st.find_entity(eid);
        if (!e) return;
        bool is_sel = (std::find(st.selected_ids.begin(),st.selected_ids.end(),eid)!=st.selected_ids.end());
        if (!is_sel) { st.selected_ids = {eid}; st.selected_id = eid; st.asset_selection_is_newer = false; }

        // Inline properties header (Unity style) --------------------------------
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));
        ImGui::TextUnformatted(e->value("name","Entity").c_str());
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
        ImGui::Text("ID: %d  |  Parent: %d", eid, EditorState::parent_of(*e));
        if (e->contains("components")) {
            std::string comp_list;
            for (auto& [k, v] : (*e)["components"].items()) {
                if (!comp_list.empty()) comp_list += ", ";
                comp_list += k;
            }
            if (!comp_list.empty()) ImGui::Text("Components: %s", comp_list.c_str());
        }
        if (e->contains("components") && (*e)["components"].contains("Transform")) {
            auto& t = (*e)["components"]["Transform"];
            ImGui::Text("Pos: (%.1f, %.1f)  Rot: %.1f  Scale: (%.1f, %.1f)",
                t.value("x",0.f), t.value("y",0.f),
                t.value("rotation",0.f),
                t.value("scale_x",1.f), t.value("scale_y",1.f));
        }
        ImGui::PopStyleColor();
        bool active = e->value("active", true);
        if (ImGui::Checkbox("Active", &active)) {
            st.undo.push_deep(st.entities);
            if (Entity* cur = st.find_entity(eid)) (*cur)["active"] = active;
        }
        ImGui::Separator();

        // Actions ---------------------------------------------------------------
        if (ImGui::MenuItem("Rename")) _open_rename_dialog(st, eid);
        if (ImGui::MenuItem("Duplicate")) _pending_action = PendingAction::Duplicate;
        if (ImGui::MenuItem("Delete"))    _pending_action = PendingAction::Delete;
        if (can_unparent && ImGui::MenuItem("Unparent")) st.reparent(eid, -1);

        // ── Prefab context menu (Gap 3) ────────────────────────────────────
        prefab_ui::draw_hierarchy_prefab_menu(eid, st, st.asset_dir);

        ImGui::Separator();
        if (ImGui::BeginMenu("Create Child")) {
            if (ImGui::MenuItem("Empty")) {
                auto ne = SceneIO::make_entity("GameObject", st);
                int new_id = ne.value("id",0);
                st.entities.push_back(ne); transform::mark_structure_dirty();
                st.reparent(new_id, eid); st.select(new_id);
            }
            if (ImGui::MenuItem("Sprite")) {
                auto ne = SceneIO::make_entity("Sprite", st);
                ne["components"]["SpriteRenderer"] = component_defaults()["SpriteRenderer"];
                int new_id = ne.value("id",0);
                st.entities.push_back(ne); transform::mark_structure_dirty();
                st.reparent(new_id, eid); st.select(new_id);
            }
            ImGui::EndMenu();
        }

        // Combined with the empty-area menu (Unity style): lets you spawn a
        // new root-level object directly from an item's context menu too.
        if (ImGui::BeginMenu("Create")) {
            _draw_create_menu_items(st);
            ImGui::EndMenu();
        }
    }

private:
    enum class DropPlacement { Before, After, Inside };

    static DropPlacement _placement_from_mouse(const ImVec2& min, const ImVec2& max) {
        float h = std::max(1.f, max.y - min.y);
        float rel = (ImGui::GetIO().MousePos.y - min.y) / h;
        if (rel < 0.33f) return DropPlacement::Before;
        if (rel > 0.67f) return DropPlacement::After;
        return DropPlacement::Inside;
    }

    static void _draw_drop_line(const ImVec2& min, const ImVec2& max, DropPlacement placement) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 col = IM_COL32(88, 155, 255, 255);
        float y = (placement == DropPlacement::Before) ? min.y : max.y;
        dl->AddLine(ImVec2(min.x, y), ImVec2(max.x, y), col, 2.0f);
        if (placement == DropPlacement::Inside) {
            dl->AddRectFilled(ImVec2(min.x, min.y), ImVec2(max.x, max.y), IM_COL32(88,155,255,30));
        }
    }

    void _draw_item_drop_preview(int target_eid) {
        if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem | ImGuiHoveredFlags_RectOnly))
            return;
        if (ImGui::GetDragDropPayload() == nullptr) return;
        ImVec2 min = ImGui::GetItemRectMin();
        ImVec2 max = ImGui::GetItemRectMax();
        _draw_drop_line(min, max, _placement_from_mouse(min, max));
    }

    // Recursively draws `eid` and (if expanded) its children, indented.
    void _draw_node(EditorState& st, int eid, int depth) {
        Entity* ep = st.find_entity(eid);
        if (!ep) return;
        std::string name = ep->value("name","Entity");
        std::vector<int> kids = st.children_of(eid);

        if (kids.empty()) {
            _draw_row(st, eid, name, false);
            return;
        }

        ImGui::PushID(eid);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth
                                  | ImGuiTreeNodeFlags_DefaultOpen;
        bool is_sel = (std::find(st.selected_ids.begin(),st.selected_ids.end(),eid)!=st.selected_ids.end());
        if (is_sel) flags |= ImGuiTreeNodeFlags_Selected;
        bool active = ep->value("active",true);
        // "##eid" suffix hides the id from the visible label while still
        // giving ImGui a unique internal id; the icon sits in the gap between
        // the expand arrow and the text, which we widen to exactly 20 px.
        std::string label = "  " + name + "  [" + std::to_string(eid) + "]" + (active?"":"  (off)") + "##" + std::to_string(eid);

        // Inline rename replaces the TreeNode while active.
        if (_rename_target == eid) {
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 4);
            bool enter = ImGui::InputText("##hierrename", _rename_buf, sizeof(_rename_buf),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            bool just_opened = _rename_just_opened;
            if (just_opened) { ImGui::SetKeyboardFocusHere(-1); _rename_just_opened = false; }
            bool commit = enter || ImGui::IsItemDeactivatedAfterEdit();
            // On the opening frame, focus was only just requested above and
            // ImGui hasn't applied it yet, so IsItemActive() is still false —
            // checking the now-cleared _rename_just_opened here would close
            // the box on the very frame it appears. Use the pre-clear value.
            bool cancel = !ImGui::IsItemActive() && !just_opened && !enter;
            if (commit || cancel) {
                if (commit && _rename_buf[0] != '\0') {
                    st.undo.push_deep(st.entities);
                    (*ep)["name"] = std::string(_rename_buf);
                }
                _rename_target = -1;
            }
            ImGui::PopID();
            return;
        }

        bool open = ImGui::TreeNodeEx(label.c_str(), flags);
        // Overlay entity icon
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 item_min = ImGui::GetItemRectMin();
            float  item_h   = ImGui::GetItemRectSize().y;
            float  arrow_w  = ImGui::GetTreeNodeToLabelSpacing();
            ImVec2 icon_p{item_min.x + arrow_w + 2.f, item_min.y + (item_h - 14.f) * 0.5f};
            hier_icons::draw(dl, icon_p, hier_icons::entity_kind(*ep));
        }
        // Double-click label to rename inline.
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            _rename_target = eid;
            _rename_just_opened = true;
            snprintf(_rename_buf, sizeof(_rename_buf), "%s", name.c_str());
        }
        _handle_row_interaction(st, eid);
        _handle_drag_source(st, eid);
        _draw_item_drop_preview(eid);
        _handle_drop_target(st, eid);
        if (ImGui::BeginPopupContextItem("##itemctx")) {
            _draw_item_context(st, eid, EditorState::parent_of(*ep) >= 0);
            ImGui::EndPopup();
        }

        if (open) {
            for (int kid : kids) _draw_node(st, kid, depth+1);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    // Leaf row (no children) — plain Selectable, still drag/drop enabled.
    void _draw_row(EditorState& st, int eid, const std::string& name, bool /*unused*/) {
        Entity* ep = st.find_entity(eid);
        if (!ep) return;
        bool is_sel = (std::find(st.selected_ids.begin(),st.selected_ids.end(),eid)!=st.selected_ids.end());
        bool active = ep->value("active",true);

        ImGui::PushID(eid);

        // ── Inline double-click rename (same collision-safe approach as param rename) ──
        if (_rename_target == eid) {
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 4);
            bool enter = ImGui::InputText("##hierrename", _rename_buf, sizeof(_rename_buf),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            bool just_opened = _rename_just_opened;
            if (just_opened) { ImGui::SetKeyboardFocusHere(-1); _rename_just_opened = false; }
            bool commit = enter || ImGui::IsItemDeactivatedAfterEdit();
            bool cancel = !ImGui::IsItemActive() && !just_opened && !enter;
            if (commit || cancel) {
                if (commit && _rename_buf[0] != '\0') {
                    st.undo.push_deep(st.entities);
                    (*ep)["name"] = std::string(_rename_buf);
                }
                _rename_target = -1;
            }
            ImGui::PopID();
            return;
        }

        // The icon below is painted at item_min.x + arrow_w + 2 so leaf rows
        // line up in the same icon column as tree-node rows (see _draw_node).
        // The label's leading gutter must therefore reserve that same
        // arrow_w of space — a fixed "two spaces" fell short of it at
        // larger UI scales/fonts and let the icon overlap the name's first
        // letter(s), so the space count is derived from the real glyph width.
        float arrow_w = ImGui::GetTreeNodeToLabelSpacing();
        float space_w = ImGui::CalcTextSize(" ").x;
        float target_gap = arrow_w + 18.f; // clears the 14px icon (drawn at +2px) plus a small margin
        int   gutter_spaces = space_w > 0.f ? (int)std::ceil(target_gap / space_w) : 5;
        std::string label = std::string(gutter_spaces, ' ') + name + "  [" + std::to_string(eid) + "]" + (active?"":"  (off)") + "##" + std::to_string(eid);
        ImGui::Selectable(label.c_str(), is_sel, ImGuiSelectableFlags_AllowDoubleClick);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            _rename_target = eid;
            _rename_just_opened = true;
            snprintf(_rename_buf, sizeof(_rename_buf), "%s", name.c_str());
        }
        // Overlay entity icon (leaf row — no expand arrow, so use the same
        // arrow spacing value which equals the icon indent width ImGui reserves).
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 item_min = ImGui::GetItemRectMin();
            float  item_h   = ImGui::GetItemRectSize().y;
            ImVec2 icon_p{item_min.x + arrow_w + 2.f, item_min.y + (item_h - 14.f) * 0.5f};
            hier_icons::draw(dl, icon_p, hier_icons::entity_kind(*ep));
        }
        _handle_row_interaction(st, eid);
        _handle_drag_source(st, eid);
        _draw_item_drop_preview(eid);
        _handle_drop_target(st, eid);

        if (ImGui::BeginPopupContextItem("##itemctx")) {
            _draw_item_context(st, eid, EditorState::parent_of(*ep) >= 0);
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    // Click-to-select / ctrl-multiselect / shift-range-select, shared by both
    // tree nodes and flat search rows.
    void _handle_row_interaction(EditorState& st, int eid) {
        if (!ImGui::IsItemClicked() && !(ImGui::IsItemToggledOpen())) return;
        if (ImGui::IsItemToggledOpen()) return; // arrow-click just expands, doesn't select
        if (!ImGui::IsItemClicked(ImGuiMouseButton_Left)) return;

        bool ctrl  = ImGui::GetIO().KeyCtrl;
        bool shift = ImGui::GetIO().KeyShift;
        if (ctrl) {
            auto it = std::find(st.selected_ids.begin(),st.selected_ids.end(),eid);
            if (it==st.selected_ids.end()) st.selected_ids.push_back(eid);
            else st.selected_ids.erase(it);
            st.selected_id = eid;
            st.asset_selection_is_newer = false;
        } else if (shift && st.selected_id>=0) {
            int idx1=-1, idx0=-1;
            for (int i=0;i<(int)st.entities.size();++i) {
                if (st.entities[i].value("id",0)==eid) idx1=i;
                if (st.entities[i].value("id",0)==st.selected_id) idx0=i;
            }
            if (idx0>=0 && idx1>=0) {
                int lo=std::min(idx0,idx1), hi=std::max(idx0,idx1);
                st.selected_ids.clear();
                for (int i=lo;i<=hi;++i) st.selected_ids.push_back(st.entities[i].value("id",0));
            } else {
                st.selected_ids = {eid};
            }
            st.selected_id = eid;
            st.asset_selection_is_newer = false;
        } else {
            st.selected_ids = {eid};
            st.selected_id = eid;
            st.asset_selection_is_newer = false;
        }
    }

    // Makes the current item a drag source carrying the full current
    // multi-selection (so dragging any selected entity moves the whole
    // selection), matching Unity's Hierarchy drag behavior.
    void _handle_drag_source(EditorState& st, int eid) {
        if (!ImGui::BeginDragDropSource()) return;
        bool dragging_sel = std::find(st.selected_ids.begin(),st.selected_ids.end(),eid)!=st.selected_ids.end();
        std::vector<int> payload_ids = dragging_sel
            ? std::vector<int>(st.selected_ids.begin(), st.selected_ids.end())
            : std::vector<int>{eid};
        ImGui::SetDragDropPayload(DRAG_PAYLOAD, payload_ids.data(), payload_ids.size()*sizeof(int));

        if (payload_ids.size()==1) {
            Entity* ep = st.find_entity(payload_ids[0]);
            ImGui::Text("%s", ep ? ep->value("name","Entity").c_str() : "Entity");
        } else {
            ImGui::Text("%d entities", (int)payload_ids.size());
        }
        ImGui::EndDragDropSource();
    }

    void _handle_drop_target(EditorState& st, int target_eid) {
        if (!ImGui::BeginDragDropTarget()) return;
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(DRAG_PAYLOAD)) {
            ImVec2 min = ImGui::GetItemRectMin();
            ImVec2 max = ImGui::GetItemRectMax();
            DropPlacement placement = _placement_from_mouse(min, max);
            _drop_onto(st, target_eid, payload, placement);
        }
        // Dragging a .prefab asset from the Assets panel onto a hierarchy
        // row instantiates it as a child of that row, same as Unity.
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            const char* payload_str = static_cast<const char*>(pl->Data);
            prefab_ui::handle_hierarchy_prefab_drop(payload_str, target_eid, st);
        }
        ImGui::EndDragDropTarget();
    }

    std::vector<int> _payload_ids(const ImGuiPayload* payload) {
        std::vector<int> ids;
        if (!payload || payload->DataSize < (int)sizeof(int)) return ids;
        int count = (int)(payload->DataSize / sizeof(int));
        const int* raw = static_cast<const int*>(payload->Data);
        ids.reserve(count);
        for (int i = 0; i < count; ++i) {
            int id = raw[i];
            if (std::find(ids.begin(), ids.end(), id) == ids.end())
                ids.push_back(id);
        }
        return ids;
    }

    std::vector<int> _top_level_dragged_ids(EditorState& st, const std::vector<int>& ids) {
        std::vector<int> out;
        for (int id : ids) {
            bool has_parent_in_set = false;
            for (int other : ids) {
                if (other == id) continue;
                if (st.is_descendant_of(id, other)) { has_parent_in_set = true; break; }
            }
            if (!has_parent_in_set) out.push_back(id);
        }
        return out;
    }

    // Applies a drag-drop payload with Unity-like semantics:
    //   - dropping in the middle re-parents as a child
    //   - dropping near the top/bottom inserts before/after the target
    void _drop_onto(EditorState& st, int target_eid, const ImGuiPayload* payload, DropPlacement placement) {
        auto ids = _payload_ids(payload);
        ids = _top_level_dragged_ids(st, ids);
        if (ids.empty()) return;

        int moved = 0, blocked = 0;
        for (int id : ids) {
            if (id == target_eid) continue;

            bool ok = false;
            if (placement == DropPlacement::Inside) {
                ok = st.reparent(id, target_eid, /*keep_world_position=*/true);
            } else {
                ok = st.reparent_relative(id, target_eid,
                    placement == DropPlacement::Before ? EditorState::HierarchyDropMode::Before
                                                       : EditorState::HierarchyDropMode::After,
                    /*keep_world_position=*/true);
            }

            if (ok) ++moved;
            else ++blocked;
        }

        if (moved > 0) {
            std::string target_name = (placement == DropPlacement::Inside)
                ? (target_eid < 0 ? "Scene Root" : (st.find_entity(target_eid) ? st.find_entity(target_eid)->value("name","Entity") : "?"))
                : (target_eid < 0 ? "Scene Root" : (st.find_entity(target_eid) ? st.find_entity(target_eid)->value("name","Entity") : "?"));
            if (placement == DropPlacement::Inside)
                st.log("Parented " + std::to_string(moved) + " entity(ies) under " + target_name + ".");
            else
                st.log("Reordered " + std::to_string(moved) + " entity(ies) around " + target_name + ".");
        }
        if (blocked > 0) st.log_warn("Ignored " + std::to_string(blocked) + " drop(s) that would create a cycle.");
    }

    // Recursively collects an entity and all of its descendants.
    void _collect_with_descendants(EditorState& st, int id, std::vector<int>& out) {
        out.push_back(id);
        for (int kid : st.children_of(id)) _collect_with_descendants(st, kid, out);
    }

    void _duplicate_selected(EditorState& st) {
        // Unity duplicates each selected object together with its entire
        // subtree, keeping the new subtree's internal parent/child links
        // (remapped to fresh ids) while staying attached to the *original*
        // external parent. SceneIO::duplicate_with_descendants() does this
        // per root and is also used by Ctrl+D / Edit > Duplicate so all three
        // entry points behave identically.
        std::vector<int> roots(st.selected_ids.begin(), st.selected_ids.end());
        if (roots.empty()) return;
        st.undo.push_deep(st.entities);
        std::vector<int> new_ids;
        for (int id : roots) {
            int new_root = SceneIO::duplicate_with_descendants(id, st);
            if (new_root >= 0) new_ids.push_back(new_root);
        }
        if (!new_ids.empty()) {
            st.selected_ids = new_ids;
            st.selected_id = new_ids.back();
            st.asset_selection_is_newer = false;
            st.log("Duplicated " + std::to_string(new_ids.size()) + " entity tree(s).");
        }
    }

    void _delete_selected(EditorState& st) {
        // Unity deletes a GameObject's entire subtree along with it.
        std::vector<int> to_del;
        for (int id : st.selected_ids) _collect_with_descendants(st, id, to_del);
        std::sort(to_del.begin(), to_del.end());
        to_del.erase(std::unique(to_del.begin(), to_del.end()), to_del.end());

        st.entities.erase(std::remove_if(st.entities.begin(),st.entities.end(),
            [&](const Entity& e){ return std::find(to_del.begin(),to_del.end(),e.value("id",0))!=to_del.end(); }),
            st.entities.end());
        st.clear_selection();
        st.resync_children_arrays();
        // Same reasoning as in _duplicate_selected: rebuild now, synchronously,
        // so nothing later this frame reads a dangling Entity* for a deleted
        // (or shifted) node out of the transform registry.
        transform::rebuild_registry(st.entities);
        st.log("Deleted " + std::to_string(to_del.size()) + " entity(ies).");
    }
};

// ─── Script field introspection (for the Inspector) ───────────────────────────
// Scripts expose fields via EXPOSE_FIELD(member) inside awake()/Awake(). To
// show those fields (and their default values) in the Inspector even when
// the entity isn't currently running, we spin up one disposable "preview"
// instance per script class (entity/input left null — every script in this
// project already guards those with `entity ? ... : ...`), call its
// awake(), read what it registered, then throw it away. Results are cached
// per class name since constructing one is cheap but doesn't need to happen
// every frame.
namespace script_introspect {

struct FieldInfo { std::string name; scriptfields::FieldType type; Entity default_value; };

inline std::vector<FieldInfo> describe(const std::string& script_name) {
    std::vector<FieldInfo> out;
    auto inst = ScriptRegistry::instance().make(script_name);
    if (!inst) return out;
    try { inst->awake(); } catch (...) { /* a script that requires a live entity to awake() safely
                                            simply won't report fields here — not fatal */ }
    if (auto* fields = scriptfields::InstanceRegistry::instance().fields_for(inst.get())) {
        for (auto& f : *fields) {
            Entity def;
            try { def = f.get(); } catch (...) {}
            out.push_back({f.name, f.type, def});
        }
    }
    return out;
}

inline std::unordered_map<std::string, std::vector<FieldInfo>>& cache() {
    static std::unordered_map<std::string, std::vector<FieldInfo>> c;
    return c;
}

// Cached lookup. Call invalidate(name) (or invalidate_all()) if a script's
// source has just been recompiled and its field set may have changed.
inline const std::vector<FieldInfo>& fields_for(const std::string& script_name) {
    auto& c = cache();
    auto it = c.find(script_name);
    if (it != c.end()) return it->second;
    return c.emplace(script_name, describe(script_name)).first->second;
}

inline void invalidate_all() { cache().clear(); }

} // namespace script_introspect

// ─── Component icons (inline, Unity Inspector-style) ──────────────────────────
namespace comp_icons {

inline void draw(ImDrawList* dl, ImVec2 p, const std::string& ctype) {
    const float S = 14.f;
    float cx = p.x + S * 0.5f, cy = p.y + S * 0.5f;

    if (ctype == "Transform") {
        // Four-arrow move gizmo
        ImU32 col = IM_COL32(230, 180, 60, 255);
        dl->AddLine({cx, p.y+1}, {cx, p.y+S-1}, col, 1.4f);
        dl->AddLine({p.x+1, cy}, {p.x+S-1, cy}, col, 1.4f);
        dl->AddTriangleFilled({cx-2.5f,p.y+3},{cx+2.5f,p.y+3},{cx,p.y+0.5f}, col);
        dl->AddTriangleFilled({cx-2.5f,p.y+S-3},{cx+2.5f,p.y+S-3},{cx,p.y+S-0.5f}, col);
        dl->AddTriangleFilled({p.x+3,cy-2.5f},{p.x+3,cy+2.5f},{p.x+0.5f,cy}, col);
        dl->AddTriangleFilled({p.x+S-3,cy-2.5f},{p.x+S-3,cy+2.5f},{p.x+S-0.5f,cy}, col);
    } else if (ctype == "SpriteRenderer") {
        // Tiny landscape image
        ImU32 frame = IM_COL32(60, 170, 140, 255);
        ImU32 sky   = IM_COL32(90, 160, 230, 255);
        ImU32 mtn   = IM_COL32(90, 200, 160, 255);
        dl->AddRectFilled({p.x+1,p.y+2},{p.x+S-1,p.y+S-2}, sky, 1.f);
        dl->AddTriangleFilled({p.x+2,p.y+S-3},{p.x+S*0.5f,p.y+5},{p.x+S-2,p.y+S-3}, mtn);
        dl->AddRect({p.x+1,p.y+2},{p.x+S-1,p.y+S-2}, frame, 1.f, 0, 1.2f);
    } else if (ctype == "Camera" || ctype == "Camera2D") {
        ImU32 body = IM_COL32(72, 150, 220, 255);
        ImU32 lens = IM_COL32(200, 230, 255, 255);
        dl->AddRectFilled({p.x+1,p.y+3},{p.x+S-3,p.y+S-3}, body, 2.f);
        dl->AddTriangleFilled({p.x+S-3,p.y+4},{p.x+S,p.y+2},{p.x+S,p.y+S-4}, IM_COL32(100,175,240,255));
        dl->AddCircleFilled({p.x+S*0.38f,cy}, S*0.2f, lens);
    } else if (ctype == "Rigidbody" || ctype == "Rigidbody2D") {
        // Mass/weight icon: filled diamond
        ImU32 col = IM_COL32(230, 120, 50, 255);
        dl->AddQuadFilled({cx,p.y+2},{p.x+S-2,cy},{cx,p.y+S-2},{p.x+2,cy}, col);
        dl->AddLine({cx, p.y+S-2}, {cx, p.y+S+1}, col, 1.5f);
    } else if (ctype == "BoxCollider" || ctype == "BoxCollider2D") {
        ImU32 col = IM_COL32(100, 220, 110, 255);
        dl->AddRect({p.x+2,p.y+2},{p.x+S-2,p.y+S-2}, col, 1.f, 0, 2.0f);
    } else if (ctype == "CircleCollider" || ctype == "CircleCollider2D") {
        ImU32 col = IM_COL32(100, 220, 110, 255);
        dl->AddCircle({cx,cy}, S*0.43f, col, 20, 2.0f);
    } else if (ctype == "Script" || ctype == "ScriptComponent") {
        ImU32 col = IM_COL32(100, 170, 255, 255);
        dl->AddLine({p.x+3,cy-S*0.28f},{p.x+0.5f,cy}, col, 1.8f);
        dl->AddLine({p.x+0.5f,cy},{p.x+3,cy+S*0.28f}, col, 1.8f);
        dl->AddLine({p.x+S-3,cy-S*0.28f},{p.x+S-0.5f,cy}, col, 1.8f);
        dl->AddLine({p.x+S-0.5f,cy},{p.x+S-3,cy+S*0.28f}, col, 1.8f);
        dl->AddLine({p.x+S*0.38f,cy+S*0.3f},{p.x+S*0.62f,cy-S*0.3f}, IM_COL32(160,200,255,200), 1.3f);
    } else if (ctype == "AudioSource" || ctype == "AudioListener") {
        ImU32 col = IM_COL32(120, 200, 120, 255);
        dl->AddQuadFilled({p.x+2,p.y+5},{p.x+5,p.y+5},{p.x+5,p.y+S-5},{p.x+2,p.y+S-5}, col);
        dl->AddTriangleFilled({p.x+5,p.y+5},{p.x+8,p.y+3},{p.x+8,p.y+S-3}, col);
        dl->AddCircle({p.x+8,cy}, S*0.22f, IM_COL32(160,230,160,255), 12, 1.1f);
    } else if (ctype == "Light" || ctype == "Light2D" || ctype == "PointLight" || ctype == "DirectionalLight") {
        ImU32 col = IM_COL32(255, 220, 60, 255);
        dl->AddCircleFilled({cx,cy}, S*0.27f, col);
        for (int i=0;i<8;++i){float a=i*3.14159f*2.f/8.f;float r0=S*0.34f,r1=S*0.47f;
            dl->AddLine({cx+cosf(a)*r0,cy+sinf(a)*r0},{cx+cosf(a)*r1,cy+sinf(a)*r1},col,1.4f);}
    } else if (ctype == "ParticleSystem" || ctype == "ParticleEmitter") {
        ImU32 col = IM_COL32(200, 140, 220, 255);
        float dots[5][2]={{.5f,.2f},{.2f,.55f},{.8f,.45f},{.45f,.8f},{.72f,.72f}};
        for(auto& d:dots) dl->AddCircleFilled({p.x+S*d[0],p.y+S*d[1]},1.6f,col);
    } else if (ctype == "Tilemap") {
        ImU32 col = IM_COL32(200, 160, 90, 255);
        float gs = S / 3.2f;
        for(int r=0;r<3;++r) for(int c=0;c<3;++c){
            ImVec2 tl{p.x+1+c*(gs+1),p.y+1+r*(gs+1)};
            dl->AddRect(tl,{tl.x+gs,tl.y+gs},col,0.5f,0,1.f);}
    } else if (ctype == "Animator") {
        // Play triangle
        ImU32 col = IM_COL32(160, 130, 230, 255);
        dl->AddTriangleFilled({p.x+3,p.y+3},{p.x+3,p.y+S-3},{p.x+S-2,cy}, col);
    } else if (ctype == "SpriteMask") {
        // Stencil-style icon: a filled circle clipped by a diamond outline
        ImU32 fill = IM_COL32(210, 130, 230, 255);
        ImU32 outline = IM_COL32(140, 80, 160, 255);
        dl->AddCircleFilled({cx,cy}, S*0.3f, fill, 16);
        dl->AddQuad({cx,p.y+1.5f},{p.x+S-1.5f,cy},{cx,p.y+S-1.5f},{p.x+1.5f,cy}, outline, 1.3f);
    } else if (ctype == "SortingGroup") {
        // Stacked layers icon
        ImU32 col = IM_COL32(120, 190, 220, 255);
        for (int i=0;i<3;++i) {
            float yy = p.y + 3.f + i*3.4f;
            dl->AddLine({p.x+2.5f, yy}, {cx, yy+2.6f}, col, 1.3f);
            dl->AddLine({cx, yy+2.6f}, {p.x+S-2.5f, yy}, col, 1.3f);
        }
    } else {
        // Generic component: doc icon
        ImU32 fill = IM_COL32(80,80,90,255);
        ImU32 out  = IM_COL32(140,142,150,255);
        dl->AddRectFilled({p.x+2,p.y+1},{p.x+S-2,p.y+S-1}, fill, 2.f);
        dl->AddRect({p.x+2,p.y+1},{p.x+S-2,p.y+S-1}, out, 2.f, 0, 1.2f);
        for(int i=0;i<3;++i) dl->AddLine({p.x+4,p.y+4+i*3.f},{p.x+S-4,p.y+4+i*3.f},out,1.f);
    }
}

} // namespace comp_icons

// ─── Known enum-valued component fields ────────────────────────────────────
// Maps a JSON field name to its fixed set of legal values so the Inspector
// can show a Unity-style dropdown instead of a free-text box for fields like
// SpriteRenderer.draw_mode or Rigidbody2D.body_type. Field names are unique
// enough across components in this engine that no component-type lookup is
// needed; if that ever stops being true, key by "ctype.field" instead.
inline const std::vector<std::pair<std::string,std::string>>* enum_field_options(const std::string& field) {
    static const std::vector<std::pair<std::string,std::string>> draw_mode = {
        {"simple","Simple"}, {"tiled","Tiled"}, {"sliced","Sliced"}
    };
    static const std::vector<std::pair<std::string,std::string>> filter_mode = {
        {"point","Point (Pixel Art)"}, {"bilinear","Bilinear (Smooth)"}
    };
    static const std::vector<std::pair<std::string,std::string>> mask_interaction = {
        {"none","None"}, {"visible_inside_mask","Visible Inside Mask"}, {"visible_outside_mask","Visible Outside Mask"}
    };
    static const std::vector<std::pair<std::string,std::string>> body_type = {
        {"dynamic","Dynamic"}, {"kinematic","Kinematic"}, {"static","Static"}
    };
    static const std::vector<std::pair<std::string,std::string>> direction = {
        {"vertical","Vertical"}, {"horizontal","Horizontal"}
    };
    static const std::vector<std::pair<std::string,std::string>> render_mode = {
        {"screen_space","Screen Space"}, {"world_space","World Space"}
    };
    static const std::vector<std::pair<std::string,std::string>> layout_group_type = {
        {"horizontal","Horizontal"}, {"vertical","Vertical"}, {"grid","Grid"}
    };
    static const std::vector<std::pair<std::string,std::string>> child_alignment = {
        {"start","Start"}, {"center","Center"}, {"end","End"}
    };
    if (field == "draw_mode") return &draw_mode;
    if (field == "filter_mode") return &filter_mode;
    if (field == "mask_interaction") return &mask_interaction;
    if (field == "body_type") return &body_type;
    if (field == "direction") return &direction;
    if (field == "render_mode") return &render_mode;
    if (field == "type") return &layout_group_type;
    if (field == "child_alignment") return &child_alignment;
    return nullptr;
}


// ─── Asset icons (flat vector icons, Unity-Project-window style) ──────────────
namespace asset_icons {

inline std::string classify(bool is_dir, const std::string& ext_in) {
    std::string ext = ext_in;
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (is_dir) return "folder";
    if (ext==".png"||ext==".jpg"||ext==".jpeg"||ext==".bmp"||ext==".tga") return "image";
    if (ext==".cpp"||ext==".hpp"||ext==".h"||ext==".c"||ext==".inl") return "script";
    if (ext==".material") return "material";
    if (ext==".json") return "json";
    if (ext==".wav"||ext==".ogg"||ext==".mp3") return "audio";
    if (ext==".prefab") return "prefab";
    return "file";
}

// Plain document silhouette (folded top-right corner), used as the base for
// most file-type icons.
inline void doc_base(ImDrawList* dl, ImVec2 p, ImVec2 s, ImU32 fill, ImU32 outline, ImU32 cut_color) {
    float fold = s.x * 0.30f;
    ImVec2 mn{p.x + s.x*0.16f, p.y};
    ImVec2 mx{p.x + s.x*0.84f, p.y + s.y};
    dl->AddRectFilled(mn, mx, fill, 2.0f);
    ImVec2 t1{mx.x - fold, mn.y};
    ImVec2 t2{mx.x, mn.y + fold};
    dl->AddTriangleFilled(t1, ImVec2(mx.x, mn.y), t2, cut_color);
    dl->AddLine(t1, t2, outline, 1.3f);
    dl->AddRect(mn, mx, outline, 2.0f, 0, 1.3f);
}

inline void draw(ImDrawList* dl, ImVec2 pos, ImVec2 size, const std::string& kind, ImU32 bg) {
    float w = size.x, h = size.y;

    if (kind == "folder") {
        ImU32 body = IM_COL32(233, 188, 101, 255);
        ImU32 dark = IM_COL32(189, 145, 65, 255);
        ImVec2 tab_mn{pos.x + w*0.06f, pos.y + h*0.16f};
        ImVec2 tab_mx{pos.x + w*0.46f, pos.y + h*0.30f};
        dl->AddRectFilled(tab_mn, tab_mx, body, 2.0f);
        ImVec2 body_mn{pos.x + w*0.06f, pos.y + h*0.26f};
        ImVec2 body_mx{pos.x + w*0.94f, pos.y + h*0.86f};
        dl->AddRectFilled(body_mn, body_mx, body, 2.5f);
        dl->AddRect(body_mn, body_mx, dark, 2.5f, 0, 1.4f);
        return;
    }

    ImU32 outline = IM_COL32(40, 40, 44, 255);

    if (kind == "image") {
        ImU32 frame = IM_COL32(70, 75, 84, 255);
        ImU32 art   = IM_COL32(108, 196, 150, 255);
        ImU32 sun   = IM_COL32(247, 209, 92, 255);
        ImVec2 mn{pos.x + w*0.08f, pos.y + h*0.10f};
        ImVec2 mx{pos.x + w*0.92f, pos.y + h*0.88f};
        dl->AddRectFilled(mn, mx, frame, 2.0f);
        dl->AddRect(mn, mx, outline, 2.0f, 0, 1.3f);
        dl->AddCircleFilled({pos.x + w*0.30f, pos.y + h*0.30f}, w*0.07f, sun);
        dl->AddTriangleFilled({mn.x+2, mx.y-2}, {pos.x+w*0.45f, pos.y+h*0.42f}, {pos.x+w*0.62f, mx.y-2}, art);
        dl->AddTriangleFilled({pos.x+w*0.50f, mx.y-2}, {pos.x+w*0.70f, pos.y+h*0.32f}, {mx.x-2, mx.y-2}, art);
        return;
    }

    if (kind == "script") {
        doc_base(dl, pos, size, IM_COL32(58, 60, 68, 255), outline, bg);
        ImU32 accent = IM_COL32(99, 169, 247, 255);
        float cy = pos.y + h*0.58f, dx = w*0.10f;
        ImVec2 lc{pos.x + w*0.30f, cy};
        dl->AddLine({lc.x+dx, cy-dx}, {lc.x-dx*0.3f, cy}, accent, 1.8f);
        dl->AddLine({lc.x-dx*0.3f, cy}, {lc.x+dx, cy+dx}, accent, 1.8f);
        ImVec2 rc{pos.x + w*0.62f, cy};
        dl->AddLine({rc.x-dx, cy-dx}, {rc.x+dx*0.3f, cy}, accent, 1.8f);
        dl->AddLine({rc.x+dx*0.3f, cy}, {rc.x-dx, cy+dx}, accent, 1.8f);
        return;
    }

    if (kind == "json") {
        doc_base(dl, pos, size, IM_COL32(58, 60, 68, 255), outline, bg);
        ImU32 accent = IM_COL32(232, 165, 84, 255);
        ImVec2 ts = ImGui::CalcTextSize("{ }");
        dl->AddText({pos.x + (w-ts.x)*0.5f, pos.y + h*0.50f - ts.y*0.5f}, accent, "{ }");
        return;
    }

    if (kind == "audio") {
        doc_base(dl, pos, size, IM_COL32(58, 60, 68, 255), outline, bg);
        ImU32 accent = IM_COL32(146, 196, 109, 255);
        float bx = pos.x + w*0.32f, by = pos.y + h*0.62f;
        float bw = w*0.09f, gap = w*0.13f;
        float heights[4] = {0.14f, 0.30f, 0.20f, 0.10f};
        for (int i = 0; i < 4; ++i) {
            float bh = h * heights[i];
            dl->AddRectFilled({bx + gap*i, by - bh}, {bx + gap*i + bw, by}, accent, 1.0f);
        }
        return;
    }

    if (kind == "material") {
        // Unity-style material thumbnail: a tinted sphere on a checkered
        // background, so it's instantly distinguishable from a script/json
        // file at a glance in the grid.
        ImU32 chk_a = IM_COL32(64,64,68,255), chk_b = IM_COL32(80,80,86,255);
        ImVec2 mn{pos.x + w*0.08f, pos.y + h*0.10f};
        ImVec2 mx{pos.x + w*0.92f, pos.y + h*0.88f};
        int cells = 4;
        float cw = (mx.x-mn.x)/cells, ch = (mx.y-mn.y)/cells;
        for (int yy=0; yy<cells; ++yy)
            for (int xx=0; xx<cells; ++xx)
                dl->AddRectFilled({mn.x+xx*cw, mn.y+yy*ch}, {mn.x+(xx+1)*cw, mn.y+(yy+1)*ch},
                                   ((xx+yy)%2==0) ? chk_a : chk_b);
        dl->AddRect(mn, mx, outline, 2.0f, 0, 1.3f);
        ImVec2 c{(mn.x+mx.x)*0.5f, (mn.y+mx.y)*0.5f};
        float r = std::min(mx.x-mn.x, mx.y-mn.y) * 0.34f;
        dl->AddCircleFilled(c, r, IM_COL32(235,90,90,255));
        dl->AddCircleFilled({c.x - r*0.32f, c.y - r*0.32f}, r*0.32f, IM_COL32(255,180,170,200));
        return;
    }

    if (kind == "prefab") {
        doc_base(dl, pos, size, IM_COL32(58, 60, 68, 255), outline, bg);
        ImU32 top = IM_COL32(146, 186, 255, 255);
        ImU32 left = IM_COL32(76, 116, 209, 255);
        ImU32 right = IM_COL32(108, 150, 235, 255);
        float cx = pos.x + w*0.5f;
        ImVec2 T{cx, pos.y + h*0.34f}, R{pos.x + w*0.76f, pos.y + h*0.46f},
               B{cx, pos.y + h*0.58f}, L{pos.x + w*0.24f, pos.y + h*0.46f};
        ImVec2 Bb{cx, pos.y + h*0.78f}, Lb{L.x, pos.y + h*0.66f}, Rb{R.x, pos.y + h*0.66f};
        ImVec2 top_pts[4] = {T, R, B, L};
        dl->AddConvexPolyFilled(top_pts, 4, top);
        ImVec2 left_pts[4] = {L, B, Bb, Lb};
        dl->AddConvexPolyFilled(left_pts, 4, left);
        ImVec2 right_pts[4] = {B, R, Rb, Bb};
        dl->AddConvexPolyFilled(right_pts, 4, right);
        return;
    }

    // generic file
    doc_base(dl, pos, size, IM_COL32(58, 60, 68, 255), outline, bg);
    ImU32 line_col = IM_COL32(140, 142, 150, 255);
    for (int i = 0; i < 3; ++i) {
        float ly = pos.y + h*0.42f + i * h*0.13f;
        dl->AddLine({pos.x + w*0.30f, ly}, {pos.x + w*0.70f, ly}, line_col, 1.3f);
    }
}

} // namespace asset_icons

// ─── InspectorPanel ──────────────────────────────────────────────────────────
class InspectorPanel {
    char _add_comp_filter[64] = {};
    bool _show_add_popup = false;
    char _add_script_buf[128] = {};
    // The Tile Palette owns Vulkan thumbnail resources, while the Inspector
    // owns the Tilemap component UI.  Keeping this as a callback avoids a
    // second, stale palette implementation and lets the Inspector expose the
    // same visual multi-select grid as the dedicated palette window.
    std::function<void(EditorState&, Entity&)> _tile_palette_picker;

    // Backs the Material Inspector (see _draw_asset_inspector / _draw_material_inspector
    // below) — independent from RenderSystem's own MaterialCache since this
    // panel has no renderer handle, but resolves paths identically (same
    // base_dir-relative lookup), so edits saved here are picked up by the
    // live viewport's cache on its own mtime check next frame.
    material::MaterialCache _mat_cache;
    std::string _mat_cache_dir;

    static bool _create_empty_visual_script_asset(const fs::path& path, int owner_id,
                                                   const std::string& owner_name) {
        // Use the shared atomic writer. A graph file is a runtime asset, so a
        // half-written file during autosave/antivirus activity must never be
        // presented as a valid, but empty, behaviour.
        const nlohmann::json blank = {
            {"format", "gameengine.visual-script"}, {"version", 3},
            {"owner", {{"entity_id", owner_id}, {"entity_name", owner_name}}},
            {"next_id", 1}, {"next_pin_id", 1},
            {"nodes", nlohmann::json::array()}, {"links", nlohmann::json::array()},
            {"variables", nlohmann::json::array()}
        };
        const fs::path temporary = path.string() + ".tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out << blank.dump(2) << '\n';
            if (!out.good()) { out.close(); std::error_code ignored; fs::remove(temporary, ignored); return false; }
        }
#if defined(_WIN32)
        // MoveFileEx replacement is atomic from the reader's perspective on
        // NTFS and avoids the transient missing-file window of remove+rename.
        if (MoveFileExA(temporary.string().c_str(), path.string().c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        std::error_code ignored; fs::remove(temporary, ignored); return false;
#else
        std::error_code ec; fs::rename(temporary, path, ec);
        if (!ec) return true;
        fs::remove(temporary, ec); return false;
#endif
    }

    static bool _has_allowed_extension(const fs::path& path,
                                       std::initializer_list<const char*> extensions) {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        for (const char* allowed : extensions) if (ext == allowed) return true;
        return false;
    }

    static std::string _project_asset_reference(const EditorState& st, const fs::path& chosen) {
        // Project-panel payloads are already relative to Assets. Preserve that
        // form instead of resolving it against the editor process directory.
        if (!chosen.is_absolute()) return chosen.generic_string();
        std::error_code ec;
        const fs::path relative = fs::relative(chosen, fs::path(st.asset_dir), ec);
        if (!ec && !relative.empty() && *relative.begin() != fs::path(".."))
            return relative.generic_string();
        return chosen.generic_string();
    }

    static std::string _browse_asset_file(const EditorState& st, const char* title, const char* filter) {
#if defined(_WIN32)
        char file_buf[MAX_PATH * 4] = {};
        OPENFILENAMEA ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = nullptr;
        ofn.lpstrFilter = filter;
        ofn.lpstrFile = file_buf;
        ofn.nMaxFile = static_cast<DWORD>(sizeof(file_buf));
        ofn.lpstrTitle = title;
        ofn.lpstrInitialDir = st.asset_dir.empty() ? nullptr : st.asset_dir.c_str();
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        if (GetOpenFileNameA(&ofn)) return file_buf;
#else
        (void)st; (void)title; (void)filter;
#endif
        return {};
    }

    // Reference fields never expose an editable raw path.  They accept a
    // compatible Project drag or open the normal OS browser, and retain only
    // project-relative references where possible so exports remain portable.
    static bool _draw_asset_slot(EditorState& st, const char* label, std::string& value,
                                 std::initializer_list<const char*> extensions,
                                 const char* browser_filter, const char* browser_title) {
        bool changed = false;
        ImGui::PushID(label);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        const std::string display = value.empty() ? "(None)" : fs::path(value).filename().string();
        ImGui::Button(display.c_str(), {std::max(100.f, ImGui::GetContentRegionAvail().x - 92.f), 0.f});
        if (ImGui::IsItemHovered() && !value.empty()) ImGui::SetTooltip("%s", value.c_str());
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                const fs::path dropped(static_cast<const char*>(pl->Data));
                if (_has_allowed_extension(dropped, extensions)) {
                    value = _project_asset_reference(st, dropped);
                    changed = true;
                } else {
                    st.log_warn(std::string("Rejected incompatible asset for ") + label + ": " + dropped.filename().string());
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine(0, 4);
        if (ImGui::SmallButton("Browse...")) {
            const std::string picked = _browse_asset_file(st, browser_title, browser_filter);
            if (!picked.empty()) {
                const fs::path path(picked);
                if (_has_allowed_extension(path, extensions)) { value = _project_asset_reference(st, path); changed = true; }
                else st.log_warn(std::string("Rejected incompatible file for ") + label + ".");
            }
        }
        if (!value.empty()) {
            ImGui::SameLine(0, 3);
            if (ImGui::SmallButton("X")) { value.clear(); changed = true; }
        }
        ImGui::PopID();
        return changed;
    }

    // Unity-style "Parent" combo at the top of the Transform inspector.
    // Choosing an entry reparents the current selection under it, preserving
    // world-space position/rotation/scale (same default as dragging in the
    // Hierarchy window). Entities that are the current entity itself, or one
    // of its own descendants, are excluded to prevent creating a cycle.
    void _draw_parent_picker(EditorState& st, Entity& e) {
        int eid = e.value("id", 0);
        int cur_parent = EditorState::parent_of(e);
        Entity* cur_parent_ep = cur_parent >= 0 ? st.find_entity(cur_parent) : nullptr;
        std::string cur_label = cur_parent_ep ? cur_parent_ep->value("name","Entity") : "None";

        ImGui::SetNextItemWidth(200);
        if (ImGui::BeginCombo("Parent", cur_label.c_str())) {
            bool none_selected = (cur_parent < 0);
            if (ImGui::Selectable("None", none_selected)) {
                if (cur_parent >= 0) {
                    st.reparent(eid, -1);
                    st.log(std::string("Unparented \"") + e.value("name", std::string("Entity")) + "\".");
                }
            }
            ImGui::Separator();
            for (auto& cand : st.entities) {
                if (cand.value("_runtime_only", false)) continue;
                int cid = cand.value("id", 0);
                if (cid == eid) continue;                          // can't parent to self
                if (st.is_descendant_of(cid, eid)) continue;        // would create a cycle
                std::string cname = cand.value("name", "Entity");
                std::string label = cname + "  [" + std::to_string(cid) + "]";
                bool sel = (cid == cur_parent);
                if (ImGui::Selectable(label.c_str(), sel)) {
                    if (cid != cur_parent) {
                        st.reparent(eid, cid);
                        st.log(std::string("Parented \"") + e.value("name", std::string("Entity")) + "\" under \"" + cname + "\".");
                    }
                }
            }
            ImGui::EndCombo();
        }
        if (cur_parent_ep) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Unparent")) { st.reparent(eid, -1); }
        }
        ImGui::Spacing();
    }

    // Collects every script name currently attached to this Script/ScriptComponent
    // component (the legacy single "class_name" slot plus the "scripts" array),
    // in attachment order, without duplicates.
    static std::vector<std::string> _attached_script_names(Entity& comp) {
        std::vector<std::string> out;
        auto add = [&](const std::string& s){
            if (s.empty()) return;
            if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
        };
        if (comp.contains("class_name") && comp["class_name"].is_string())
            add(comp["class_name"].get<std::string>());
        if (comp.contains("scripts") && comp["scripts"].is_array())
            for (auto& s : comp["scripts"]) if (s.is_string()) add(s.get<std::string>());
        return out;
    }

    // Removes `sname` from wherever it's attached (the legacy class_name slot
    // and/or the scripts array) and drops its saved field overrides.
    static void _detach_script(Entity& comp, const std::string& sname) {
        if (comp.contains("class_name") && comp["class_name"].is_string() &&
            comp["class_name"].get<std::string>() == sname)
            comp["class_name"] = std::string("");
        if (comp.contains("scripts") && comp["scripts"].is_array()) {
            auto& arr = comp["scripts"];
            Entity kept = Entity::array();
            for (auto& s : arr) {
                if (s.is_string() && s.get<std::string>() == sname) continue;
                kept.push_back(s);
            }
            arr = kept;
        }
        if (comp.contains("field_overrides")) comp["field_overrides"].erase(sname);
    }

    // Draws one attached script: a header row (name + remove button) and,
    // beneath it, every field that script exposed via EXPOSE_FIELD — editable
    // exactly like a Unity public field. Edits write into
    // Script.field_overrides[sname][field], which ScriptSystem re-applies to
    // the live instance every frame.
    void _draw_script_fields(EditorState& st, Entity& comp, const std::string& sname) {
        const auto& fields = script_introspect::fields_for(sname);
        bool known = ScriptRegistry::instance().has(sname);

        if (!known) {
            ImGui::TextColored({1.f,0.55f,0.3f,1.f}, "  (no C++ class registered for \"%s\")", sname.c_str());
            ImGui::TextDisabled("  Tip: use the class name (e.g. \"MyScript\"), not the");
            ImGui::TextDisabled("  filename. Or click Browse Scripts... to pick one.");
            return;
        }
        if (fields.empty()) {
            ImGui::TextDisabled("  (no exposed fields — add EXPOSE_FIELD(member) in Awake())");
            return;
        }

        auto& overrides = comp["field_overrides"][sname];
        if (overrides.is_null()) overrides = Entity::object();
        for (auto& f : fields) {
            Entity cur = overrides.contains(f.name) ? overrides[f.name] : f.default_value;
            std::string lbl = f.name;
            std::replace(lbl.begin(), lbl.end(), '_', ' ');
            ImGui::PushID(f.name.c_str());
            ImGui::Indent(12.f);
            ImGui::SetNextItemWidth(140);

            switch (f.type) {
                case scriptfields::FieldType::Bool: {
                    bool v = cur.is_boolean() ? cur.get<bool>() : false;
                    if (ImGui::Checkbox(lbl.c_str(), &v)) overrides[f.name] = v;
                    break;
                }
                case scriptfields::FieldType::Int: {
                    int v = cur.is_number() ? cur.get<int>() : 0;
                    if (ImGui::InputInt(lbl.c_str(), &v)) overrides[f.name] = v;
                    break;
                }
                case scriptfields::FieldType::Float: {
                    float v = cur.is_number() ? cur.get<float>() : 0.f;
                    if (ImGui::DragFloat(lbl.c_str(), &v, 0.1f)) overrides[f.name] = v;
                    break;
                }
                case scriptfields::FieldType::String: {
                    std::string sv = cur.is_string() ? cur.get<std::string>() : "";
                    char buf[256]; snprintf(buf, sizeof(buf), "%s", sv.c_str());
                    ImGui::SetNextItemWidth(200);
                    if (ImGui::InputText(lbl.c_str(), buf, sizeof(buf))) overrides[f.name] = std::string(buf);
                    break;
                }
            }

            // Right-click a field to reset it back to the script's default.
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Reset to Default")) overrides.erase(f.name);
                ImGui::EndPopup();
            }
            ImGui::Unindent(12.f);
            ImGui::PopID();
        }
        if (overrides.is_object() && overrides.empty()) comp["field_overrides"].erase(sname);
    }

    // Full Script/ScriptComponent panel: list of attached scripts (each
    // expandable to show its public fields) + an Enter-to-add field.
    void _draw_script_component(EditorState& st, Entity& comp, const ImVec2& card_top_left) {
        auto names = _attached_script_names(comp);

        // Script assets are not ordinary strings: a .cpp drop must resolve
        // its real ScriptBase class and must belong to this game's scripts
        // folder.  This makes ScriptComponent a proper compatible Inspector
        // drop target instead of forcing users to type a class name.
        auto attach_script = [&](const std::string& sname, const fs::path& source) {
            if (sname.empty()) return;
            if (std::find(names.begin(), names.end(), sname) != names.end()) {
                st.log_warn("Script already attached: " + sname);
                return;
            }
            if (!comp.contains("scripts") || !comp["scripts"].is_array())
                comp["scripts"] = nlohmann::json::array();
            comp["scripts"].push_back(sname);
            names.push_back(sname);
            if (ScriptRegistry::instance().has(sname)) {
                st.log("Attached script: " + sname);
            } else {
                st.log_warn("Attached \"" + sname + "\" from " + source.filename().string() +
                            " — it will become live when its one-file reload completes.");
            }
        };

        auto attach_dropped_script = [&](const char* raw_path) {
            if (!raw_path || !*raw_path) return;
            const fs::path root = find_engine_project_root();
            fs::path source(raw_path);
            std::error_code ec;
            if (source.is_relative()) source = (root / source).lexically_normal();
            source = fs::absolute(source, ec).lexically_normal();
            if (ec || !fs::is_regular_file(source, ec)) {
                st.log_warn("Dropped script could not be found.");
                return;
            }
            std::string ext = source.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (ext != ".cpp") {
                st.log_warn("Rejected incompatible asset for Script Component: " + source.filename().string() +
                            " (drop a .cpp ScriptBase file).");
                return;
            }
            const fs::path project_scripts = fs::absolute(fs::path(st.scene_path).parent_path() / "scripts", ec).lexically_normal();
            const fs::path relative = fs::relative(source, project_scripts, ec);
            if (ec || relative.empty() || *relative.begin() == fs::path("..")) {
                st.log_warn("Rejected script outside this project's scripts folder: " + source.filename().string());
                return;
            }
            std::ifstream in(source);
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const auto classes = extract_script_class_names(text);
            if (classes.empty()) {
                st.log_warn("Rejected \"" + source.filename().string() +
                            "\": no class deriving from ScriptBase or MonoBehaviour was found.");
                return;
            }
            for (const auto& class_name : classes) attach_script(class_name, source);
        };

        if (names.empty()) {
            ImGui::TextDisabled("No scripts attached.");
        }

        std::string to_detach;
        for (auto& sname : names) {
            ImGui::PushID(sname.c_str());
            bool node_open = ImGui::TreeNodeEx(sname.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed);
            ImGui::SameLine(ImGui::GetWindowWidth() - 40.f);
            if (ImGui::SmallButton("X")) to_detach = sname;
            if (node_open) {
                _draw_script_fields(st, comp, sname);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (!to_detach.empty()) {
            _detach_script(comp, to_detach);
            st.log("Removed script: " + to_detach);
        }

        ImGui::Spacing();
        ImGui::SetNextItemWidth(200);
        bool enter_pressed = ImGui::InputTextWithHint("##add_script", "Type script name, press Enter",
                                                        _add_script_buf, sizeof(_add_script_buf),
                                                        ImGuiInputTextFlags_EnterReturnsTrue);
        bool add_clicked = false;
        ImGui::SameLine();
        add_clicked = ImGui::SmallButton("Add");

        if (enter_pressed || add_clicked) {
            std::string sname = _add_script_buf;
            // trim whitespace
            while (!sname.empty() && std::isspace((unsigned char)sname.front())) sname.erase(sname.begin());
            while (!sname.empty() && std::isspace((unsigned char)sname.back())) sname.pop_back();

            if (sname.empty()) {
                // nothing typed — ignore
            } else if (std::find(names.begin(), names.end(), sname) != names.end()) {
                st.log_warn("Script already attached: " + sname);
            } else {
                if (!comp.contains("scripts") || !comp["scripts"].is_array())
                    comp["scripts"] = nlohmann::json::array();
                comp["scripts"].push_back(sname);
                if (!ScriptRegistry::instance().has(sname))
                    st.log_warn("Added \"" + sname + "\" — no C++ class with that name is registered. Make sure the class name inside the .cpp matches what you typed (use Browse Scripts... to pick from registered classes).");
                else
                    st.log("Attached script: " + sname);
                _add_script_buf[0] = '\0';
            }
        }

        ImGui::Spacing();
        ImGui::Button("Drop a .cpp Script Here##script_asset_drop", ImVec2(-1.0f, 28.0f));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Drag a ScriptBase/MonoBehaviour .cpp from Project > Scripts.\nOnly scripts in this project's scripts folder are accepted.");
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
                attach_dropped_script(static_cast<const char*>(payload->Data));
            ImGui::EndDragDropTarget();
        }

        // "Browse Scripts..." opens a native OS file picker so the user can
        // select a .cpp from their project folder.  The class name is extracted
        // from the file content and attached — no typing needed.
        if (ImGui::SmallButton("Browse Scripts...")) {
            std::string picked_class;
#if defined(_WIN32)
            char file_buf[MAX_PATH * 4] = {};
            std::string script_dir_str = st.scene_path.empty()
                ? fs::current_path().string()
                : (fs::path(st.scene_path).parent_path() / "scripts").string();

            OPENFILENAMEA ofn{};
            ofn.lStructSize  = sizeof(ofn);
            ofn.hwndOwner    = nullptr;
            ofn.lpstrFilter  = "C++ Script Files (*.cpp)\0*.cpp\0All Files (*.*)\0*.*\0\0";
            ofn.lpstrFile    = file_buf;
            ofn.nMaxFile     = static_cast<DWORD>(sizeof(file_buf));
            ofn.lpstrTitle   = "Select Script File";
            ofn.lpstrInitialDir = script_dir_str.c_str();
            ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST
                             | OFN_HIDEREADONLY  | OFN_NOCHANGEDIR;

            if (GetOpenFileNameA(&ofn)) {
                // Read the file and extract the first script class name
                std::ifstream f(file_buf);
                std::string src((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
                auto classes = extract_script_class_names(src);
                if (!classes.empty()) {
                    picked_class = classes.front();
                } else {
                    // Fall back to the file stem if no class found
                    picked_class = fs::path(file_buf).stem().string();
                    st.log_warn("Could not find a ScriptBase/MonoBehaviour class in that file. Using filename \"" + picked_class + "\" — make sure the class name matches.");
                }
            }
#else
            // On non-Windows, fall back to an ImGui popup listing registered scripts
            ImGui::OpenPopup("##browse_scripts_fallback");
#endif
            if (!picked_class.empty()) {
                if (std::find(names.begin(), names.end(), picked_class) != names.end()) {
                    st.log_warn("Script \"" + picked_class + "\" is already attached.");
                } else {
                    if (!comp.contains("scripts") || !comp["scripts"].is_array())
                        comp["scripts"] = nlohmann::json::array();
                    comp["scripts"].push_back(picked_class);
                    if (!ScriptRegistry::instance().has(picked_class))
                        st.log_warn("Attached \"" + picked_class + "\" — not registered yet. It'll be picked up automatically on the next Play, or click \"Rebuild Scripts\" in the toolbar.");
                    else
                        st.log("Attached script: " + picked_class);
                }
            }
        }
        // Fallback popup for non-Windows: list registered scripts
        if (ImGui::BeginPopup("##browse_scripts_fallback")) {
            auto all = ScriptRegistry::instance().all_names();
            if (all.empty()) {
                ImGui::TextDisabled("No scripts registered yet.");
            }
            for (auto& n : all) {
                if (std::find(names.begin(), names.end(), n) != names.end()) continue;
                if (ImGui::Selectable(n.c_str())) {
                    if (!comp.contains("scripts") || !comp["scripts"].is_array())
                        comp["scripts"] = nlohmann::json::array();
                    comp["scripts"].push_back(n);
                    st.log("Attached script: " + n);
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }

        // The Script component must accept a compatible asset across its
        // entire inspector card, not just on the small helper button above.
        // BeginDragDropTargetCustom only participates while a payload is
        // actively dragged, so normal controls remain fully interactive.  A
        // stable, component-local ID also prevents an asset drop intended for
        // a neighbouring component from being consumed here.
        const ImRect whole_card(card_top_left, ImGui::GetCursorScreenPos());
        if (whole_card.GetHeight() > 1.0f &&
            ImGui::BeginDragDropTargetCustom(whole_card, ImGui::GetID("##script_component_full_card_drop"))) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
                attach_dropped_script(static_cast<const char*>(payload->Data));
            ImGui::EndDragDropTarget();
        }
    }

    // Generic flat-array keyframe curve editor. Curve is stored as
    // [t0,v0..vN-1, t1,v0..vN-1, ...] (stride = 1+n_components). Renders one
    // row per keyframe (a time field + one field per component) with
    // add/remove buttons — the minimal "AnimationCurve" equivalent without a
    // draggable graph widget.
    void _draw_curve_editor(Entity& comp, const char* key, int n_components,
                             std::initializer_list<const char*> comp_labels) {
        if (!comp.contains(key) || !comp[key].is_array()) comp[key] = nlohmann::json::array();
        auto& curve = comp[key];
        int stride = 1 + n_components;
        int count = (int)curve.size() / stride;

        std::vector<const char*> labels(comp_labels.begin(), comp_labels.end());
        int remove_idx = -1;
        for (int i = 0; i < count; ++i) {
            ImGui::PushID(i);
            float t = curve[i*stride].template get<float>();
            ImGui::SetNextItemWidth(60);
            if (ImGui::DragFloat("t", &t, 0.01f, 0.f, 1.f, "%.2f")) curve[i*stride] = t;
            for (int c = 0; c < n_components; ++c) {
                ImGui::SameLine();
                float v = curve[i*stride + 1 + c].template get<float>();
                ImGui::SetNextItemWidth(50);
                const char* lbl = c < (int)labels.size() ? labels[c] : "v";
                if (ImGui::DragFloat(lbl, &v, n_components==4 ? 1.f : 0.1f)) curve[i*stride + 1 + c] = v;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) remove_idx = i;
            ImGui::PopID();
        }
        if (remove_idx >= 0) {
            nlohmann::json new_curve = nlohmann::json::array();
            for (int i = 0; i < count; ++i) {
                if (i == remove_idx) continue;
                for (int s = 0; s < stride; ++s) new_curve.push_back(curve[i*stride+s]);
            }
            curve = new_curve;
        }
        if (ImGui::SmallButton(("+ Add Keyframe##"+std::string(key)).c_str())) {
            float last_t = count > 0 ? curve[(count-1)*stride].template get<float>() : 0.f;
            curve.push_back(std::min(1.f, last_t + (count==0?0.f:0.25f)));
            for (int c = 0; c < n_components; ++c) curve.push_back(n_components==4 ? 255.f : 10.f);
        }
    }

    // Draw a single component section. Returns true if component was removed.
    bool _draw_component(EditorState& st, Entity& e, const std::string& ctype) {
        auto& comp = e["components"][ctype];
        std::string dname = component_display_name(ctype);

        // Two spaces give a visible gap; the component icon is painted on top
        // via DrawList once the header rect is known — same technique as the
        // Hierarchy row icons, but using CollapsingHeader instead of TreeNode.
        std::string header_label = "  " + dname + "##comp_" + ctype;
        // Reserve the right edge of the header for real enabled/remove
        // controls.  CollapsingHeader normally owns the entire row, which
        // made the old trailing X visually visible but hit-tested as part of
        // the header (the Script component in particular could not be
        // removed).  Allow overlap deliberately, then place the controls
        // inside the header's rect and restore the cursor below it.
        bool open = ImGui::CollapsingHeader(header_label.c_str(),
            ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
        const ImVec2 header_min = ImGui::GetItemRectMin();
        const ImVec2 header_max = ImGui::GetItemRectMax();
        const ImVec2 cursor_after_header = ImGui::GetCursorScreenPos();
        const float control_size = std::max(14.f, header_max.y - header_min.y - 4.f);
        float control_right = header_max.x - 4.f;
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 item_min = ImGui::GetItemRectMin();
            float  item_h   = ImGui::GetItemRectSize().y;
            float  arrow_w  = ImGui::GetTreeNodeToLabelSpacing();
            ImVec2 icon_p{item_min.x + arrow_w + 2.f, item_min.y + (item_h - 14.f) * 0.5f};
            comp_icons::draw(dl, icon_p, ctype);
        }
        const bool header_hovered = ImGui::IsItemHovered();
        bool removed = false;

        if (!is_required_component(ctype)) {
            control_right -= control_size;
            ImGui::SetCursorScreenPos({control_right, header_min.y + 2.f});
            ImGui::PushID((ctype + "_rmv").c_str());
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.56f,0.18f,0.18f,0.92f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.78f,0.25f,0.25f,1.f));
            if (ImGui::Button("x##remove_component", {control_size, control_size})) removed = true;
            const bool remove_hovered = ImGui::IsItemHovered();
            if (remove_hovered) ImGui::SetTooltip("Remove %s", dname.c_str());
            ImGui::PopStyleColor(2);
            ImGui::PopID();
            control_right -= 4.f;
        }

        // Enabled state belongs beside removal rather than after the header,
        // preventing a second overlapping hit target on narrow inspectors.
        if (comp.is_object() && comp.contains("enabled") && comp["enabled"].is_boolean()) {
            bool en = comp["enabled"].get<bool>();
            control_right -= control_size;
            ImGui::SetCursorScreenPos({control_right, header_min.y + 2.f});
            ImGui::PushID((ctype + "_en").c_str());
            if (ImGui::Checkbox("##enabled_component", &en)) comp["enabled"] = en;
            ImGui::PopID();
        }
        ImGui::SetCursorScreenPos(cursor_after_header);

        // Avoid showing the component-description tooltip over the actionable
        // buttons: tooltips must never make a remove target feel obstructed.
        const ImRect header_description_rect(header_min, {control_right, header_max.y});
        if (header_hovered && ImGui::IsMouseHoveringRect(header_description_rect.Min, header_description_rect.Max)) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(component_description(ctype).c_str());
            if (const auto* descriptor = component_descriptor(ctype)) {
                ImGui::TextDisabled("%s · %s", descriptor->category.c_str(), component_support_label(descriptor->support));
                if (!descriptor->tool_hint.empty()) ImGui::TextDisabled("Related tool: %s", descriptor->tool_hint.c_str());
            }
            ImGui::EndTooltip();
        }

        // Right-click on the component header: Reset / Remove
        if (ImGui::BeginPopupContextItem((ctype+"_ctx").c_str())) {
            if (ImGui::MenuItem("Reset to Defaults")) {
                if (component_defaults().contains(ctype)) {
                    comp = component_defaults()[ctype];
                    st.log("Reset component: " + ctype);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Copy Component")) {
                st._comp_clipboard_type = ctype;
                st._comp_clipboard_data = comp;
            }
            if (!st._comp_clipboard_type.empty() && st._comp_clipboard_type == ctype) {
                if (ImGui::MenuItem("Paste Component Values")) {
                    comp = st._comp_clipboard_data;
                    st.log("Pasted component values: " + ctype);
                }
            }
            ImGui::Separator();
            if (!is_required_component(ctype) && ImGui::MenuItem("Remove Component"))
                removed = true;
            ImGui::EndPopup();
        }

        if (!open || removed) return removed;

        ImGui::PushID(ctype.c_str());
        ImGui::Indent(8.f);

        bool transform_changed = false;
        if (ctype == "Transform") _draw_parent_picker(st, e);

        if (ctype == "Script" || ctype == "ScriptComponent") {
            _draw_script_component(st, comp, header_min);
            ImGui::Unindent(8.f);
            ImGui::PopID();
            return false;
        }

        if (ctype == "VisualScript") {
            bool enabled = comp.value("enabled", true);
            if (ImGui::Checkbox("Enabled##vscript", &enabled)) comp["enabled"] = enabled;
            bool run_on_start = comp.value("run_on_start", true);
            ImGui::SameLine(0, 12);
            if (ImGui::Checkbox("Run On Start##vscript", &run_on_start)) comp["run_on_start"] = run_on_start;

            std::string asset = comp.value("asset", std::string());
            if (_draw_asset_slot(st, "Graph Asset", asset, {".json"},
                                 "Visual Script (*.json)\0*.json\0\0", "Select Visual Script"))
                comp["asset"] = asset;
            ImGui::TextDisabled("Drag a graph from Project or use Browse. Graphs are stored relative to Assets.");

            if (ImGui::Button("Create Graph Asset##vscript")) {
                std::string stem = e.value("name", "entity");
                for (char& ch : stem) if (!std::isalnum((unsigned char)ch) && ch != '_' && ch != '-') ch = '_';
                if (stem.empty()) stem = "entity";
                const fs::path graph_dir = fs::path(st.asset_dir) / "visual_scripts";
                const fs::path graph_path = graph_dir / (stem + "_" + std::to_string(e.value("id", 0)) + ".json");
                std::error_code ec;
                fs::create_directories(graph_dir, ec);
                if (ec) {
                    st.log_error("Could not create graph folder: " + ec.message());
                } else if (fs::exists(graph_path, ec)) {
                    st.log_warn("Graph already exists: " + graph_path.string());
                } else {
                    if (_create_empty_visual_script_asset(graph_path, e.value("id", 0),
                                                          e.value("name", "Entity"))) {
                        const fs::path rel = fs::relative(graph_path, fs::path(st.asset_dir), ec);
                        comp["asset"] = (!ec && !rel.empty()) ? rel.generic_string() : graph_path.generic_string();
                        st.log_success("Created Visual Script graph: " + graph_path.string());
                    } else {
                        st.log_error("Could not write Visual Script graph: " + graph_path.string());
                    }
                }
            }
            ImGui::SameLine();
            const std::string current_asset = comp.value("asset", std::string());
            ImGui::BeginDisabled(current_asset.empty());
            if (ImGui::Button("Open Graph##vscript")) {
                st.requested_visual_script_asset = current_asset;
                st.requested_visual_script_entity_id = e.value("id", -1);
                st.request_visual_script_open = true;
            }
            ImGui::EndDisabled();
            if (st.playing) ImGui::TextColored(ImVec4(0.50f, 0.85f, 0.55f, 1.f), "Active in this Play session");

            ImGui::Unindent(8.f);
            ImGui::PopID();
            return false;
        }

        // ── Custom Light2D editor ─────────────────────────────────────────────
        // Replaces the generic key-value dump with purpose-built controls that
        // match Unity's Light2D inspector layout (enabled toggle, color picker,
        // radius + intensity sliders, cast_shadows toggle).
        if (ctype == "Light2D") {
            bool enabled = comp.value("enabled", true);
            if (ImGui::Checkbox("Enabled##l2d", &enabled)) comp["enabled"] = enabled;

            // Color — stored as [R,G,B,A] 0-255 array
            float col[4] = {1,1,1,1};
            if (comp.contains("color") && comp["color"].is_array() && comp["color"].size() >= 4) {
                col[0] = comp["color"][0].get<int>() / 255.f;
                col[1] = comp["color"][1].get<int>() / 255.f;
                col[2] = comp["color"][2].get<int>() / 255.f;
                col[3] = comp["color"][3].get<int>() / 255.f;
            }
            if (ImGui::ColorEdit4("Color##l2d", col)) {
                comp["color"] = nlohmann::json::array({(int)(col[0]*255),(int)(col[1]*255),(int)(col[2]*255),(int)(col[3]*255)});
            }

            float radius = comp.value("radius", 200.f);
            ImGui::SetNextItemWidth(200);
            if (ImGui::DragFloat("Radius##l2d", &radius, 2.f, 0.f, 4000.f, "%.0f px"))
                comp["radius"] = radius;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Light falloff radius in screen pixels at zoom=1.\nFragments at this distance receive zero contribution.");

            float intensity = comp.value("intensity", 1.f);
            ImGui::SetNextItemWidth(200);
            if (ImGui::SliderFloat("Intensity##l2d", &intensity, 0.f, 4.f))
                comp["intensity"] = intensity;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Linear intensity multiplier.\nFor Sprite-Lit sprites, accumulates with other lights.\n>1.0 allows overbright / HDR-style bloom contribution.");

            bool cast_sh = comp.value("cast_shadows", true);
            if (ImGui::Checkbox("Cast Shadows##l2d", &cast_sh)) {
                comp["cast_shadows"] = cast_sh;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Allows this light to project shadows from Shadow2DCaster components.");
            }

            ImGui::Spacing();


            ImGui::Unindent(8.f);
            ImGui::PopID();
            return false;
        }

        // ── Special Unity-style Transform layout ──────────────────────────────
            if (ctype == "Transform" && comp.is_object()) {
            // Position row: X and Y side by side
            {
                float x = comp.value("x", 0.f), y = comp.value("y", 0.f);
                ImGui::TextUnformatted("Position");
                ImGui::SameLine(70);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f,0.4f,0.4f,1.f));
                ImGui::TextUnformatted("X"); ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                if (ImGui::DragFloat("##px", &x, 1.f, 0,0,"%.1f")) { comp["x"]=x; transform_changed=true; }
                ImGui::SameLine(0,6);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f,1.f,0.4f,1.f));
                ImGui::TextUnformatted("Y"); ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                if (ImGui::DragFloat("##py", &y, 1.f, 0,0,"%.1f")) { comp["y"]=y; transform_changed=true; }
                ImGui::SameLine(0,4);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f,0.22f,0.22f,1.f));
                if (ImGui::SmallButton("R##rpos")) { comp["x"]=0.f; comp["y"]=0.f; transform_changed=true; }
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset position to (0, 0).");
            }
            // Rotation row
            {
                float r = comp.value("rotation", 0.f);
                ImGui::TextUnformatted("Rotation");
                ImGui::SameLine(70);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f,0.5f,1.f,1.f));
                ImGui::TextUnformatted("Z"); ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                if (ImGui::DragFloat("##rot", &r, 0.5f, 0,0,"%.1f")) { comp["rotation"]=r; transform_changed=true; }
                ImGui::SameLine(0,4);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f,0.22f,0.22f,1.f));
                if (ImGui::SmallButton("R##rrot")) { comp["rotation"]=0.f; transform_changed=true; }
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset rotation to 0°.");
            }
            // Scale row with uniform lock
            {
                float sx = comp.value("scale_x", 1.f), sy = comp.value("scale_y", 1.f);
                // _uniform_scale: when true, dragging either axis locks both together
                bool uni = e.value("_uniform_scale", true);
                ImGui::TextUnformatted("Scale");
                ImGui::SameLine(70);
                // Lock toggle button
                ImGui::PushStyleColor(ImGuiCol_Button, uni ? ImVec4(0.24f,0.49f,0.91f,1.f) : ImVec4(0.28f,0.28f,0.28f,1.f));
                if (ImGui::SmallButton(uni ? " = ##scl" : " / ##scl")) e["_uniform_scale"] = !uni;
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(uni ? "Uniform scale ON — X and Y move together." : "Uniform scale OFF — X and Y are independent.");
                ImGui::SameLine(0,4);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f,0.4f,0.4f,1.f));
                ImGui::TextUnformatted("X"); ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                if (ImGui::DragFloat("##scx", &sx, 0.01f, 0,0,"%.3f")) {
                    if (uni && sy != 0.f) sy = sy * (sx / std::max(0.0001f, comp.value("scale_x",1.f)));
                    comp["scale_x"]=sx; if (uni) comp["scale_y"]=sy; transform_changed=true;
                }
                ImGui::SameLine(0,6);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f,1.f,0.4f,1.f));
                ImGui::TextUnformatted("Y"); ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                if (ImGui::DragFloat("##scy", &sy, 0.01f, 0,0,"%.3f")) {
                    if (uni && sx != 0.f) sx = sx * (sy / std::max(0.0001f, comp.value("scale_y",1.f)));
                    comp["scale_y"]=sy; if (uni) comp["scale_x"]=sx; transform_changed=true;
                }
                ImGui::SameLine(0,4);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f,0.22f,0.22f,1.f));
                if (ImGui::SmallButton("R##rscl")) { comp["scale_x"]=1.f; comp["scale_y"]=1.f; transform_changed=true; }
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset scale to (1, 1).");
            }
            if (transform_changed) transform::mark_local_dirty(e.value("id",0));
            ImGui::Unindent(8.f);
            ImGui::PopID();
            return false;
        }

        // ── Animator: clip picker, playback controls, frame scrubber ────────────
        if (ctype == "Animator") {
            // Current animation dropdown (also sets default on enter Play)
            if (comp.contains("animations") && comp["animations"].is_object()) {
                std::string cur = comp.value("current_animation", std::string());
                if (ImGui::BeginCombo("Default Clip##anim_cur", cur.empty() ? "(none)" : cur.c_str())) {
                    if (ImGui::Selectable("(none)", cur.empty())) { comp["current_animation"] = ""; }
                    for (auto& [clipName, _] : comp["animations"].items()) {
                        bool sel = (clipName == cur);
                        if (ImGui::Selectable(clipName.c_str(), sel)) {
                            comp["current_animation"] = clipName;
                            comp["frame"] = 0.f;
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("The clip that plays on Start and is shown in the scene view.\nPick from the clips defined in the Animator panel.");
            }

            // Playing + Loop on same row
            bool playing  = comp.value("playing",  true);
            bool loop     = comp.value("loop",     true);
            bool pingpong = comp.value("ping_pong", false);
            if (ImGui::Checkbox("Playing##anim", &playing)) comp["playing"] = playing;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play/pause the animation. Useful for one-shot clips you start from script.");
            ImGui::SameLine(0,8);
            if (ImGui::Checkbox("Loop##anim", &loop)) { comp["loop"] = loop; if (loop) { comp["ping_pong"] = false; pingpong = false; } }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Restart the clip from frame 0 when it ends.");
            ImGui::SameLine(0,8);
            if (ImGui::Checkbox("Ping Pong##anim", &pingpong)) { comp["ping_pong"] = pingpong; if (pingpong) comp["loop"] = true; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play forward then backward repeatedly. Implies Loop.");

            // FPS + Speed on same row
            float fps   = comp.value("default_fps", 12.f);
            float speed = comp.value("speed", 1.f);
            float speed_mul = comp.value("speed_multiplier", 1.f);
            ImGui::SetNextItemWidth(100.f);
            if (ImGui::DragFloat("FPS##anim", &fps, 0.5f, 0.1f, 240.f, "%.1f"))
                comp["default_fps"] = fps;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Base frames per second for clips that don\'t set their own fps.");
            ImGui::SameLine(0,6);
            ImGui::SetNextItemWidth(80.f);
            if (ImGui::DragFloat("Speed##anim", &speed, 0.01f, 0.f, 10.f, "%.2fx"))
                comp["speed"] = speed;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Global speed multiplier. 1.0=normal, 2.0=double speed, 0.5=half speed.");
            if (st.playing && speed_mul != 1.f) {
                ImGui::SameLine(0,6);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f,0.8f,0.4f,1.f));
                ImGui::Text("x%.2f", speed_mul);
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Runtime speed_multiplier set by script (Animator.SetSpeedMultiplier).");
            }

            // Frame scrubber — only meaningful for sprite-sheet mode
            if (comp.value("use_sprite_sheet", false)) {
                std::string cur = comp.value("current_animation", std::string());
                int total_frames = 1;
                if (!cur.empty() && comp.contains("animations") && comp["animations"].contains(cur)) {
                    auto& clip = comp["animations"][cur];
                    Entity frames = Entity::array();
                    if (clip.is_object()) frames = clip.value("frames", clip.value("textures", Entity::array()));
                    else if (clip.is_array()) frames = clip;
                    if (frames.empty()) {
                        int cols = std::max(1, comp.value("sheet_columns",1));
                        int rows = std::max(1, comp.value("sheet_rows",1));
                        total_frames = cols * rows;
                    } else {
                        total_frames = (int)frames.size();
                    }
                }
                float frame_f = comp.value("frame", 0.f);
                int frame_i = (int)frame_f;
                ImGui::SetNextItemWidth(200.f);
                if (ImGui::SliderInt("Frame##anim_frame", &frame_i, 0, std::max(0, total_frames - 1)))
                    comp["frame"] = (float)frame_i;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Scrub through frames in the scene view without entering Play mode.");
            }
        }
        // ── Camera2D: entity picker for follow_target + contextual sliders ────
        if (ctype == "Camera2D") {
            // follow_target: entity dropdown instead of raw int
            int cur_target = comp.value("follow_target", -1);
            Entity* cur_ep = cur_target >= 0 ? st.find_entity(cur_target) : nullptr;
            std::string cur_label = cur_ep ? cur_ep->value("name", "Entity") : "None";
            ImGui::SetNextItemWidth(200.f);
            if (ImGui::BeginCombo("Follow Target##cam2d", cur_label.c_str())) {
                if (ImGui::Selectable("None", cur_target < 0)) comp["follow_target"] = -1;
                ImGui::Separator();
                for (auto& cand : st.entities) {
                    if (cand.value("_runtime_only", false)) continue;
                    int cid = cand.value("id", 0);
                    std::string cname = cand.value("name", "Entity") + "  [" + std::to_string(cid) + "]";
                    if (ImGui::Selectable(cname.c_str(), cid == cur_target))
                        comp["follow_target"] = cid;
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The entity this camera will follow at runtime.\nPick from the scene — no need to type an id.");

            // Clear button
            if (cur_target >= 0) {
                ImGui::SameLine(0,4);
                if (ImGui::SmallButton("Clear##cam2d")) comp["follow_target"] = -1;
            }

            // Zoom slider: 0.1x → 10x
            float zoom = comp.value("zoom", 1.f);
            ImGui::SetNextItemWidth(200.f);
            if (ImGui::SliderFloat("Zoom##cam2d", &zoom, 0.1f, 10.f, "%.2fx")) {
                comp["zoom"] = zoom;
                comp["projection_size_mode"] = "zoom";
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scene zoom. 1.0 = 1 world-unit per pixel at the native resolution.");

            // Follow smoothing
            float smooth = comp.value("smooth", 0.f);
            ImGui::SetNextItemWidth(200.f);
            if (ImGui::SliderFloat("Follow Smooth##cam2d", &smooth, 0.f, 30.f, "%.1f"))
                comp["smooth"] = smooth;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Camera follow damping (exp-decay rate).\n0 = instant snap. Higher = slower, smoother follow.");

            // Offset
            float ox = comp.value("offset_x", 0.f), oy = comp.value("offset_y", 0.f);
            ImGui::SetNextItemWidth(96.f);
            if (ImGui::DragFloat("Off X##cam2d", &ox, 1.f)) comp["offset_x"] = ox;
            ImGui::SameLine(0,4);
            ImGui::SetNextItemWidth(96.f);
            if (ImGui::DragFloat("Off Y##cam2d", &oy, 1.f)) comp["offset_y"] = oy;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("World-unit offset applied on top of the follow target position.");

            // Orthographic size: how many world-units tall the half-screen is
            float ortho = comp.value("orthographic_size", 5.f);
            ImGui::SetNextItemWidth(160.f);
            if (ImGui::DragFloat("Ortho Size##cam2d", &ortho, 0.1f, 0.1f, 1000.f, "%.2f u")) {
                comp["orthographic_size"] = ortho;
                comp["projection_size_mode"] = "orthographic";
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Relative half-height of the camera view. Smaller = zoomed in. Editing this makes orthographic size the active framing control; editing Zoom switches back.");

            // Camera rotation (angle in degrees)
            float angle = comp.value("angle", 0.f);
            ImGui::SetNextItemWidth(160.f);
            if (ImGui::DragFloat("Angle##cam2d", &angle, 0.5f, -360.f, 360.f, "%.1fÂ°"))
                comp["angle"] = angle;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rotate the camera view. Useful for tilt effects and top-down rotation.");
            if (angle != 0.f) {
                ImGui::SameLine(0,4);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f,0.22f,0.22f,1.f));
                if (ImGui::SmallButton("0Â°##cam2d")) comp["angle"] = 0.f;
                ImGui::PopStyleColor();
            }
        }


        // ── Rigidbody2D: body type, physics properties, freeze axes, velocity ─
        if (ctype == "Rigidbody2D") {
            // Body Type dropdown first — changes meaning of all physics fields
            std::string bt = comp.value("body_type", "dynamic");
            const char* bt_label = bt=="kinematic" ? "Kinematic" : bt=="static" ? "Static" : "Dynamic";
            ImGui::SetNextItemWidth(160.f);
            if (ImGui::BeginCombo("Body Type##rb", bt_label)) {
                if (ImGui::Selectable("Dynamic",   bt=="dynamic"))   comp["body_type"] = "dynamic";
                if (ImGui::Selectable("Kinematic", bt=="kinematic")) comp["body_type"] = "kinematic";
                if (ImGui::Selectable("Static",    bt=="static"))    comp["body_type"] = "static";
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Dynamic: fully simulated.\nKinematic: moved by script, pushes others.\nStatic: never moves, cheapest for world geometry.");

            bool is_dynamic = (bt == "dynamic");
            if (is_dynamic) {
                float mass = comp.value("mass", 1.f);
                ImGui::SetNextItemWidth(160.f);
                if (ImGui::DragFloat("Mass##rb", &mass, 0.05f, 0.001f, 10000.f, "%.3f kg"))
                    comp["mass"] = mass;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Resistance to linear acceleration.\nHeavier bodies need more force and hit harder.");

                float grav = comp.value("gravity_scale", 1.f);
                ImGui::SetNextItemWidth(160.f);
                if (ImGui::DragFloat("Gravity Scale##rb", &grav, 0.05f, -10.f, 20.f, "%.2f"))
                    comp["gravity_scale"] = grav;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Multiplier on global gravity (980 units/s²).\n0=weightless, negative=floats up, >1=falls faster.");

                float drag = comp.value("drag", 0.05f);
                ImGui::SetNextItemWidth(160.f);
                if (ImGui::DragFloat("Linear Drag##rb", &drag, 0.005f, 0.f, 100.f, "%.3f"))
                    comp["drag"] = drag;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Air resistance on movement. 0=slides forever.\nHigher values slow the body when no force is applied.");

                float adrag = comp.value("angular_drag", 0.05f);
                ImGui::SetNextItemWidth(160.f);
                if (ImGui::DragFloat("Angular Drag##rb", &adrag, 0.005f, 0.f, 100.f, "%.3f"))
                    comp["angular_drag"] = adrag;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rotational damping. 0=spins forever.\nHigh values stop spinning very quickly.");
            }

            ImGui::Spacing();
            // Per-axis freeze (Unity's Constraints section)
            auto& constr = comp["constraints"];
            if (!constr.is_object()) constr = Entity::object();
            bool freeze_x   = constr.value("freeze_pos_x",  false);
            bool freeze_y   = constr.value("freeze_pos_y",  false);
            bool freeze_rot = comp.value("freeze_rotation",  false);
            ImGui::TextUnformatted("Freeze:");
            ImGui::SameLine(0,6);
            if (ImGui::Checkbox("Pos X##rb", &freeze_x))  constr["freeze_pos_x"]  = freeze_x;
            ImGui::SameLine(0,4);
            if (ImGui::Checkbox("Pos Y##rb", &freeze_y))  constr["freeze_pos_y"]  = freeze_y;
            ImGui::SameLine(0,4);
            if (ImGui::Checkbox("Rot##rb",   &freeze_rot)) comp["freeze_rotation"] = freeze_rot;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Freeze individual axes so physics cannot move or rotate this body.");

            // Live velocity readout (only meaningful while playing)
            if (st.playing) {
                float vx = comp.value("velocity_x", 0.f);
                float vy = comp.value("velocity_y", 0.f);
                float av = comp.value("angular_velocity", 0.f);
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f,0.85f,0.55f,1.f));
                ImGui::Text("Vel  X: %.2f  Y: %.2f  Ang: %.2fÂ°/s", vx, vy, av);
                ImGui::PopStyleColor();
                if (ImGui::SmallButton("Zero Velocity##rb")) {
                    comp["velocity_x"] = 0.f; comp["velocity_y"] = 0.f; comp["angular_velocity"] = 0.f;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Instantly zero out all linear and angular velocity.");
                ImGui::Spacing();
            }

            // Layer mask: 16-bit visual checkbox grid instead of raw int
            int mask = comp.value("layer_mask", 65535);
            ImGui::TextUnformatted("Collision Layers:");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Bit-mask of physics layers this body collides with.\nUncheck a layer to ignore collisions with bodies on that layer.");
            for (int i = 0; i < 16; ++i) {
                if (i % 8 != 0) ImGui::SameLine(0,2);
                bool on = (mask >> i) & 1;
                char lbl[8]; snprintf(lbl, sizeof(lbl), "##lm%d", i);
                if (ImGui::Checkbox(lbl, &on)) {
                    if (on) mask |= (1 << i); else mask &= ~(1 << i);
                    comp["layer_mask"] = mask;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Layer %d", i);
            }
        }
        // ── BoxCollider2D / CircleCollider2D / CapsuleCollider2D: layer mask ──
        if (ctype == "BoxCollider2D" || ctype == "CircleCollider2D" || ctype == "CapsuleCollider2D") {
            // is_trigger highlight
            bool is_trig = comp.value("is_trigger", false);
            if (is_trig) ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.6f,0.25f,0.1f,0.6f));
            if (ImGui::Checkbox("Is Trigger##col", &is_trig)) comp["is_trigger"] = is_trig;
            if (is_trig) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Trigger mode: overlaps fire OnTriggerEnter/Exit but no physics impulse is applied.\nHighlighted in orange as a reminder.");

            // Material shorthand: bounciness + friction sliders side by side
            float bounce = comp.value("bounciness", 0.f);
            float frict  = comp.value("friction",   0.3f);
            ImGui::SetNextItemWidth(120.f);
            if (ImGui::SliderFloat("Bounce##col", &bounce, 0.f, 1.f, "%.2f")) comp["bounciness"] = bounce;
            ImGui::SameLine(0,8);
            ImGui::SetNextItemWidth(120.f);
            if (ImGui::SliderFloat("Friction##col", &frict, 0.f, 1.f, "%.2f")) comp["friction"] = frict;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Physics material shorthand. 0 bounce = perfectly inelastic. 1 bounce = perfect rebound.");
            // offset/radius/size fields shown via generic loop below (no custom controls needed)
        }

        // ── AudioSource: asset drag-drop, in-editor preview play/stop ─────────
        if (ctype == "AudioSource") {
            // Clip field with drag-drop from Assets panel
            std::string clip = comp.value("clip", std::string());
            if (_draw_asset_slot(st, "Clip", clip, {".wav", ".ogg", ".mp3", ".flac"},
                                 "Audio Files (*.wav;*.ogg;*.mp3;*.flac)\0*.wav;*.ogg;*.mp3;*.flac\0\0", "Select Audio Clip"))
                comp["clip"] = clip;

            // In-editor preview: only works while Playing (AudioSystem is live)
            ImGui::SameLine(0,6);
            bool is_playing = comp.value("_is_playing", false);
            if (is_playing) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f,0.2f,0.2f,1.f));
                if (ImGui::SmallButton("Stop##au_prev")) comp["_stop_now"] = true;
                ImGui::PopStyleColor();
            } else {
                if (ImGui::SmallButton("Play##au_prev")) comp["_play_now"] = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(st.playing ? "Preview this clip (engine must be Playing)."
                                             : "Press Play first to preview audio.");

            // Volume + Pitch sliders (more useful than raw number boxes)
            float vol   = comp.value("volume",  1.f);
            float pitch = comp.value("pitch",   1.f);
            ImGui::SetNextItemWidth(180.f);
            if (ImGui::SliderFloat("Volume##au",  &vol,   0.f, 1.f, "%.2f")) comp["volume"]  = vol;
            ImGui::SetNextItemWidth(180.f);
            if (ImGui::SliderFloat("Pitch##au",   &pitch, 0.1f, 3.f, "%.2f")) comp["pitch"]  = pitch;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Playback speed multiplier. 1.0 = normal. 2.0 = one octave up.");

            // Spatial: show distance fields only when spatial is on
            bool spatial = comp.value("spatial", false);
            if (ImGui::Checkbox("Spatial##au", &spatial)) comp["spatial"] = spatial;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Attenuate volume by distance from the Camera2D.\nExposes min/max distance fields below.");
            if (spatial) {
                float mind = comp.value("min_distance",  100.f);
                float maxd = comp.value("max_distance", 1000.f);
                ImGui::SetNextItemWidth(120.f);
                if (ImGui::DragFloat("Min Dist##au", &mind, 1.f, 0.f, maxd - 1.f)) comp["min_distance"] = mind;
                ImGui::SameLine(0,6);
                ImGui::SetNextItemWidth(120.f);
                if (ImGui::DragFloat("Max Dist##au", &maxd, 1.f, mind + 1.f, 10000.f)) comp["max_distance"] = maxd;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Full volume within Min Dist. Silent beyond Max Dist. Linear falloff between.");
            }
            // Loop + Play on Awake — drawn here so nothing falls through to generic loop
            bool loop_au = comp.value("loop", false);
            bool poa_au  = comp.value("play_on_awake", false);
            if (ImGui::Checkbox("Loop##au", &loop_au)) comp["loop"] = loop_au;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Repeat the clip endlessly. Stop with AudioSource.Stop() from script.");
            ImGui::SameLine(0,10);
            if (ImGui::Checkbox("Play On Awake##au", &poa_au)) comp["play_on_awake"] = poa_au;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Start playing automatically when the scene starts, without any script call.");
        }

        // ── Cinemachine2D: entity pickers for follow/look_at + confinement ───
        if (ctype == "Cinemachine2D") {
            // Helper lambda: entity dropdown that writes an int id
            auto entity_picker = [&](const char* label, const char* id_key) {
                int cur = comp.value(id_key, -1);
                Entity* ep = cur >= 0 ? st.find_entity(cur) : nullptr;
                std::string lbl = ep ? ep->value("name","Entity") + "  ["+std::to_string(cur)+"]" : "None";
                ImGui::SetNextItemWidth(200.f);
                if (ImGui::BeginCombo(label, lbl.c_str())) {
                    if (ImGui::Selectable("None", cur < 0)) comp[id_key] = -1;
                    ImGui::Separator();
                    for (auto& cand : st.entities) {
                        if (cand.value("_runtime_only",false)) continue;
                        int cid = cand.value("id",-1);
                        std::string cn = cand.value("name","Entity")+"  ["+std::to_string(cid)+"]";
                        if (ImGui::Selectable(cn.c_str(), cid==cur)) comp[id_key] = cid;
                    }
                    ImGui::EndCombo();
                }
                if (cur >= 0) { ImGui::SameLine(0,4); char cb[32]; snprintf(cb,sizeof(cb),"Clear##%s",id_key); if(ImGui::SmallButton(cb)) comp[id_key]=-1; }
            };
            entity_picker("Follow Target##cin", "follow_target");
            entity_picker("Look At##cin",       "look_at_target");

            // Priority
            int pri = comp.value("priority", 10);
            ImGui::SetNextItemWidth(80.f);
            if (ImGui::InputInt("Priority##cin", &pri)) comp["priority"] = pri;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Higher priority Cinemachine2D takes control of the active Camera2D.");

            // Dead / soft zone as labelled sliders (0..1 fraction of screen)
            float dzw = comp.value("dead_zone_w",0.2f), dzh = comp.value("dead_zone_h",0.15f);
            float szw = comp.value("soft_zone_w",0.6f), szh = comp.value("soft_zone_h",0.6f);
            ImGui::TextUnformatted("Dead Zone:");
            ImGui::SetNextItemWidth(120.f);
            if (ImGui::SliderFloat("W##dzw",  &dzw, 0.f, 1.f, "%.2f")) comp["dead_zone_w"] = dzw;
            ImGui::SameLine(0,4);
            ImGui::SetNextItemWidth(120.f);
            if (ImGui::SliderFloat("H##dzh",  &dzh, 0.f, 1.f, "%.2f")) comp["dead_zone_h"] = dzh;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Camera doesn't move while the target stays inside this screen-fraction box.");
            ImGui::TextUnformatted("Soft Zone:");
            ImGui::SetNextItemWidth(120.f);
            if (ImGui::SliderFloat("W##szw",  &szw, 0.f, 1.f, "%.2f")) comp["soft_zone_w"] = szw;
            ImGui::SameLine(0,4);
            ImGui::SetNextItemWidth(120.f);
            if (ImGui::SliderFloat("H##szh",  &szh, 0.f, 1.f, "%.2f")) comp["soft_zone_h"] = szh;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Camera hard-clamps to keep target inside soft zone. Damped between dead and soft zone edges.");

            // World-bounds confinement
            bool confine = comp.value("confine", false);
            if (ImGui::Checkbox("Confine To Bounds##cin", &confine)) comp["confine"] = confine;
            if (confine) {
                float mnx=comp.value("confine_min_x",-1000.f), mxx=comp.value("confine_max_x",1000.f);
                float mny=comp.value("confine_min_y",-1000.f), mxy=comp.value("confine_max_y",1000.f);
                ImGui::SetNextItemWidth(90.f); if(ImGui::DragFloat("Min X##cin",&mnx,1.f)) comp["confine_min_x"]=mnx; ImGui::SameLine(0,4);
                ImGui::SetNextItemWidth(90.f); if(ImGui::DragFloat("Max X##cin",&mxx,1.f)) comp["confine_max_x"]=mxx;
                ImGui::SetNextItemWidth(90.f); if(ImGui::DragFloat("Min Y##cin",&mny,1.f)) comp["confine_min_y"]=mny; ImGui::SameLine(0,4);
                ImGui::SetNextItemWidth(90.f); if(ImGui::DragFloat("Max Y##cin",&mxy,1.f)) comp["confine_max_y"]=mxy;
            }
            // Damping + lookahead
            float dx2 = comp.value("damping_x", 0.3f), dy2 = comp.value("damping_y", 0.3f);
            ImGui::SetNextItemWidth(90.f);
            if (ImGui::DragFloat("Damp X##cin", &dx2, 0.01f, 0.f, 20.f, "%.2f")) comp["damping_x"] = dx2;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Horizontal follow damping. 0=instant snap, higher=laggy smooth follow.");
            ImGui::SameLine(0,4);
            ImGui::SetNextItemWidth(90.f);
            if (ImGui::DragFloat("Damp Y##cin", &dy2, 0.01f, 0.f, 20.f, "%.2f")) comp["damping_y"] = dy2;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Vertical follow damping.");

            float lah = comp.value("lookahead_time", 0.f);
            ImGui::SetNextItemWidth(120.f);
            if (ImGui::DragFloat("Lookahead##cin", &lah, 0.02f, 0.f, 3.f, "%.2f s")) comp["lookahead_time"] = lah;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("How far ahead (seconds) the camera leads the target's velocity.\n0 = no lookahead. ~0.2-0.5 feels responsive for platformers.");

            float zoom_cin = comp.value("zoom", 1.f);
            ImGui::SetNextItemWidth(120.f);
            if (ImGui::SliderFloat("Zoom##cin", &zoom_cin, 0.1f, 10.f, "%.2fx")) comp["zoom"] = zoom_cin;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Camera zoom override. 1.0 = default size. Multiplied with orthographic_size.");

            float sx = comp.value("screen_x", 0.5f), sy = comp.value("screen_y", 0.5f);
            ImGui::SetNextItemWidth(90.f);
            if (ImGui::DragFloat("Screen X##cin", &sx, 0.01f, 0.f, 1.f, "%.2f")) comp["screen_x"] = sx;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Horizontal screen composition (0=left edge, 0.5=center, 1=right edge).");
            ImGui::SameLine(0,4);
            ImGui::SetNextItemWidth(90.f);
            if (ImGui::DragFloat("Screen Y##cin", &sy, 0.01f, 0.f, 1.f, "%.2f")) comp["screen_y"] = sy;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Vertical screen composition (0=top, 0.5=center, 1=bottom).");
        }

        // ── Joints: entity picker for connected_entity ────────────────────────
        if (ctype == "DistanceJoint2D" || ctype == "SpringJoint2D" ||
            ctype == "HingeJoint2D"    || ctype == "MouseJoint2D"  ||
            ctype == "FixedJoint2D") {
            int cur = comp.value("connected_entity", -1);
            Entity* ep = cur >= 0 ? st.find_entity(cur) : nullptr;
            std::string lbl = ep ? ep->value("name","Entity")+"  ["+std::to_string(cur)+"]" : "None";
            ImGui::SetNextItemWidth(200.f);
            if (ImGui::BeginCombo("Connected Body##jt", lbl.c_str())) {
                if (ImGui::Selectable("None", cur < 0)) comp["connected_entity"] = -1;
                ImGui::Separator();
                int self_id = e.value("id", -1);
                for (auto& cand : st.entities) {
                    if (cand.value("_runtime_only",false)) continue;
                    int cid = cand.value("id",-1);
                    if (cid == self_id) continue;
                    if (!has_component(cand,"Rigidbody2D")) continue; // only show physics bodies
                    std::string cn = cand.value("name","Entity")+"  ["+std::to_string(cid)+"]";
                    if (ImGui::Selectable(cn.c_str(), cid==cur)) comp["connected_entity"] = cid;
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The Rigidbody2D this joint connects to.\nOnly entities with a Rigidbody2D are listed.");
            if (cur >= 0) {
                ImGui::SameLine(0,4);
                if (ImGui::SmallButton("Clear##jt")) comp["connected_entity"] = -1;
            }
            // Per-joint-type controls
            if (ctype == "SpringJoint2D") {
                float rest  = comp.value("rest_length", 80.f);
                float stiff = comp.value("stiffness",   10.f);
                float damp  = comp.value("damping",     1.f);
                ImGui::SetNextItemWidth(140.f);
                if (ImGui::DragFloat("Rest Length##jt", &rest, 1.f, 0.f, 10000.f, "%.1f"))  comp["rest_length"] = rest;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Natural length of the spring in world-units. Bodies pull/push to reach this distance.");
                ImGui::SetNextItemWidth(140.f);
                if (ImGui::DragFloat("Stiffness##jt", &stiff, 0.5f, 0.f, 10000.f, "%.1f")) comp["stiffness"]   = stiff;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Spring constant (Hooke\'s law). Higher = snappier spring, lower = loose rubber band.");
                ImGui::SetNextItemWidth(140.f);
                if (ImGui::DragFloat("Damping##jt",   &damp,  0.05f, 0.f, 100.f,   "%.2f")) comp["damping"]     = damp;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Energy loss per oscillation. 0=bounces forever, high=stops quickly.");
            }
            if (ctype == "DistanceJoint2D") {
                float maxl = comp.value("max_length", 100.f);
                float minl = comp.value("min_length",   0.f);
                ImGui::SetNextItemWidth(110.f);
                if (ImGui::DragFloat("Max Dist##jt", &maxl, 1.f, 0.f, 100000.f, "%.1f")) comp["max_length"] = maxl;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Maximum allowed distance between anchor points (rope/rod upper bound).");
                ImGui::SameLine(0,4);
                ImGui::SetNextItemWidth(110.f);
                if (ImGui::DragFloat("Min Dist##jt", &minl, 1.f, 0.f, maxl,      "%.1f")) comp["min_length"] = minl;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Minimum allowed distance. 0=rope only. >0=rigid rod that also pushes.");
            }
            if (ctype == "HingeJoint2D") {
                bool use_motor  = comp.value("use_motor", false);
                bool use_limits = comp.value("use_limits", false);
                if (ImGui::Checkbox("Motor##jt_h", &use_motor))   comp["use_motor"]  = use_motor;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drive the hinge to rotate at a target speed.");
                ImGui::SameLine(0,8);
                if (ImGui::Checkbox("Limits##jt_h", &use_limits)) comp["use_limits"] = use_limits;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clamp rotation between lower and upper angle.");
                if (use_motor) {
                    float mspd = comp.value("motor_speed", 0.f), mtq = comp.value("max_torque", 10000.f);
                    ImGui::SetNextItemWidth(110.f);
                    if (ImGui::DragFloat("Motor Spd##jt_h", &mspd, 1.f, -3600.f, 3600.f, "%.0fÂ°/s")) comp["motor_speed"] = mspd;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Target angular speed in degrees/second. Negative = reverse.");
                    ImGui::SameLine(0,4);
                    ImGui::SetNextItemWidth(110.f);
                    if (ImGui::DragFloat("Max Torque##jt_h", &mtq, 10.f, 0.f, 1e6f, "%.0f")) comp["max_torque"] = mtq;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Maximum force the motor can apply to reach its target speed.");
                }
                if (use_limits) {
                    float lo = comp.value("lower_angle", -90.f), hi = comp.value("upper_angle", 90.f);
                    ImGui::SetNextItemWidth(100.f);
                    if (ImGui::DragFloat("LowerÂ°##jt_h", &lo, 0.5f, -360.f, hi,    "%.1f")) comp["lower_angle"] = lo;
                    ImGui::SameLine(0,4);
                    ImGui::SetNextItemWidth(100.f);
                    if (ImGui::DragFloat("UpperÂ°##jt_h", &hi, 0.5f, lo,    360.f, "%.1f")) comp["upper_angle"] = hi;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rotation is clamped to [Lower, Upper] degrees relative to anchor.");
                }
            }
        }

        // ── LineRenderer2D: point list editor ─────────────────────────────────
        if (ctype == "LineRenderer2D") {
            auto& pts = comp["points"];
            if (!pts.is_array()) pts = nlohmann::json::array();
            int n = (int)pts.size() / 2; // flat [x0,y0, x1,y1, ...]
            ImGui::Text("Points (%d)", n);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("World-space XY pairs. Add/remove via the buttons below.");

            int to_del = -1;
            for (int i = 0; i < n; ++i) {
                float px = pts[i*2].get<float>(), py = pts[i*2+1].get<float>();
                ImGui::PushID(i);
                ImGui::SetNextItemWidth(80.f);
                if (ImGui::DragFloat("##lrx", &px, 1.f)) { pts[i*2] = px; }
                ImGui::SameLine(0,2);
                ImGui::SetNextItemWidth(80.f);
                if (ImGui::DragFloat("##lry", &py, 1.f)) { pts[i*2+1] = py; }
                ImGui::SameLine(0,4);
                if (ImGui::SmallButton("-##lrdel")) to_del = i;
                ImGui::PopID();
            }
            if (to_del >= 0) {
                pts.erase_at((size_t)(to_del*2+1));
                pts.erase_at((size_t)(to_del*2));
            }
            if (ImGui::Button("+ Add Point##lr")) {
                float lx = n > 0 ? pts[(n-1)*2].get<float>() + 32.f : 0.f;
                float ly = n > 0 ? pts[(n-1)*2+1].get<float>()       : 0.f;
                pts.push_back(lx); pts.push_back(ly);
            }
            // Width taper + colors + loop
            float ws = comp.value("width_start", 4.f), we = comp.value("width_end", 4.f);
            ImGui::SetNextItemWidth(100.f);
            if (ImGui::DragFloat("Width Start##lr", &ws, 0.1f, 0.f, 500.f, "%.1f")) comp["width_start"] = ws;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Line width in world-units at the first point.");
            ImGui::SameLine(0,4);
            ImGui::SetNextItemWidth(100.f);
            if (ImGui::DragFloat("End##lr_we", &we, 0.1f, 0.f, 500.f, "%.1f")) comp["width_end"] = we;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Line width at the last point. Tapers linearly from start to end.");

            auto lr_col = [&](const char* label, const char* key) {
                float c[4]={1,1,1,1};
                auto cv = comp.value(key, std::vector<float>{255,255,255,255});
                for (int i=0;i<4&&i<(int)cv.size();++i) c[i]=cv[i]/255.f;
                if (ImGui::ColorEdit4(label,c,ImGuiColorEditFlags_AlphaBar))
                    comp[key]=nlohmann::json::array({(int)(c[0]*255),(int)(c[1]*255),(int)(c[2]*255),(int)(c[3]*255)});
            };
            lr_col("Color Start##lr", "color_start");
            lr_col("Color End##lr",   "color_end");

            bool lr_loop = comp.value("loop", false);
            bool lr_ws   = comp.value("use_world_space", true);
            if (ImGui::Checkbox("Loop##lr", &lr_loop))        comp["loop"]            = lr_loop;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Connect the last point back to the first to close the shape.");
            ImGui::SameLine(0,8);
            if (ImGui::Checkbox("World Space##lr", &lr_ws))  comp["use_world_space"]  = lr_ws;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("World Space: points are world-unit coordinates.\nLocal Space: points are relative to the entity\'s Transform.");
        }

        // ── NavMeshAgent2D: destination entity picker ─────────────────────────
        if (ctype == "NavMeshAgent2D") {
            // Entity-based destination: pick an entity, destination is snapped
            // to that entity's world position every frame at runtime.
            int dest_eid = comp.value("_dest_entity", -1);
            Entity* dep  = dest_eid >= 0 ? st.find_entity(dest_eid) : nullptr;
            std::string dlbl = dep ? dep->value("name","Entity")+"  ["+std::to_string(dest_eid)+"]" : "None (use X/Y below)";
            ImGui::SetNextItemWidth(200.f);
            if (ImGui::BeginCombo("Dest Entity##nav", dlbl.c_str())) {
                if (ImGui::Selectable("None##nav", dest_eid < 0)) {
                    comp["_dest_entity"] = -1;
                    comp["_path_requested"] = true;
                }
                ImGui::Separator();
                for (auto& cand : st.entities) {
                    if (cand.value("_runtime_only",false)) continue;
                    int cid = cand.value("id",-1);
                    std::string cn = cand.value("name","Entity")+"  ["+std::to_string(cid)+"]";
                    if (ImGui::Selectable(cn.c_str(), cid==dest_eid)) {
                        comp["_dest_entity"] = cid;
                        comp["_path_requested"] = true;
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Optional: pick an entity as the nav destination.\nIts world position is used instead of the manual X/Y below.\nLeave as None to set destination_x/y manually or from a script.");
            // Manual X/Y still visible so scripts can write them and the user
            // can inspect the current value
            float dx = comp.value("destination_x", 0.f), dy = comp.value("destination_y", 0.f);
            ImGui::SetNextItemWidth(90.f);
            if (ImGui::DragFloat("Dest X##nav", &dx, 1.f)) {
                comp["destination_x"] = dx;
                comp["_path_requested"] = true;
            }
            ImGui::SameLine(0,4);
            ImGui::SetNextItemWidth(90.f);
            if (ImGui::DragFloat("Dest Y##nav", &dy, 1.f)) {
                comp["destination_y"] = dy;
                comp["_path_requested"] = true;
            }
            // Movement parameters
            float nav_spd  = comp.value("speed",           150.f);
            float nav_stop = comp.value("stopping_distance", 8.f);
            float nav_acc  = comp.value("acceleration",    500.f);
            float nav_avr  = comp.value("avoidance_radius", 20.f);
            bool  nav_ar   = comp.value("auto_repath",      true);

            ImGui::SetNextItemWidth(140.f);
            if (ImGui::DragFloat("Speed##nav",     &nav_spd,  1.f, 0.f, 10000.f, "%.0f u/s")) comp["speed"]            = nav_spd;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Maximum movement speed in world-units per second.");

            ImGui::SetNextItemWidth(140.f);
            if (ImGui::DragFloat("Stop Dist##nav", &nav_stop, 0.5f, 0.f, 1000.f,  "%.1f u"))  comp["stopping_distance"] = nav_stop;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Agent stops this many world-units short of the destination.\nIncrease if the agent jitters on arrival.");

            ImGui::SetNextItemWidth(140.f);
            if (ImGui::DragFloat("Accel##nav",     &nav_acc,  5.f, 0.f, 100000.f, "%.0f"))    comp["acceleration"]      = nav_acc;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("How quickly the agent ramps up to full speed.");

            ImGui::SetNextItemWidth(140.f);
            if (ImGui::DragFloat("Avoid Radius##nav", &nav_avr, 0.5f, 0.f, 500.f, "%.1f u")) comp["avoidance_radius"]  = nav_avr;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Personal space bubble for local RVO avoidance between agents.\n0 = avoidance disabled.");

            if (ImGui::Checkbox("Auto Re-path##nav", &nav_ar)) comp["auto_repath"] = nav_ar;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Automatically request a new path when the destination moves far enough.\nDisable to control pathing entirely from script.");
        }

        // ── ParticleEmitter: emission controls + presets ──────────────────────
        if (ctype == "ParticleEmitter") {
            ImGui::Spacing();

            // Emitting toggle + looping on same row
            bool emitting = comp.value("emitting", true);
            bool looping  = comp.value("looping", true);
            if (ImGui::Checkbox("Emitting##pe", &emitting)) comp["emitting"] = emitting;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle emission on/off at runtime without removing the component.");
            ImGui::SameLine(0,10);
            if (ImGui::Checkbox("Looping##pe", &looping)) comp["looping"] = looping;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Repeat after one burst cycle. Disable for one-shot effects.");

            ImGui::Spacing();

            // Burst mode toggle
            bool burst = comp.value("burst", false);
            if (ImGui::Checkbox("Burst##pe", &burst)) comp["burst"] = burst;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Emit all particles in one instant burst instead of a continuous stream.");

            if (burst) {
                int bc = (int)comp.value("burst_count", 20.0);
                ImGui::SetNextItemWidth(140.f);
                if (ImGui::DragInt("Count##pe_bc", &bc, 1, 1, 2000)) comp["burst_count"] = (double)bc;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Number of particles spawned in the burst.");
            } else {
                float rate = comp.value("rate", 10.f);
                ImGui::SetNextItemWidth(140.f);
                if (ImGui::DragFloat("Rate (per sec)##pe", &rate, 0.5f, 0.1f, 2000.f, "%.1f"))
                    comp["rate"] = rate;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("How many particles are spawned per second.");
            }

            // Max particles cap
            int maxp = (int)comp.value("max_particles", 1000.0);
            ImGui::SetNextItemWidth(140.f);
            if (ImGui::DragInt("Max Particles##pe", &maxp, 5, 1, 100000)) comp["max_particles"] = (double)maxp;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hard cap on live particle count. Raise for dense effects, lower for performance.");

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Lifetime
            float lifetime = comp.value("lifetime", 2.f);
            float life_var = comp.value("lifetime_variation", 0.f);
            ImGui::SetNextItemWidth(110.f);
            if (ImGui::DragFloat("Lifetime##pe", &lifetime, 0.05f, 0.01f, 60.f, "%.2f s"))
                comp["lifetime"] = lifetime;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("How long each particle lives in seconds.");
            ImGui::SameLine(0,4);
            ImGui::SetNextItemWidth(80.f);
            if (ImGui::DragFloat("±Var##pe_lv", &life_var, 0.01f, 0.f, 1.f, "%.2f"))
                comp["lifetime_variation"] = life_var;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Random lifetime variance as a fraction (0.2 = ±20%% of lifetime).");

            // Speed
            float speed = comp.value("speed", 80.f);
            float speed_var = comp.value("speed_variation", 0.3f);
            ImGui::SetNextItemWidth(110.f);
            if (ImGui::DragFloat("Speed##pe", &speed, 1.f, 0.f, 5000.f, "%.0f"))
                comp["speed"] = speed;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Initial speed of each particle in world-units/second.");
            ImGui::SameLine(0,4);
            ImGui::SetNextItemWidth(80.f);
            if (ImGui::DragFloat("±Var##pe_sv", &speed_var, 0.01f, 0.f, 1.f, "%.2f"))
                comp["speed_variation"] = speed_var;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Speed variance as a fraction (0.3 = each particle gets ±30%% random speed).");

            // Direction + spread on same row
            float dir_angle = comp.value("direction_angle", -90.f);
            float spread    = comp.value("spread", 360.f);
            ImGui::SetNextItemWidth(110.f);
            if (ImGui::DragFloat("Direction°##pe", &dir_angle, 1.f, -360.f, 360.f, "%.0f°"))
                comp["direction_angle"] = dir_angle;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Emission direction in degrees. -90 = upward, 0 = right, 90 = down.");
            ImGui::SameLine(0,4);
            ImGui::SetNextItemWidth(80.f);
            if (ImGui::DragFloat("Spread°##pe", &spread, 1.f, 0.f, 360.f, "%.0f°"))
                comp["spread"] = spread;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cone angle around the direction. 360 = omnidirectional, 0 = laser-straight.");

            // Gravity
            float grav = comp.value("gravity_scale", 0.3f);
            ImGui::SetNextItemWidth(140.f);
            if (ImGui::DragFloat("Gravity Scale##pe", &grav, 0.02f, -5.f, 10.f, "%.2f"))
                comp["gravity_scale"] = grav;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Particle gravity multiplier. 0=weightless, negative=rises, 1=full gravity.");

            // Rotation per particle
            float rot_start = comp.value("rotation_start", 0.f);
            float rot_var   = comp.value("rotation_variation", 0.f);
            float ang_vel   = comp.value("angular_velocity", 0.f);
            float ang_var   = comp.value("angular_velocity_variation", 0.f);
            ImGui::SetNextItemWidth(100.f);
            if (ImGui::DragFloat("Start Rot°##pe", &rot_start, 1.f, -360.f, 360.f, "%.0f°"))
                comp["rotation_start"] = rot_start;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Initial rotation of each particle sprite in degrees.");
            ImGui::SameLine(0,4);
            ImGui::SetNextItemWidth(80.f);
            if (ImGui::DragFloat("±Var##pe_rv", &rot_var, 1.f, 0.f, 360.f, "%.0f°"))
                comp["rotation_variation"] = rot_var;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Random offset on spawn rotation.");
            ImGui::SetNextItemWidth(100.f);
            if (ImGui::DragFloat("Spin°/s##pe", &ang_vel, 1.f, -3600.f, 3600.f, "%.0f"))
                comp["angular_velocity"] = ang_vel;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("How fast each particle spins (degrees per second). Positive = clockwise.");
            ImGui::SameLine(0,4);
            ImGui::SetNextItemWidth(80.f);
            if (ImGui::DragFloat("±Var##pe_av", &ang_var, 1.f, 0.f, 3600.f, "%.0f"))
                comp["angular_velocity_variation"] = ang_var;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Random variance on spin speed.");

            // Size
            float sz_start = comp.value("size_start", 4.f);
            float sz_end   = comp.value("size_end", 0.f);
            ImGui::SetNextItemWidth(100.f);
            if (ImGui::DragFloat("Size Start##pe", &sz_start, 0.1f, 0.f, 500.f, "%.1f"))
                comp["size_start"] = sz_start;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Particle radius at spawn (world units). Overridden by Size Over Lifetime curve.");
            ImGui::SameLine(0,4);
            ImGui::SetNextItemWidth(100.f);
            if (ImGui::DragFloat("→End##pe", &sz_end, 0.1f, 0.f, 500.f, "%.1f"))
                comp["size_end"] = sz_end;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Particle radius at death. Linearly interpolated from Start.");

            // Colors
            auto draw_pe_color = [&](const char* label, const char* key) {
                float c[4]={1,1,1,1};
                auto cv = comp.value(key, std::vector<float>{255,255,255,255});
                for (int i=0;i<4&&i<(int)cv.size();++i) c[i]=cv[i]/255.f;
                if (ImGui::ColorEdit4(label,c,ImGuiColorEditFlags_AlphaBar))
                    comp[key]=nlohmann::json::array({(int)(c[0]*255),(int)(c[1]*255),(int)(c[2]*255),(int)(c[3]*255)});
            };
            draw_pe_color("Color Start##pe", "color_start");
            draw_pe_color("Color End##pe",   "color_end");

            ImGui::Spacing();
            // Presets
            ImGui::TextUnformatted("Presets:"); ImGui::SameLine(0,6);
            struct PEPreset { const char* name; nlohmann::json fields; };
            static const PEPreset presets[] = {
                {"Fire",    {{"rate",40},{"lifetime",1.2},{"speed",80},{"spread",40},{"gravity_scale",-0.4},
                             {"size_start",8},{"size_end",1},{"direction_angle",-90},{"speed_variation",0.4},
                             {"color_start",nlohmann::json::array({255,160,30,255})},
                             {"color_end",  nlohmann::json::array({255,30,0,0})}}},
                {"Smoke",   {{"rate",12},{"lifetime",3.0},{"speed",30},{"spread",25},{"gravity_scale",-0.15},
                             {"size_start",6},{"size_end",18},{"direction_angle",-90},{"speed_variation",0.3},
                             {"color_start",nlohmann::json::array({180,180,180,180})},
                             {"color_end",  nlohmann::json::array({120,120,120,0})}}},
                {"Sparks",  {{"burst",true},{"burst_count",40},{"lifetime",0.6},{"speed",200},{"spread",360},
                             {"gravity_scale",0.8},{"size_start",3},{"size_end",0},{"speed_variation",0.5},
                             {"color_start",nlohmann::json::array({255,230,80,255})},
                             {"color_end",  nlohmann::json::array({255,100,10,0})}}},
                {"Rain",    {{"rate",80},{"lifetime",1.0},{"speed",400},{"spread",5},{"gravity_scale",0.0},
                             {"size_start",2},{"size_end",2},{"direction_angle",90},{"speed_variation",0.1},
                             {"color_start",nlohmann::json::array({160,200,255,200})},
                             {"color_end",  nlohmann::json::array({160,200,255,0})}}},
                {"Explode", {{"burst",true},{"burst_count",60},{"lifetime",0.8},{"speed",250},{"spread",360},
                             {"gravity_scale",0.5},{"size_start",10},{"size_end",0},{"speed_variation",0.6},
                             {"sub_emitter_on_death",true},{"sub_emitter_count",4},{"sub_emitter_speed",80},
                             {"color_start",nlohmann::json::array({255,200,50,255})},
                             {"color_end",  nlohmann::json::array({200,50,0,0})}}},
                {"Magic",   {{"rate",25},{"lifetime",1.5},{"speed",60},{"spread",360},{"gravity_scale",-0.1},
                             {"size_start",5},{"size_end",0},{"speed_variation",0.5},
                             {"color_start",nlohmann::json::array({180,80,255,255})},
                             {"color_end",  nlohmann::json::array({80,200,255,0})}}},
            };
            for (auto& p : presets) {
                if (ImGui::SmallButton(p.name)) {
                    for (auto& [k,v] : p.fields.items()) comp[k] = v;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Apply %s preset.", p.name);
                ImGui::SameLine(0,4);
            }
            ImGui::NewLine();
        }

        // ── UI anchor picker (UIPanel, UIText, UIButton, UIImage, UIProgressBar) ─
        if (ctype == "UIPanel" || ctype == "UIText" || ctype == "UIButton" ||
            ctype == "UIImage" || ctype == "UIProgressBar" || ctype == "UILayoutGroup") {
            ImGui::Spacing();
            ImGui::TextUnformatted("Anchor:");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click a cell to set the anchor preset.\nHold Shift to also move the element to that anchor position.");
            ImGui::SameLine(0,6);
            // 3x3 grid of anchor presets: [col,row] → (anchor_x, anchor_y)
            static const float ax_vals[3] = {0.f, 0.5f, 1.f};
            static const float ay_vals[3] = {0.f, 0.5f, 1.f};
            static const char* alabel[3][3] = {
                {"\xe2\x86\x96","  \xe2\x86\x91  ","\xe2\x86\x97"}, // ↖ ↑ ↗
                {"\xe2\x86\x90","  \xe2\x97\x8f  ","\xe2\x86\x92"}, // ← ● →
                {"\xe2\x86\x99","  \xe2\x86\x93  ","\xe2\x86\x98"}, // ↙ ↓ ↘
            };
            float cur_ax = comp.value("anchor_x", 0.5f);
            float cur_ay = comp.value("anchor_y", 0.5f);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(1,1));
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 3; ++col) {
                    if (col > 0) ImGui::SameLine(0,1);
                    bool active = (std::abs(cur_ax - ax_vals[col]) < 0.01f &&
                                   std::abs(cur_ay - ay_vals[row]) < 0.01f);
                    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f,0.49f,0.91f,1.f));
                    else        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f,0.22f,0.22f,1.f));
                    char bid[16]; snprintf(bid,sizeof(bid),"%s##anc%d%d", alabel[row][col], row, col);
                    if (ImGui::Button(bid, ImVec2(26,22))) {
                        comp["anchor_x"] = ax_vals[col];
                        comp["anchor_y"] = ay_vals[row];
                        if (ImGui::GetIO().KeyShift) {
                            // Also snap pos to match anchor
                            comp["pos_x"] = 0.f; comp["pos_y"] = 0.f;
                        }
                    }
                    ImGui::PopStyleColor();
                }
            }
            ImGui::PopStyleVar();
            ImGui::SameLine(0,8);
            ImGui::BeginGroup();
            ImGui::SetNextItemWidth(70.f);
            float anx = comp.value("anchor_x",0.5f);
            if (ImGui::DragFloat("anc X##ui",&anx,0.01f,0.f,1.f,"%.2f")) comp["anchor_x"]=anx;
            ImGui::SetNextItemWidth(70.f);
            float any = comp.value("anchor_y",0.5f);
            if (ImGui::DragFloat("anc Y##ui",&any,0.01f,0.f,1.f,"%.2f")) comp["anchor_y"]=any;
            ImGui::EndGroup();
            // Fall through so pos/width/height/color still show via generic loop
        }

        // ── CustomRenderTexture2D: live procedural texture controls ───────────
        if (ctype == "CustomRenderTexture2D") {
            ImGui::SeparatorText("Runtime Texture");
            bool enabled = comp.value("enabled", true);
            if (ImGui::Checkbox("Enabled##crt", &enabled)) comp["enabled"] = enabled;
            int width = std::clamp(comp.value("width", 256), 1, 2048);
            int height = std::clamp(comp.value("height", 256), 1, 2048);
            ImGui::SetNextItemWidth(105.f);
            if (ImGui::InputInt("Width##crt", &width)) comp["width"] = std::clamp(width, 1, 2048);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(105.f);
            if (ImGui::InputInt("Height##crt", &height)) comp["height"] = std::clamp(height, 1, 2048);

            const char* generators[] = {"solid", "checker", "radial", "noise"};
            std::string generator = comp.value("generator", std::string("solid"));
            if (ImGui::BeginCombo("Generator##crt", generator.c_str())) {
                for (const char* option : generators) {
                    if (ImGui::Selectable(option, generator == option)) comp["generator"] = option;
                }
                ImGui::EndCombo();
            }
            const auto color = comp.value("clear_color", std::vector<int>{255,255,255,255});
            float rgba[4] = {1,1,1,1};
            for (int index = 0; index < 4 && index < (int)color.size(); ++index) rgba[index] = color[index] / 255.f;
            if (ImGui::ColorEdit4("Color##crt", rgba, ImGuiColorEditFlags_AlphaBar)) {
                comp["clear_color"] = nlohmann::json::array({(int)(rgba[0]*255.f), (int)(rgba[1]*255.f),
                                                               (int)(rgba[2]*255.f), (int)(rgba[3]*255.f)});
            }
            if (generator == "checker") {
                int checker_size = std::clamp(comp.value("checker_size", 16), 1, 512);
                ImGui::SetNextItemWidth(120.f);
                if (ImGui::DragInt("Cell Size##crt", &checker_size, 1.f, 1, 512)) comp["checker_size"] = checker_size;
            }
            if (generator == "noise") {
                int seed = comp.value("seed", 1);
                ImGui::SetNextItemWidth(120.f);
                if (ImGui::InputInt("Seed##crt", &seed)) comp["seed"] = seed;
            }

            const char* update_modes[] = {"on_demand", "realtime"};
            std::string update_mode = comp.value("update_mode", std::string("on_demand"));
            if (ImGui::BeginCombo("Update Mode##crt", update_mode == "realtime" ? "Realtime" : "On Demand")) {
                for (const char* mode : update_modes) {
                    const char* label = std::strcmp(mode, "realtime") == 0 ? "Realtime" : "On Demand";
                    if (ImGui::Selectable(label, update_mode == mode)) comp["update_mode"] = mode;
                }
                ImGui::EndCombo();
            }
            if (update_mode == "realtime") {
                float interval = std::clamp(comp.value("update_interval", 1.f/15.f), 1.f/120.f, 5.f);
                ImGui::SetNextItemWidth(140.f);
                if (ImGui::DragFloat("Interval##crt", &interval, .005f, 1.f/120.f, 5.f, "%.3f s")) comp["update_interval"] = interval;
                float speed = comp.value("animation_speed", 1.f);
                ImGui::SetNextItemWidth(140.f);
                if (ImGui::DragFloat("Animation Speed##crt", &speed, .05f, -10.f, 10.f)) comp["animation_speed"] = speed;
            }
            if (ImGui::Button("Update Now##crt")) comp["request_update"] = true;
            ImGui::SameLine();
            ImGui::TextDisabled("Drives the owner SpriteRenderer in Play/export.");
        }

        // ── VideoPlayer2D: self-contained animated-GIF playback ───────────────
        if (ctype == "VideoPlayer2D") {
            ImGui::SeparatorText("Animated GIF Playback");
            std::string clip = comp.value("clip", std::string());
            if (_draw_asset_slot(st, "Clip", clip, {".gif"},
                                 "Animated GIF (*.gif)\0*.gif\0\0", "Select Animated GIF"))
                comp["clip"] = clip;
            bool enabled = comp.value("enabled", true);
            if (ImGui::Checkbox("Enabled##video", &enabled)) comp["enabled"] = enabled;
            ImGui::SameLine();
            bool play_on_awake = comp.value("play_on_awake", false);
            if (ImGui::Checkbox("Play On Awake##video", &play_on_awake)) comp["play_on_awake"] = play_on_awake;
            bool playing = comp.value("playing", false);
            if (ImGui::Checkbox("Playing##video", &playing)) comp["playing"] = playing;
            ImGui::SameLine();
            bool loop = comp.value("loop", false);
            if (ImGui::Checkbox("Loop##video", &loop)) comp["loop"] = loop;
            float speed = std::clamp(comp.value("playback_speed", 1.f), 0.f, 8.f);
            ImGui::SetNextItemWidth(150.f);
            if (ImGui::DragFloat("Playback Speed##video", &speed, .05f, 0.f, 8.f)) comp["playback_speed"] = speed;
            float time = std::max(0.f, comp.value("playback_time", 0.f));
            ImGui::SetNextItemWidth(150.f);
            if (ImGui::DragFloat("Preview Time##video", &time, .01f, 0.f, 3600.f, "%.2f s")) comp["playback_time"] = time;
            ImGui::SameLine();
            if (ImGui::Button("Restart##video")) comp["restart"] = true;
            ImGui::TextDisabled("GIF playback is rendered directly on this entity's SpriteRenderer.");
        }

        // ── Waypoint2D: ordered entity list with add/remove ───────────────────
        if (ctype == "Waypoint2D") {
            auto& path = comp["path"];
            if (!path.is_array()) path = nlohmann::json::array();
            ImGui::Text("Waypoints (%d):", (int)path.size());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ordered list of entity IDs this path visits.\nDelete with the X button. Add with the picker below.");
            int del_idx = -1;
            for (int i = 0; i < (int)path.size(); ++i) {
                int wid = path[i].get<int>();
                Entity* we = st.find_entity(wid);
                std::string wname = we ? we->value("name","Entity")+"  ["+std::to_string(wid)+"]" : "(missing) ["+std::to_string(wid)+"]";
                ImGui::PushID(i);
                ImGui::BulletText("%d. %s", i+1, wname.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("X##wpdel")) del_idx = i;
                // Move up/down
                if (i > 0) {
                    ImGui::SameLine(0,2);
                    if (ImGui::SmallButton("^##wpup")) std::swap(path[i], path[i-1]);
                }
                if (i < (int)path.size()-1) {
                    ImGui::SameLine(0,2);
                    if (ImGui::SmallButton("v##wpdn")) std::swap(path[i], path[i+1]);
                }
                ImGui::PopID();
            }
            if (del_idx >= 0) path.erase_at((size_t)del_idx);
            // Add picker
            ImGui::SetNextItemWidth(200.f);
            if (ImGui::BeginCombo("Add Waypoint##wp", "Pick entity...")) {
                int self_id = e.value("id",-1);
                for (auto& cand : st.entities) {
                    if (cand.value("_runtime_only",false) || cand.value("id",-1)==self_id) continue;
                    int cid = cand.value("id",-1);
                    std::string cn = cand.value("name","Entity")+"  ["+std::to_string(cid)+"]";
                    if (ImGui::Selectable(cn.c_str(), false)) path.push_back(cid);
                }
                ImGui::EndCombo();
            }
            // Path behaviour + gizmo color
            bool wp_loop = comp.value("loop", true);
            bool wp_rev  = comp.value("reverse", false);
            if (ImGui::Checkbox("Loop##wp", &wp_loop))    comp["loop"]    = wp_loop;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cycle back to waypoint 0 after reaching the last one.");
            ImGui::SameLine(0,8);
            if (ImGui::Checkbox("Reverse##wp", &wp_rev)) comp["reverse"] = wp_rev;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Travel the waypoints in reverse order. Combine with Loop for a ping-pong patrol.");

            float gc[4]={0,1,0.4f,0.78f};
            auto gcv = comp.value("gizmo_color", std::vector<float>{0,255,100,200});
            for (int i=0;i<4&&i<(int)gcv.size();++i) gc[i]=gcv[i]/255.f;
            ImGui::SetNextItemWidth(180.f);
            if (ImGui::ColorEdit4("Gizmo Color##wp", gc, ImGuiColorEditFlags_AlphaBar))
                comp["gizmo_color"]=nlohmann::json::array({(int)(gc[0]*255),(int)(gc[1]*255),(int)(gc[2]*255),(int)(gc[3]*255)});
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Color of the path gizmo drawn in the scene view.");
        }

        // ── EventEmitter: readable layout + inline payload key-value editor ───
        if (ctype == "EventEmitter") {
            std::string evname = comp.value("event_name","");
            char evbuf[128]; snprintf(evbuf,sizeof(evbuf),"%s",evname.c_str());
            ImGui::SetNextItemWidth(180.f);
            if (ImGui::InputText("Event Name##ev", evbuf, sizeof(evbuf))) comp["event_name"] = std::string(evbuf);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Name of the event broadcast to all scripts listening for it.");

            bool on_start = comp.value("emit_on_start",true);
            bool once     = comp.value("once",false);
            if (ImGui::Checkbox("Emit On Start##ev",&on_start)) comp["emit_on_start"]=on_start;
            ImGui::SameLine(0,10);
            if (ImGui::Checkbox("Once##ev",&once)) comp["once"]=once;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fire only on the first trigger, then disable itself.");

            float every = comp.value("emit_every",0.f);
            ImGui::SetNextItemWidth(100.f);
            if (ImGui::DragFloat("Repeat Every##ev",&every,0.05f,0.f,9999.f,"%.2f s")) comp["emit_every"]=every;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = fire once per trigger. >0 = repeat at this interval (seconds).");

            // Payload key-value editor
            auto& pl = comp["payload"];
            if (!pl.is_object()) pl = nlohmann::json::object();
            if (ImGui::CollapsingHeader("Payload##ev")) {
                ImGui::TextDisabled("String key-value pairs sent with the event.");
                int del_pk = -1; int pk_idx = 0;
                for (auto& [k, v] : pl.items()) {
                    ImGui::PushID(pk_idx);
                    ImGui::SetNextItemWidth(90.f);
                    char kbuf[64]; snprintf(kbuf,sizeof(kbuf),"%s",k.c_str());
                    // Key is immutable once set — show it as disabled text
                    ImGui::TextDisabled("%s", k.c_str());
                    ImGui::SameLine(0,4);
                    std::string vs = static_cast<std::string>(v);
                    char vbuf[128]; snprintf(vbuf,sizeof(vbuf),"%s",vs.c_str());
                    ImGui::SetNextItemWidth(110.f);
                    if (ImGui::InputText("##plv", vbuf, sizeof(vbuf))) pl[k] = std::string(vbuf);
                    ImGui::SameLine(0,4);
                    if (ImGui::SmallButton("X##pldel")) del_pk = pk_idx;
                    ImGui::PopID(); ++pk_idx;
                }
                if (del_pk >= 0) {
                    int i2 = 0;
                    std::string key_to_erase;
                    for (auto& [k, v] : pl.items()) {
                        if (i2++ == del_pk) { key_to_erase = k; break; }
                    }
                    if (!key_to_erase.empty()) pl.erase(key_to_erase);
                }
                // Add new key
                static char new_pk[64]={}, new_pv[128]={};
                ImGui::SetNextItemWidth(90.f); ImGui::InputText("Key##plnk", new_pk, sizeof(new_pk));
                ImGui::SameLine(0,4);
                ImGui::SetNextItemWidth(110.f); ImGui::InputText("Val##plnv", new_pv, sizeof(new_pv));
                ImGui::SameLine(0,4);
                if (ImGui::SmallButton("+##pladd") && new_pk[0]) {
                    pl[std::string(new_pk)] = std::string(new_pv);
                    new_pk[0] = new_pv[0] = '\0';
                }
            }
            // enabled checkbox shown in the component header toggle above
        }

        // ── TextMeshPro2D: multi-line text input + alignment picker ───────────
        if (ctype == "TextMeshPro2D") {
            std::string txt = comp.value("text","Text");
            char tbuf[1024]; snprintf(tbuf,sizeof(tbuf),"%s",txt.c_str());
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 4.f);
            if (ImGui::InputTextMultiline("##tmp_text", tbuf, sizeof(tbuf), ImVec2(0,60)))
                comp["text"] = std::string(tbuf);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Supports rich text markup, e.g. <b>bold</b>, <color=#ff0000>red</color>.");

            // Font references are always project-relative asset references.
            // This uses the same compatible Browse/drag-drop control as other
            // renderer assets rather than exposing a fragile manual path field.
            std::string font = comp.value("font", std::string());
            if (_draw_asset_slot(st, "Font", font, {".ttf", ".otf"},
                                 "Font Files (*.ttf;*.otf)\0*.ttf;*.otf\0\0",
                                 "Select Font"))
                comp["font"] = font;

            // Alignment picker — 3 buttons
            std::string align = comp.value("alignment","center");
            auto abtn = [&](const char* lbl, const char* val) {
                bool on = (align == val);
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f,0.49f,0.91f,1.f));
                else    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f,0.28f,0.28f,1.f));
                if (ImGui::Button(lbl)) comp["alignment"] = val;
                ImGui::PopStyleColor();
                ImGui::SameLine(0,2);
            };
            abtn("\xe2\x98\xb0 L##tmp","left"); abtn("= C##tmp","center"); abtn("\xe2\x98\xb1 R##tmp","right");
            ImGui::NewLine();

            // Font size slider
            float fs = comp.value("font_size", 24.f);
            ImGui::SetNextItemWidth(140.f);
            if (ImGui::SliderFloat("Font Size##tmp",&fs,6.f,120.f,"%.0f")) comp["font_size"]=fs;

            // Color + outline pickers
            auto draw_col = [&](const char* label, const char* key) {
                float c[4]={1,1,1,1};
                auto cv = comp.value(key, std::vector<float>{255,255,255,255});
                for (int i=0;i<4&&i<(int)cv.size();++i) c[i]=cv[i]/255.f;
                if (ImGui::ColorEdit4(label,c,ImGuiColorEditFlags_AlphaBar))
                    comp[key]=nlohmann::json::array({(int)(c[0]*255),(int)(c[1]*255),(int)(c[2]*255),(int)(c[3]*255)});
            };
            draw_col("Color##tmp","color");
            float ow = comp.value("outline_width",0.f);
            ImGui::SetNextItemWidth(120.f);
            if (ImGui::SliderFloat("Outline##tmp",&ow,0.f,0.5f,"%.2f")) comp["outline_width"]=ow;
            if (ow > 0.f) draw_col("Outline Color##tmp","outline_color");
            // Text layout and rendering options
            bool wrapping = comp.value("wrapping", true);
            bool rich_txt = comp.value("rich_text", true);
            bool auto_sz  = comp.value("auto_size", false);
            if (ImGui::Checkbox("Word Wrap##tmp", &wrapping)) comp["wrapping"]   = wrapping;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Wrap text that exceeds the bounds width onto a new line.");
            ImGui::SameLine(0,8);
            if (ImGui::Checkbox("Rich Text##tmp", &rich_txt)) comp["rich_text"]  = rich_txt;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enable <b>bold</b>, <color=#...>color</color> and other markup tags.");
            ImGui::SameLine(0,8);
            if (ImGui::Checkbox("Auto Size##tmp",&auto_sz))   comp["auto_size"]  = auto_sz;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Automatically shrink font size to fit inside the bounds box.");

            if (auto_sz) {
                float mn = comp.value("min_size", 8.f), mx = comp.value("max_size", 72.f);
                ImGui::SetNextItemWidth(80.f);
                if (ImGui::DragFloat("Min##tmp_as",&mn,0.5f,4.f,mx,"%.0f"))  comp["min_size"]=mn;
                ImGui::SameLine(0,4);
                ImGui::SetNextItemWidth(80.f);
                if (ImGui::DragFloat("Max##tmp_as",&mx,0.5f,mn,300.f,"%.0f")) comp["max_size"]=mx;
            }

            // Bounds box (controls wrapping and auto-size region)
            float bw = comp.value("bounds_w", 200.f), bh = comp.value("bounds_h", 0.f);
            ImGui::SetNextItemWidth(100.f);
            if (ImGui::DragFloat("Bounds W##tmp",&bw,1.f,0.f,10000.f,"%.0f u")) comp["bounds_w"]=bw;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Maximum width before text wraps. 0=no limit.");
            ImGui::SameLine(0,4);
            ImGui::SetNextItemWidth(100.f);
            if (ImGui::DragFloat("H##tmp_bh",&bh,1.f,0.f,10000.f,"%.0f u")) comp["bounds_h"]=bh;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Maximum height for overflow clipping. 0=no clipping.");

            // Face dilate: SDF spread — widens the glyph slightly (useful for thin fonts)
            float fd = comp.value("face_dilate", 0.f);
            ImGui::SetNextItemWidth(140.f);
            if (ImGui::SliderFloat("Face Dilate##tmp",&fd,-1.f,1.f,"%.2f")) comp["face_dilate"]=fd;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("SDF glyph spread. Positive = fatten glyphs, negative = thin them.\nUseful for making small text more readable.");
        }

        // ── ConstantForce2D: direction wheel ──────────────────────────────────
        if (ctype == "ConstantForce2D") {
            float fx = comp.value("force_x",0.f), fy = comp.value("force_y",0.f);
            float mag = std::sqrt(fx*fx+fy*fy);
            float angle_deg = (mag > 0.001f) ? std::atan2(fy,fx)*180.f/3.14159f : 0.f;

            // Direction knob: drag around a small circle widget
            ImVec2 cpos = ImGui::GetCursorScreenPos();
            float r = 28.f;
            ImGui::Dummy(ImVec2(r*2+4, r*2+4));
            ImDrawList* wdl = ImGui::GetWindowDrawList();
            ImVec2 ctr{cpos.x+r+2, cpos.y+r+2};
            wdl->AddCircle(ctr, r, IM_COL32(80,80,80,200), 32);
            float rad = angle_deg * 3.14159f / 180.f;
            float hx = ctr.x + std::cos(rad)*r, hy = ctr.y + std::sin(rad)*r;
            wdl->AddLine(ctr, {hx,hy}, IM_COL32(255,140,0,255), 2.f);
            wdl->AddCircleFilled({hx,hy}, 5.f, IM_COL32(255,140,0,255));
            // Drag interaction on the knob
            ImGui::SetCursorScreenPos(cpos);
            ImGui::InvisibleButton("##cf_knob", ImVec2(r*2+4,r*2+4));
            if (ImGui::IsItemActive()) {
                ImVec2 mp = ImGui::GetIO().MousePos;
                float dx = mp.x - ctr.x, dy = mp.y - ctr.y;
                float new_ang = std::atan2(dy,dx);
                float new_mag = std::max(0.f, mag); if (new_mag < 1.f) new_mag = 10.f;
                comp["force_x"] = new_mag * std::cos(new_ang);
                comp["force_y"] = new_mag * std::sin(new_ang);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag to set force direction.\nAdjust magnitude with the sliders.");
            ImGui::SameLine(0,8);
            ImGui::BeginGroup();
            ImGui::SetNextItemWidth(120.f);
            if (ImGui::DragFloat("Magnitude##cf",&mag,1.f,0.f,10000.f,"%.1f")) {
                float a = angle_deg * 3.14159f / 180.f;
                comp["force_x"] = mag * std::cos(a);
                comp["force_y"] = mag * std::sin(a);
            }
            ImGui::SetNextItemWidth(120.f);
            if (ImGui::DragFloat("Angle##cf",&angle_deg,1.f,-360.f,360.f,"%.1f°")) {
                float a = angle_deg * 3.14159f / 180.f;
                comp["force_x"] = mag * std::cos(a);
                comp["force_y"] = mag * std::sin(a);
            }
            float torque = comp.value("torque",0.f);
            ImGui::SetNextItemWidth(120.f);
            if (ImGui::DragFloat("Torque##cf",&torque,0.1f)) comp["torque"]=torque;
            ImGui::EndGroup();
            // Relative force (applied in the entity's local axes, rotates with the body)
            float rfx = comp.value("relative_force_x", 0.f), rfy = comp.value("relative_force_y", 0.f);
            float rfmag = std::sqrt(rfx*rfx + rfy*rfy);
            float rfang = (rfmag > 0.001f) ? std::atan2(rfy,rfx)*180.f/3.14159f : 0.f;
            ImGui::Spacing();
            ImGui::TextUnformatted("Relative Force (local axes):");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Force applied in the body\'s own local coordinate frame.\nRotates with the entity — useful for thrusters, fans, directional jets.");
            ImGui::SetNextItemWidth(120.f);
            if (ImGui::DragFloat("Magnitude##cf_r",&rfmag,1.f,0.f,10000.f,"%.1f")) {
                float a = rfang * 3.14159f / 180.f;
                comp["relative_force_x"] = rfmag * std::cos(a);
                comp["relative_force_y"] = rfmag * std::sin(a);
            }
            ImGui::SameLine(0,4);
            ImGui::SetNextItemWidth(100.f);
            if (ImGui::DragFloat("Angle##cf_r",&rfang,1.f,-360.f,360.f,"%.1fÂ°")) {
                float a = rfang * 3.14159f / 180.f;
                comp["relative_force_x"] = rfmag * std::cos(a);
                comp["relative_force_y"] = rfmag * std::sin(a);
            }
        }

        // ── Tilemap: persistent paint palette ───────────────────────────────
        // Paint tools belong with the selected Tilemap, not in a temporary
        // viewport row that vanishes when another tool is selected. This also
        // makes it explicit which tileset every preset will apply to.
        if (ctype == "Tilemap") {
            ImGui::SeparatorText("Tile Painting");
            ImGui::TextDisabled("Configure the brush here, then paint in the Viewport.");

            const bool paint_active = st.tool == "paint";
            if (paint_active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.48f, 0.84f, 1.f));
            if (ImGui::Button(paint_active ? "Painting in Viewport" : "Paint Tilemap", {-1, 0})) st.tool = "paint";
            if (paint_active) ImGui::PopStyleColor();

            std::string palette_ref = comp.value("tile_palette", std::string());
            const std::string previous_palette_ref = palette_ref;
            if (_draw_asset_slot(st, "Tile Palette", palette_ref, {".json"},
                                 "Tile Palette (*.tilepalette.json)\0*.tilepalette.json\0\0", "Select Tile Palette")) {
                const std::string filename = fs::path(palette_ref).filename().string();
                if (filename.size() >= 17 && filename.rfind(".tilepalette.json") == filename.size() - 17) {
                    bool has_painted_cells = false;
                    if (comp.contains("grid") && comp["grid"].is_array()) {
                        for (const auto& row : comp["grid"]) {
                            if (!row.is_array()) continue;
                            for (const auto& cell : row) {
                                if (cell.is_number_integer() && cell.get<int>() >= 0) { has_painted_cells = true; break; }
                            }
                            if (has_painted_cells) break;
                        }
                    }
                    // Palette tile IDs are mapping keys.  Swapping a palette
                    // on a painted Tilemap would reinterpret every existing
                    // key against a different image atlas, which was the
                    // source of the reported "changing one tile changes the
                    // viewport" data loss.  Never remap silently.
                    if (has_painted_cells && palette_ref != previous_palette_ref) {
                        palette_ref = previous_palette_ref;
                        st.log_warn("Palette assignment blocked: this Tilemap already has painted cells. Clear or duplicate the Tilemap before assigning a palette, so existing tile IDs are never silently reinterpreted as different art.");
                    } else {
                        comp["tile_palette"] = palette_ref;
                        st.active_tile_palette = palette_ref;
                        // Palette cells are the authoritative paint/render grid. A
                        // new palette commonly uses 32px tiles while a legacy map
                        // retains 64px; leaving that value behind makes painting
                        // and Pick Tile land on different cells from the renderer.
                        try {
                            std::ifstream input(fs::path(st.asset_dir) / palette_ref);
                            nlohmann::json manifest;
                            if (input && (input >> manifest) && manifest.is_object() &&
                                manifest.value("format", std::string()) == "gameengine.tile-palette") {
                                const int cell_w = std::max(1, manifest.value("cell_width", 32));
                                const int cell_h = std::max(1, manifest.value("cell_height", cell_w));
                                comp["tile_size"] = cell_w;
                                comp["_grid_cell_width"] = cell_w;
                                comp["_grid_cell_height"] = cell_h;
                            } else {
                                st.log_warn("Tile Palette could not be read; its cell size was not applied.");
                            }
                        } catch (const std::exception&) {
                            st.log_warn("Tile Palette could not be read; its cell size was not applied.");
                        }
                    }
                } else st.log_warn("A Tilemap requires a .tilepalette.json asset.");
            }
            ImGui::SameLine();
            if (ImGui::Button("Open Palette##tilepaint")) {
                st.requested_tile_palette_asset = palette_ref;
                st.request_tile_palette_open = true;
            }

            if (palette_ref.empty()) {
                ImGui::TextDisabled("No Tile Palette assigned. Create one with Open Palette; legacy sprite-sheet settings remain available below.");
                std::string tileset = comp.value("tileset", std::string());
                if (_draw_asset_slot(st, "Legacy Tile Sheet", tileset,
                                     {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif"},
                                     "Image Files (*.png;*.jpg;*.jpeg;*.bmp;*.tga)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0\0",
                                     "Select Legacy Tile Sheet"))
                    comp["tileset"] = tileset;
                int tile_size = std::max(1, comp.value("tile_size", 32));
                ImGui::SetNextItemWidth(105.f);
                if (ImGui::InputInt("Tile Size##tilepaint", &tile_size, 1, 8)) comp["tile_size"] = std::max(1, tile_size);
            } else {
                ImGui::TextDisabled("Palette: %s", fs::path(palette_ref).parent_path().filename().string().c_str());
                if (_tile_palette_picker) _tile_palette_picker(st, e);
                else ImGui::TextDisabled("Tile Palette preview is not initialized yet.");
            }

            std::string filter_mode = comp.value("filter_mode", std::string("point"));
            const char* filter_label = filter_mode == "bilinear" ? "Bilinear" : "Point (Pixel Art)";
            ImGui::SetNextItemWidth(150.f);
            if (ImGui::BeginCombo("Filter##tilepaint", filter_label)) {
                if (ImGui::Selectable("Point (Pixel Art)", filter_mode == "point")) comp["filter_mode"] = "point";
                if (ImGui::Selectable("Bilinear", filter_mode == "bilinear")) comp["filter_mode"] = "bilinear";
                ImGui::EndCombo();
            }
            bool generate_colliders = comp.value("generate_colliders", false);
            if (ImGui::Checkbox("Generate Colliders##tilepaint", &generate_colliders))
                comp["generate_colliders"] = generate_colliders;

            if (palette_ref.empty()) {
                ImGui::SetNextItemWidth(105.f);
                if (ImGui::InputInt("Tile ID##tilepaint", &st.paint_tile, 1, 1))
                    st.paint_tile = std::max(0, st.paint_tile);
                ImGui::SameLine(0, 8);
                ImGui::SetNextItemWidth(105.f);
                if (ImGui::InputInt("Brush##tilepaint", &st.paint_brush_size, 1, 1))
                    st.paint_brush_size = std::max(1, std::min(st.paint_brush_size, 16));
            }

            ImGui::Checkbox("Erase##tilepaint", &st.paint_erase);
            ImGui::SameLine(0, 12);
            ImGui::Checkbox("Rectangle Fill##tilepaint", &st.paint_rect_mode);
            ImGui::SameLine(0, 12);
            ImGui::Checkbox("Pick Tile##tilepaint", &st.paint_eyedropper);

            ImGui::Spacing();
            ImGui::TextDisabled(palette_ref.empty()
                ? "Legacy one-tile painting. Create a Tile Palette for visual multi-tile brushes."
                : "Click tiles above; Ctrl-click adds/removes tiles and Shift-click selects a range. Saved brushes live in Tile Palette.");
        }

        if (comp.is_object()) {
            for (auto& [key, val] : comp.items()) {
                if (should_hide_key(key)) continue;
                // Skip Transform fields already drawn above
                if (ctype=="Transform" && (key=="x"||key=="y"||key=="rotation"||key=="scale_x"||key=="scale_y"||key=="parent")) continue;
                // Tilemap paint fields have purpose-built controls above. Do
                // not duplicate them as raw generic fields below.
                if (ctype=="Tilemap" && (key=="tile_size" || key=="tileset" || key=="tile_palette" || key=="tile_collision" || key=="filter_mode" ||
                    key=="generate_colliders" || key=="grid")) continue;
                if (ctype=="CustomRenderTexture2D" &&
                    (key=="enabled" || key=="width" || key=="height" || key=="format" || key=="depth_bits" ||
                     key=="filter_mode" || key=="wrap_mode" || key=="update_mode" || key=="update_interval" ||
                     key=="generator" || key=="clear_color" || key=="checker_size" || key=="seed" ||
                     key=="animation_speed" || key=="request_update" || key=="double_buffered")) continue;
                if (ctype=="VideoPlayer2D" &&
                    (key=="enabled" || key=="clip" || key=="play_on_awake" || key=="loop" || key=="playing" ||
                     key=="restart" || key=="playback_time" || key=="playback_speed")) continue;
                std::string lbl = key;
                // Replace underscores with spaces and capitalize
                std::replace(lbl.begin(),lbl.end(),'_',' ');

                ImGui::PushID(key.c_str());
                bool field_overridden = prefab::is_field_overridden(e, ctype, key);
                if (field_overridden) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.72f,0.86f,1.0f,1.f));

                if (val.is_boolean()) {
                    bool b = val.get<bool>();
                    if (ImGui::Checkbox(lbl.c_str(), &b)) { comp[key] = b; if (ctype=="Transform") transform_changed = true; }
                }
                else if (val.is_number_integer()) {
                    int v = val.get<int>();
                    ImGui::SetNextItemWidth(140);
                    if (ImGui::InputInt(lbl.c_str(), &v)) { comp[key] = v; if (ctype=="Transform") transform_changed = true; }
                }
                else if (val.is_number_float()) {
                    float v = val.get<float>();
                    ImGui::SetNextItemWidth(140);
                    float speed = (key=="x"||key=="y"||key=="rotation") ? 1.f : 0.5f;
                    if (ImGui::DragFloat(lbl.c_str(), &v, speed)) { comp[key] = v; if (ctype=="Transform") transform_changed = true; }
                }
                else if (val.is_string()) {
                    std::string sv = val.get<std::string>();
                    bool handled = false;

                    // Sorting-layer fields get a combo populated from the
                    // project's named sorting layers (Unity-style "Sorting
                    // Layer" dropdown) instead of a free-text box, so it's
                    // impossible to typo a layer name into one that doesn't
                    // exist and silently fall back to layer 0.
                    if (key == "sorting_layer" || key == "front_sorting_layer" || key == "back_sorting_layer") {
                        std::string cur = sv.empty() ? "Default" : sv;
                        ImGui::SetNextItemWidth(160);
                        if (ImGui::BeginCombo(lbl.c_str(), cur.c_str())) {
                            for (auto& layer_name : st.sorting_layers) {
                                bool sel = (sv == layer_name) || (sv.empty() && layer_name == "Default");
                                if (ImGui::Selectable(layer_name.c_str(), sel)) comp[key] = layer_name;
                            }
                            ImGui::EndCombo();
                        }
                        handled = true;
                    }
                    // Known fixed-choice fields (draw_mode, filter_mode, ...)
                    // get a real dropdown instead of free text — see
                    // enum_field_options() for the full list.
                    else if (auto* opts = enum_field_options(key)) {
                        std::string cur_disp = sv;
                        for (auto& [val_str, disp] : *opts) if (val_str == sv) { cur_disp = disp; break; }
                        ImGui::SetNextItemWidth(180);
                        if (ImGui::BeginCombo(lbl.c_str(), cur_disp.c_str())) {
                            for (auto& [val_str, disp] : *opts) {
                                if (ImGui::Selectable(disp.c_str(), val_str == sv)) comp[key] = val_str;
                            }
                            ImGui::EndCombo();
                        }
                        handled = true;
                    }
                    // Texture/sprite fields accept a drag-and-drop image asset
                    // from the Assets panel (ASSET_PATH payload), same as
                    // dragging a sprite onto Unity's Inspector.
                    else if (key == "texture" || key == "tileset" || key == "clip" || key == "sprite" || key == "atlas_texture") {
                        const bool is_audio = key == "clip";
#ifdef NO_SDL_MIXER
                        // The core SDL fallback is intentionally WAV-only.
                        // Do not accept OGG/MP3 assets in an Editor build
                        // that cannot actually decode them at runtime.
                        const std::initializer_list<const char*> audio_extensions = {".wav"};
                        const char* audio_filter = "WAV Files (*.wav)\0*.wav\0\0";
#else
                        const std::initializer_list<const char*> audio_extensions = {".wav", ".ogg", ".mp3", ".flac"};
                        const char* audio_filter = "Audio Files (*.wav;*.ogg;*.mp3;*.flac)\0*.wav;*.ogg;*.mp3;*.flac\0\0";
#endif
                        if (_draw_asset_slot(st, lbl.c_str(), sv,
                                is_audio ? audio_extensions
                                         : std::initializer_list<const char*>{".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif"},
                                is_audio ? audio_filter
                                         : "Image Files (*.png;*.jpg;*.jpeg;*.bmp;*.tga)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0\0",
                                is_audio ? "Select Audio Clip" : "Select Image Asset"))
                            comp[key] = sv;
#ifdef NO_SDL_MIXER
                        if (is_audio) ImGui::TextDisabled("WAV clips are supported by this build's SDL audio fallback.");
#endif
                        handled = true;
                    }
                    // Material field (SpriteRenderer.material): same
                    // drag-and-drop from Assets as texture, plus a "Select"
                    // button that jumps the Inspector to that material's own
                    // properties — same as double-clicking a Material
                    // reference field in Unity's Inspector.
                    else if (key == "material") {
                        if (_draw_asset_slot(st, lbl.c_str(), sv, {".material"},
                                             "Material Files (*.material)\0*.material\0\0", "Select Material"))
                            comp[key] = sv;
                        if (!sv.empty()) {
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Select")) {
                                std::string resolved = sv;
                                if (!std::filesystem::exists(resolved) && !st.asset_dir.empty())
                                    resolved = (std::filesystem::path(st.asset_dir) / sv).string();
                                st.select_asset(resolved);
                            }
                        }
                        handled = true;
                    }
                    // Catch extension/path-style component fields that do not
                    // have a bespoke inspector yet. They get the same safe
                    // Project drag/browser slot rather than a raw path edit.
                    else if (key.find("path") != std::string::npos || key.find("file") != std::string::npos ||
                             key.find("asset") != std::string::npos || key.find("shader") != std::string::npos) {
                        if (_draw_asset_slot(st, lbl.c_str(), sv,
                                {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif", ".wav", ".ogg", ".mp3", ".flac",
                                 ".json", ".prefab", ".material", ".spv", ".cpp"},
                                "Project Asset Files\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif;*.wav;*.ogg;*.mp3;*.flac;*.json;*.prefab;*.material;*.spv;*.cpp\0\0",
                                "Select Project Asset"))
                            comp[key] = sv;
                        handled = true;
                    }

                    if (!handled) {
                        char buf[256]; snprintf(buf,sizeof(buf),"%s",sv.c_str());
                        ImGui::SetNextItemWidth(200);
                        if (ImGui::InputText(lbl.c_str(), buf, sizeof(buf)))
                            comp[key] = std::string(buf);
                    }
                }
                else if (val.is_array() && val.size()==4 &&
                         val[0].is_number() && val[1].is_number()) {
                    // Color array
                    float col[4] = {val[0].get<float>()/255.f, val[1].get<float>()/255.f,
                                    val[2].get<float>()/255.f, val[3].get<float>()/255.f};
                    if (ImGui::ColorEdit4(lbl.c_str(), col)) {
                        comp[key][0]=(int)(col[0]*255);
                        comp[key][1]=(int)(col[1]*255);
                        comp[key][2]=(int)(col[2]*255);
                        comp[key][3]=(int)(col[3]*255);
                        if (ctype=="Transform") transform_changed = true;
                    }
                }
                else {
                    ImGui::TextDisabled("%s: (complex)", lbl.c_str());
                }

                if (field_overridden) {
                    prefab_ui::field_context_menu(e, ctype, key, st);
                    ImGui::PopStyleColor();
                }
                ImGui::PopID();
            }
        }

        if (ctype == "Transform" && transform_changed) transform::mark_local_dirty(e.value("id",0));

        // ── GPU Instancing toggle (SpriteRenderer only) ───────────────────────
        // Rendered manually below the generic fields — same position as Unity's
        // "GPU Instancing" checkbox at the bottom of the Sprite Renderer inspector.
        // Hidden from the generic loop via should_hide_key("gpu_instancing").
        if (ctype == "SpriteRenderer") {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // ── Texture field with drag-drop ──────────────────────────────
            std::string tex = comp.value("texture", std::string());
            if (_draw_asset_slot(st, "Texture", tex, {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif"},
                                 "Image Files (*.png;*.jpg;*.jpeg;*.bmp;*.tga)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0\0", "Select Sprite Texture"))
                comp["texture"] = tex;

            // ── Color tint ───────────────────────────────────────────────
            float col[4] = {1,1,1,1};
            auto cv = comp.value("color", std::vector<float>{255,255,255,255});
            for (int i = 0; i < 4 && i < (int)cv.size(); ++i) col[i] = cv[i] / 255.f;
            if (ImGui::ColorEdit4("Color##sr", col, ImGuiColorEditFlags_AlphaBar))
                comp["color"] = nlohmann::json::array({(int)(col[0]*255),(int)(col[1]*255),(int)(col[2]*255),(int)(col[3]*255)});
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tint multiplied on top of the sprite texture.");

            // ── Flip X / Y buttons ────────────────────────────────────────
            bool fx = comp.value("flip_x", false);
            bool fy = comp.value("flip_y", false);
            if (fx) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f,0.49f,0.91f,1.f));
            else    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f,0.28f,0.28f,1.f));
            if (ImGui::Button("Flip X##sr")) { fx = !fx; comp["flip_x"] = fx; }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Flip the sprite horizontally.");
            ImGui::SameLine(0,4);
            if (fy) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f,0.49f,0.91f,1.f));
            else    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f,0.28f,0.28f,1.f));
            if (ImGui::Button("Flip Y##sr")) { fy = !fy; comp["flip_y"] = fy; }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Flip the sprite vertically.");

            // ── Opacity slider ────────────────────────────────────────────
            float opacity = comp.value("opacity", 1.f);
            ImGui::SetNextItemWidth(180.f);
            if (ImGui::SliderFloat("Opacity##sr", &opacity, 0.f, 1.f, "%.2f"))
                comp["opacity"] = opacity;

            // ── Pivot (normalized 0-1) displayed as drag ──────────────────
            float px = comp.value("pivot_x", 0.5f), py = comp.value("pivot_y", 0.5f);
            ImGui::SetNextItemWidth(80.f);
            if (ImGui::DragFloat("Pivot X##sr", &px, 0.01f, 0.f, 1.f, "%.2f")) comp["pivot_x"] = px;
            ImGui::SameLine(0,4);
            ImGui::SetNextItemWidth(80.f);
            if (ImGui::DragFloat("Pivot Y##sr", &py, 0.01f, 0.f, 1.f, "%.2f")) comp["pivot_y"] = py;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Normalized pivot point (0,0 = top-left, 0.5,0.5 = center, 1,1 = bottom-right).");

            // ── Sorting ───────────────────────────────────────────────────
            int order = comp.value("order_in_layer", 0);
            ImGui::SetNextItemWidth(80.f);
            if (ImGui::InputInt("Order In Layer##sr", &order)) comp["order_in_layer"] = order;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Higher = drawn on top within the same sorting layer.");

            // ── GPU Instancing ────────────────────────────────────────────
            bool gpu_inst = comp.value("gpu_instancing", false);
            if (ImGui::Checkbox("GPU Instancing##sr", &gpu_inst)) comp["gpu_instancing"] = gpu_inst;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Routes through instanced draw calls. Best for many identical sprites (coins, bullets, foliage).");

            ImGui::Spacing();
        }

        // ── Particle Emitter: curves, sub-emitters, atlas (task 12) ──────────
        // Curves are stored as flat [t,value,...] arrays on the component
        // (see component_defs.hpp); this draws an add/remove keyframe list
        // instead of a raw array dump, similar in spirit to Unity's
        // AnimationCurve mini-editor (without the draggable graph — keyframe
        // rows with numeric fields cover the same need with far less code).
        if (ctype == "ParticleEmitter") {
            ImGui::Spacing();
            ImGui::Separator();
            if (ImGui::CollapsingHeader("Size Over Lifetime", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextDisabled("Empty = use Size Start/End above");
                _draw_curve_editor(comp, "size_curve", 1, {"Size"});
            }
            if (ImGui::CollapsingHeader("Color Over Lifetime", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextDisabled("Empty = use Color Start/End above");
                _draw_curve_editor(comp, "color_curve", 4, {"R","G","B","A"});
            }
            if (ImGui::CollapsingHeader("Sub Emitters")) {
                bool on_death = comp.value("sub_emitter_on_death", false);
                bool on_spawn = comp.value("sub_emitter_on_spawn", false);
                if (ImGui::Checkbox("On Death##sub", &on_death)) comp["sub_emitter_on_death"] = on_death;
                ImGui::SameLine();
                if (ImGui::Checkbox("On Spawn##sub", &on_spawn)) comp["sub_emitter_on_spawn"] = on_spawn;
                if (on_death || on_spawn) {
                    int count = (int)comp.value("sub_emitter_count", 6.0);
                    if (ImGui::DragInt("Count##sub", &count, 1, 0, 200)) comp["sub_emitter_count"] = (double)count;
                    float speed = comp.value("sub_emitter_speed", 60.f);
                    if (ImGui::DragFloat("Speed##sub", &speed, 1.f, 0.f, 2000.f)) comp["sub_emitter_speed"] = speed;
                    float life = comp.value("sub_emitter_lifetime", 0.4f);
                    if (ImGui::DragFloat("Lifetime##sub", &life, 0.01f, 0.01f, 10.f)) comp["sub_emitter_lifetime"] = life;
                    float size = comp.value("sub_emitter_size", 3.f);
                    if (ImGui::DragFloat("Size##sub", &size, 0.1f, 0.1f, 100.f)) comp["sub_emitter_size"] = size;
                    float col[4] = {1,1,1,1};
                    auto cv = comp.value("sub_emitter_color", std::vector<float>{255,255,255,255});
                    for (int i=0;i<4 && i<(int)cv.size();++i) col[i] = cv[i]/255.f;
                    if (ImGui::ColorEdit4("Color##sub", col))
                        comp["sub_emitter_color"] = nlohmann::json::array({(int)(col[0]*255),(int)(col[1]*255),(int)(col[2]*255),(int)(col[3]*255)});
                }
            }
            if (ImGui::CollapsingHeader("Texture Sheet Animation")) {
                std::string atlas = comp.value("atlas_texture", std::string(""));
                if (_draw_asset_slot(st, "Atlas Texture", atlas, {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif"},
                                     "Image Files (*.png;*.jpg;*.jpeg;*.bmp;*.tga)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0\0", "Select Atlas Texture"))
                    comp["atlas_texture"] = atlas;
                if (!atlas.empty()) {
                    int cols = (int)comp.value("atlas_cols", 1.0);
                    int rows = (int)comp.value("atlas_rows", 1.0);
                    ImGui::SetNextItemWidth(80);
                    if (ImGui::InputInt("Cols##pe", &cols)) comp["atlas_cols"] = (double)std::max(1, cols);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(80);
                    if (ImGui::InputInt("Rows##pe", &rows)) comp["atlas_rows"] = (double)std::max(1, rows);
                    bool rand_frame = comp.value("atlas_random_frame", true);
                    if (ImGui::Checkbox("Random Frame Per Particle##pe", &rand_frame))
                        comp["atlas_random_frame"] = rand_frame;
                }
            }
            ImGui::Spacing();
        }

        ImGui::Unindent(8.f);
        ImGui::PopID();
        return false;
    }

    // ── Asset Inspector (Unity Project-window selection) ───────────────────
    // Shown instead of the entity inspector when an Assets-panel item is the
    // most recently selected thing. Every asset gets basic file info (name,
    // type, path, size); .material assets additionally get the full
    // editable Material Inspector — Unity's "select a Material asset, see
    // its shader/properties in the Inspector" behavior.
    void _draw_asset_inspector(EditorState& st) {
        namespace fs = std::filesystem;
        fs::path path = st.selected_asset_path;
        const auto read_asset_json = [](const fs::path& json_path) {
            nlohmann::json result;
            std::ifstream in(json_path, std::ios::binary);
            if (!in) return result;
            try { in >> result; } catch (...) { return nlohmann::json{}; }
            return result;
        };
        std::error_code ec;
        if (!fs::exists(path, ec)) {
            ImGui::TextDisabled("(asset no longer exists)");
            ImGui::TextDisabled("%s", path.string().c_str());
            return;
        }

        std::string fname = path.filename().string();
        std::string ext = path.extension().string();
        std::string lo_ext = ext; std::transform(lo_ext.begin(),lo_ext.end(),lo_ext.begin(),::tolower);

        ImGui::Text("%s", fname.c_str());
        ImGui::Separator();

        if (lo_ext == ".material") {
            _draw_material_inspector(st, path);
            return;
        }

        // Generic asset info panel (Unity shows an "Import Settings"-style
        // box for everything; this engine doesn't have a separate import
        // pipeline, so this is the lightweight equivalent: type + path +
        // size, with hints for image/script assets).
        std::string kind = asset_icons::classify(false, ext);
        ImGui::TextDisabled("Type: %s", kind.empty() ? "File" : kind.c_str());
        ImGui::TextWrapped("Path: %s", path.string().c_str());
        auto fsize = fs::file_size(path, ec);
        if (!ec) ImGui::TextDisabled("Size: %zu bytes", (size_t)fsize);

        if (kind == "image") {
            ImGui::Spacing();
            int width = 0, height = 0, channels = 0;
            if (stbi_info(path.string().c_str(), &width, &height, &channels)) {
                ImGui::SeparatorText("Texture Preview");
                ImGui::Text("%d x %d", width, height);
                ImGui::TextDisabled("%d source channel%s", channels, channels == 1 ? "" : "s");
            } else {
                ImGui::TextDisabled("Image metadata could not be decoded.");
            }

            const auto meta = read_asset_json(path.string() + ".meta");
            if (meta.is_object()) {
                static const char* filters[] = {"Point", "Bilinear", "Trilinear"};
                const int filter = std::max(0, std::min(2, meta.value("filter", 0)));
                ImGui::TextDisabled("PPU: %d   Filter: %s", meta.value("ppu", 100), filters[filter]);
            } else {
                ImGui::TextDisabled("PPU: 100   Filter: Point (default)");
            }

            if (ImGui::Button("Open Sprite Editor")) {
                st.requested_sprite_editor_asset = path.string();
                st.request_sprite_editor_open = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Slice, pivot, borders and sprite metadata");
            ImGui::Spacing();
            ImGui::TextDisabled("Drag this asset onto a SpriteRenderer or a");
            ImGui::TextDisabled("Material Texture field to assign it.");
        } else if (lo_ext == ".prefab") {
            ImGui::SeparatorText("Prefab Asset");
            const auto doc = read_asset_json(path);
            const bool valid = doc.is_object() && doc.contains("root") && doc["root"].is_object();
            ImGui::TextColored(valid ? ImVec4(0.48f,0.9f,0.55f,1.f) : ImVec4(1.f,0.42f,0.35f,1.f),
                               valid ? "Prefab is valid" : "Prefab file is invalid");
            if (valid) {
                const auto& root = doc["root"];
                ImGui::Text("Root: %s", root.value("name", path.stem().string()).c_str());
                const int legacy_children = doc.contains("children") && doc["children"].is_array()
                    ? (int)doc["children"].size() : 0;
                if (legacy_children > 0) ImGui::TextDisabled("%d child object%s", legacy_children, legacy_children == 1 ? "" : "s");
            }
            if (ImGui::Button("Open Prefab Stage")) {
                st.requested_prefab_stage_asset = path.string();
                st.request_prefab_stage_open = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Instantiate")) {
                st.undo.push_deep(st.entities);
                const int id = prefab::instantiate(path.string(), st, st.cam_x, st.cam_y);
                if (id >= 0) st.select(id);
                else st.log_error("Could not instantiate prefab: " + path.string());
            }
        } else if (kind == "script") {
            ImGui::Spacing();
            ImGui::TextDisabled("Attach via the Script component's");
            ImGui::TextDisabled("\"Add\" field, or Browse Scripts...");
        }
    }

    // Full Unity-style Material Inspector: shader dropdown + the properties
    // relevant to that shader (tint, texture override, cutoff, filter mode,
    // render queue offset). Every edit re-saves the .material file
    // immediately (Unity auto-saves material edits too), and the live
    // viewport picks the change up on its own next-frame mtime check.
    void _draw_material_inspector(EditorState& st, const std::filesystem::path& path) {
        if (_mat_cache_dir != path.parent_path().string()) {
            _mat_cache.set_base_dir(path.parent_path().string());
            _mat_cache_dir = path.parent_path().string();
        }
        auto mat_ptr = _mat_cache.get(path.string());
        material::MaterialAsset mat = mat_ptr ? *mat_ptr : material::default_material();
        bool changed = false;

        ImGui::PushID(path.string().c_str());

        ImGui::TextColored({0.6f,0.85f,1.0f,1.0f}, "Material");
        ImGui::Spacing();

        // A compact live swatch makes a material feel like an asset editor,
        // not a list of unconnected fields. It also exposes the linked
        // texture's decoded dimensions before the material is assigned to a
        // SpriteRenderer.
        if (ImGui::BeginChild("##mat_preview", {0, 96}, true)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float sw = 132.f, sh = 72.f;
            for (int y = 0; y < 6; ++y) for (int x = 0; x < 11; ++x) {
                const ImU32 c = ((x + y) & 1) ? IM_COL32(78,78,84,255) : IM_COL32(54,54,60,255);
                dl->AddRectFilled({p.x+x*12.f,p.y+y*12.f},{p.x+(x+1)*12.f,p.y+(y+1)*12.f},c);
            }
            dl->AddRectFilled({p.x+8.f,p.y+8.f},{p.x+sw-8.f,p.y+sh-8.f},
                              IM_COL32(mat.color[0],mat.color[1],mat.color[2],mat.color[3]), 4.f);
            dl->AddRect({p.x+8.f,p.y+8.f},{p.x+sw-8.f,p.y+sh-8.f}, IM_COL32(230,230,235,150), 4.f);
            ImGui::Dummy({sw + 8.f, sh});
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextDisabled("LIVE MATERIAL PREVIEW");
            ImGui::Text("Tint  #%02X%02X%02X", mat.color[0], mat.color[1], mat.color[2]);
            if (mat.texture.empty()) {
                ImGui::TextDisabled("Texture: SpriteRenderer source");
            } else {
                fs::path texture_path = mat.texture;
                if (!fs::exists(texture_path)) texture_path = path.parent_path() / mat.texture;
                if (!fs::exists(texture_path) && !st.asset_dir.empty()) texture_path = fs::path(st.asset_dir) / mat.texture;
                int w = 0, h = 0, channels = 0;
                if (stbi_info(texture_path.string().c_str(), &w, &h, &channels))
                    ImGui::Text("Texture: %s  (%d x %d)", texture_path.filename().string().c_str(), w, h);
                else
                    ImGui::TextColored({1.f,.58f,.30f,1.f}, "Texture missing or unreadable");
            }
            ImGui::EndGroup();
        }
        ImGui::EndChild();
        ImGui::Spacing();

        // Shader dropdown (Unity: the Shader popup at the top of every
        // Material Inspector). Each option below maps to a real difference
        // in render_system.hpp's _draw_sprite(): Unlit/Lit are visually
        // identical until a real lighting pass exists (documented on the
        // asset itself), Additive switches the SDL blend mode, Cutout uses
        // alpha_cutoff as a hard transparency threshold.
        static const std::pair<material::Shader,const char*> shaders[] = {
            {material::Shader::SpriteUnlit,    "Sprite-Unlit"},
            {material::Shader::SpriteLit,      "Sprite-Lit"},
            {material::Shader::SpriteAdditive, "Sprite-Additive"},
            {material::Shader::SpriteCutout,   "Sprite-Cutout"},
        };
        const char* cur_label = "Sprite-Unlit";
        for (auto& [sv, label] : shaders) if (sv == mat.shader) cur_label = label;
        ImGui::SetNextItemWidth(200);
        if (ImGui::BeginCombo("Shader", cur_label)) {
            for (auto& [sv, label] : shaders) {
                if (ImGui::Selectable(label, sv == mat.shader)) { mat.shader = sv; changed = true; }
            }
            ImGui::EndCombo();
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Tint / Color — multiplies with each SpriteRenderer's own color.
        float col[4] = { mat.color[0]/255.f, mat.color[1]/255.f, mat.color[2]/255.f, mat.color[3]/255.f };
        if (ImGui::ColorEdit4("Color", col)) {
            mat.color[0]=(Uint8)(col[0]*255); mat.color[1]=(Uint8)(col[1]*255);
            mat.color[2]=(Uint8)(col[2]*255); mat.color[3]=(Uint8)(col[3]*255);
            changed = true;
        }

        // Texture override — optional; empty means each sprite keeps using
        // its own SpriteRenderer.texture. Accepts drag-drop from Assets.
        {
            if (_draw_asset_slot(st, "Texture", mat.texture, {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif"},
                                 "Image Files (*.png;*.jpg;*.jpeg;*.bmp;*.tga)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0\0", "Select Material Texture"))
                changed = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Optional. Leave empty to use each sprite's own texture;\nset this to force every sprite using this material onto one texture.");
        }

        if (mat.shader == material::Shader::SpriteCutout) {
            float cutoff = mat.alpha_cutoff;
            ImGui::SetNextItemWidth(160);
            if (ImGui::SliderFloat("Alpha Cutoff", &cutoff, 0.f, 1.f)) { mat.alpha_cutoff = cutoff; changed = true; }
        }

        // Filter mode override (empty = "don't override", per-sprite wins)
        {
            static const std::pair<std::string,std::string> filter_opts[] = {
                {"", "(Use Sprite Setting)"}, {"point","Point (Pixel Art)"}, {"bilinear","Bilinear (Smooth)"}
            };
            std::string cur_disp = "(Use Sprite Setting)";
            for (auto& [v,label] : filter_opts) if (v == mat.filter_mode) cur_disp = label;
            ImGui::SetNextItemWidth(180);
            if (ImGui::BeginCombo("Filter Mode", cur_disp.c_str())) {
                for (auto& [v,label] : filter_opts)
                    if (ImGui::Selectable(label.c_str(), v == mat.filter_mode)) { mat.filter_mode = v; changed = true; }
                ImGui::EndCombo();
            }
        }

        int rq = mat.render_queue_offset;
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("Render Queue Offset", &rq)) { mat.render_queue_offset = rq; changed = true; }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Added to Order in Layer for every sprite using this material.\nMatches Unity's material render queue affecting draw order.");

        if (mat.shader == material::Shader::SpriteLit) {
            float ls = mat.light_strength;
            ImGui::SetNextItemWidth(160);
            if (ImGui::SliderFloat("Light Strength##mat", &ls, 0.f, 2.f)) { mat.light_strength = ls; changed = true; }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Multiplies accumulated light contribution.\n1.0 = physically correct; <1 = darker; >1 = overbright.");

            // Normal map asset picker
            ImGui::Spacing();
            if (_draw_asset_slot(st, "Normal Map", mat.normal_map, {".png", ".jpg", ".jpeg", ".bmp", ".tga"},
                                 "Image Files (*.png;*.jpg;*.jpeg;*.bmp;*.tga)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0\0", "Select Normal Map"))
                changed = true;
        }

        // ── Custom Shader (task 13) ───────────────────────────────────────────
        // When both SPV paths are filled, SpriteBatch's PipelineCache
        // compiles and caches a pipeline with those modules on first use,
        // replacing sprite.vert/frag for this material only.
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored({0.9f,0.75f,0.3f,1.0f}, "Custom Shader");
        ImGui::Spacing();

        {
            if (_draw_asset_slot(st, "Vert SPV", mat.custom_vert_spv, {".spv"},
                                 "SPIR-V Shader (*.spv)\0*.spv\0\0", "Select Vertex SPIR-V"))
                changed = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Pre-compiled SPIR-V vertex shader (.vert.spv).\n"
                                   "Must match the unlit pipeline vertex layout:\n"
                                   "  loc 0 = vec2 pos, loc 1 = vec2 uv, loc 2 = vec4 color\n"
                                   "Leave empty to use the built-in sprite.vert.");
            if (_draw_asset_slot(st, "Frag SPV", mat.custom_frag_spv, {".spv"},
                                 "SPIR-V Shader (*.spv)\0*.spv\0\0", "Select Fragment SPIR-V"))
                changed = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Pre-compiled SPIR-V fragment shader (.frag.spv).\n"
                                   "Must bind set 0 binding 0 as the albedo sampler2D\n"
                                   "and accept the same PushConstants as sprite.frag.\n"
                                   "Leave empty to use the built-in sprite.frag.");
            if (!mat.custom_vert_spv.empty() || !mat.custom_frag_spv.empty()) {
                bool both = !mat.custom_vert_spv.empty() && !mat.custom_frag_spv.empty();
                if (!both)
                    ImGui::TextColored({1.f,0.4f,0.2f,1.f}, "(!!) Both vert and frag SPV must be set");
                else
                    ImGui::TextColored({0.4f,1.f,0.4f,1.f}, "(ok) Custom shader active");
            }
        }

        if (changed) {
            mat.save_to_file(path.string());
            st.log("Updated material: " + path.filename().string());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("Assign via a Sprite Renderer's Material field,");
        ImGui::TextDisabled("or drag this asset onto one in the Inspector.");

        ImGui::PopID();
    }

public:
    // Registered by TilePalettePanel after Vulkan preview resources are ready.
    // The Inspector deliberately does not own a second palette cache.
    void set_tile_palette_picker(std::function<void(EditorState&, Entity&)> picker) {
        _tile_palette_picker = std::move(picker);
    }

    // ── Multi-object Inspector (Unity-style batch editing) ──────────────────
    // Shown when 2+ entities are selected. Lets common, safe-to-batch fields
    // (Active, Tag, uniform Transform nudges) apply to the whole selection at
    // once instead of forcing one-at-a-time editing — same gap Unity covers
    // by just instancing N inspectors and writing through all of them.
    // Public, project-rooted asset reference control for specialised editor
    // windows (Animator, Sprite tools, etc.). Keeping this one control as the
    // canonical implementation gives every caller the same extension
    // validation, relative-path conversion, drag/drop, Browse dialog, and
    // incompatible-asset warning as the Inspector.
    static bool draw_project_asset_slot(EditorState& st, const char* label, std::string& value,
                                        std::initializer_list<const char*> extensions,
                                        const char* browser_filter, const char* browser_title) {
        return _draw_asset_slot(st, label, value, extensions, browser_filter, browser_title);
    }

    void _draw_multi_select_inspector(EditorState& st) {
        int n = (int)st.selected_ids.size();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f,0.8f,1.0f,1.0f));
        ImGui::Text("%d GameObjects", n);
        ImGui::PopStyleColor();
        ImGui::TextDisabled("Multi-object editing");
        ImGui::Separator();

        // Active toggle — tri-state: if selection is mixed, checkbox shows
        // indeterminate-ish via a third "mixed" label rather than silently
        // picking one side.
        {
            int active_count = 0;
            for (int id : st.selected_ids) if (Entity* e = st.find_entity(id)) if (e->value("active", true)) ++active_count;
            bool all_active  = (active_count == n);
            bool none_active = (active_count == 0);
            bool checkbox_val = all_active;
            ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, !all_active && !none_active);
            if (ImGui::Checkbox("##active_multi", &checkbox_val)) {
                st.undo.push_deep(st.entities);
                for (int id : st.selected_ids) if (Entity* e = st.find_entity(id)) (*e)["active"] = checkbox_val;
            }
            ImGui::PopItemFlag();
            ImGui::SameLine(0,4);
            ImGui::Text("Active%s", (!all_active && !none_active) ? " (Mixed)" : "");
        }

        // Tag — applies the typed tag to every selected entity on Enter.
        {
            static char tag_buf[64] = {};
            ImGui::SetNextItemWidth(150);
            if (ImGui::InputTextWithHint("##tag_multi", "Set tag for all...", tag_buf, sizeof(tag_buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                st.undo.push_deep(st.entities);
                for (int id : st.selected_ids) if (Entity* e = st.find_entity(id)) (*e)["tag"] = std::string(tag_buf);
                st.log("Set tag \"" + std::string(tag_buf) + "\" on " + std::to_string(n) + " objects.");
                tag_buf[0] = '\0';
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Type a tag and press Enter to apply it to all selected objects.");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("Transform (Move All)");
        ImGui::TextDisabled("Offsets below shift every selected object\nby the same amount, preserving their\nrelative layout.");
        static float dx = 0.f, dy = 0.f;
        ImGui::SetNextItemWidth(90); ImGui::DragFloat("dX", &dx, 0.5f);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90); ImGui::DragFloat("dY", &dy, 0.5f);
        ImGui::SameLine();
        if (ImGui::Button("Apply")) {
            if (dx != 0.f || dy != 0.f) {
                st.undo.push_deep(st.entities);
                for (int id : st.selected_ids) {
                    Entity* e = st.find_entity(id);
                    if (!e || !has_component(*e, "Transform")) continue;
                    auto& tr = (*e)["components"]["Transform"];
                    tr["x"] = tr.value("x", 0.f) + dx;
                    tr["y"] = tr.value("y", 0.f) + dy;
                }
                transform::mark_structure_dirty();
                st.log("Moved " + std::to_string(n) + " objects by (" + std::to_string(dx) + ", " + std::to_string(dy) + ").");
                dx = dy = 0.f;
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("Tip: drag in the Viewport to move the whole\nselection together, or select one object at a\ntime for full component editing.");

        // List the selection so it's clear exactly who's affected.
        ImGui::Spacing();
        if (ImGui::TreeNode("Selected Objects")) {
            for (int id : st.selected_ids) {
                Entity* e = st.find_entity(id);
                ImGui::BulletText("%s  [%d]", e ? e->value("name","Entity").c_str() : "?", id);
            }
            ImGui::TreePop();
        }
    }

    void draw(EditorState& st) {
        ImGui::SetNextWindowSize({300,600}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Inspector##win")) { ImGui::End(); return; }

        // Unity behavior: the Inspector shows whichever was selected most
        // recently — a Hierarchy entity OR a Project/Assets-panel asset —
        // not always the entity. asset_selection_is_newer tracks that.
        Entity* ep = st.selected_entity();
        bool show_asset = st.asset_selection_is_newer && !st.selected_asset_path.empty();
        if (show_asset) {
            _draw_asset_inspector(st);
            ImGui::End();
            return;
        }

        if (!ep) {
            if (!st.selected_asset_path.empty()) {
                _draw_asset_inspector(st);
                ImGui::End();
                return;
            }
            ImGui::TextDisabled("No entity selected.");
            ImGui::End(); return;
        }

        // ── Multi-selection mode (Unity-style) ──────────────────────────────
        // When more than one entity is selected in the Hierarchy/Viewport,
        // show a compact batch-editing header instead of a single full
        // component inspector (which would only ever reflect st.selected_id
        // anyway, silently hiding the rest of the selection). Mirrors
        // Unity's multi-object Inspector: active toggle, tag, and a few
        // common batch ops apply to every selected entity at once.
        if (st.selected_ids.size() > 1) {
            _draw_multi_select_inspector(st);
            ImGui::End();
            return;
        }

        Entity& e = *ep;
        int eid = e.value("id",0);

        // Header: active checkbox + icon + name on same line
        {
            bool active = e.value("active",true);
            bool active_overridden = prefab::is_field_overridden(e, "__root__", "active");
            if (active_overridden) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.72f,0.86f,1.0f,1.f));
            if (ImGui::Checkbox("##active", &active)) e["active"]=active;
            if (active_overridden) {
                prefab_ui::field_context_menu(e, "__root__", "active", st);
                ImGui::PopStyleColor();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Active");
            ImGui::SameLine(0,4);

            ImVec2 icon_p = ImGui::GetCursorScreenPos();
            icon_p.y += (ImGui::GetFrameHeight() - 14.f) * 0.5f;
            hier_icons::draw(ImGui::GetWindowDrawList(), icon_p, hier_icons::entity_kind(e));
            ImGui::Dummy({16.f, 0});
            ImGui::SameLine(0.f, 3.f);

            std::string n = e.value("name","Entity");
            char buf[128]; snprintf(buf,sizeof(buf),"%s",n.c_str());
            ImGui::SetNextItemWidth(-1);
            bool name_overridden = prefab::is_field_overridden(e, "__root__", "name");
            if (name_overridden) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.72f,0.86f,1.0f,1.f));
            if (ImGui::InputText("##inspname", buf, sizeof(buf))) e["name"]=std::string(buf);
            if (name_overridden) {
                prefab_ui::field_context_menu(e, "__root__", "name", st);
                ImGui::PopStyleColor();
            }
        }

        // Tag + ID on second line
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f,0.45f,0.45f,1.f));
            ImGui::Text("ID: %d", eid);
            ImGui::PopStyleColor();
            ImGui::SameLine(0, 10);
            std::string tag = e.value("tag","");
            char buf[64]; snprintf(buf,sizeof(buf),"%s",tag.c_str());
            ImGui::SetNextItemWidth(120);
            bool tag_overridden = prefab::is_field_overridden(e, "__root__", "tag");
            if (tag_overridden) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.72f,0.86f,1.0f,1.f));
            if (ImGui::InputTextWithHint("##tag", "Tag...", buf, sizeof(buf))) e["tag"]=std::string(buf);
            if (tag_overridden) {
                prefab_ui::field_context_menu(e, "__root__", "tag", st);
                ImGui::PopStyleColor();
            }
        }

        // ── Prefab instance toolbar (Gap 3) ────────────────────────────────
        if (prefab_ui::draw_override_toolbar(e, st)) {
            // Entity was unpacked — nothing more to draw this frame
            ImGui::End();
            return;
        }

        ImGui::Separator();

        // Components are grouped by the same category catalog used by Add
        // Component and the Window menu.  A long flat JSON/alphabetical list
        // makes an Inspector impossible to scan once an entity has gameplay,
        // physics, rendering and UI together.
        if (e.contains("components") && e["components"].is_object()) {
            static const std::vector<std::string> category_order = {
                "Core", "Rendering", "Physics 2D", "Camera", "Animation",
                "2D World", "Navigation", "Audio", "UI", "Gameplay", "Advanced", "Other"
            };
            std::map<std::string, std::vector<std::string>> grouped;
            for (auto& [ctype, _] : e["components"].items())
                grouped[component_category(ctype)].push_back(ctype);
            for (auto& [_, entries] : grouped) {
                std::sort(entries.begin(), entries.end(), [](const std::string& a, const std::string& b) {
                    return component_display_name(a) < component_display_name(b);
                });
            }

            std::vector<std::string> to_remove;
            const auto draw_category = [&](const std::string& category) {
                const auto found = grouped.find(category);
                if (found == grouped.end()) return;
                // Component groups behave like the Window menu categories:
                // open only what is relevant instead of forcing a long flat
                // Inspector through every rendering/physics/UI component.
                const ImGuiTreeNodeFlags flags =
                    (category == "Core" || category == "Rendering" || category == "Physics 2D")
                        ? ImGuiTreeNodeFlags_DefaultOpen : 0;
                const std::string heading = category + " (" + std::to_string(found->second.size()) + ")";
                if (!ImGui::CollapsingHeader(heading.c_str(), flags)) return;
                for (const auto& ctype : found->second) {
                    if (_draw_component(st, e, ctype)) to_remove.push_back(ctype);
                    ImGui::Spacing();
                }
            };
            for (const auto& category : category_order) draw_category(category);
            for (const auto& [category, _] : grouped) {
                if (std::find(category_order.begin(), category_order.end(), category) == category_order.end())
                    draw_category(category);
            }
            for (auto& r : to_remove) {
                e["components"].erase(r);
                st.log("Removed component: " + r);
            }
        }

        ImGui::Separator();

        // Add Component button
        if (ImGui::Button("Add Component", {-1,0}))
            _show_add_popup = true;

        if (_show_add_popup) {
            ImGui::OpenPopup("##addcomp");
            _show_add_popup = false;
        }

        ImGui::SetNextWindowSize({500.f, 560.f}, ImGuiCond_FirstUseEver);
        if (ImGui::BeginPopup("##addcomp")) {
            ImGui::TextUnformatted("Add Component");
            ImGui::SameLine();
            ImGui::TextDisabled("Grouped by system");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##filter", "Search name, category, or capability...", _add_comp_filter, sizeof(_add_comp_filter));
            std::string flt = _add_comp_filter;
            std::transform(flt.begin(),flt.end(),flt.begin(),::tolower);
            static bool show_experimental_components = false;
            ImGui::Checkbox("Show experimental components", &show_experimental_components);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Production components have a verified editor, runtime and export path. Experimental components are shown only for testing.");
            ImGui::Separator();

            std::map<std::string, std::vector<std::string>> candidates;
            for (const auto& cname : component_names_for_picker(show_experimental_components)) {
                if (e["components"].contains(cname)) continue;
                std::string searchable = component_display_name(cname) + " " + cname + " " +
                                         component_category(cname) + " " + component_description(cname);
                std::transform(searchable.begin(),searchable.end(),searchable.begin(),::tolower);
                if (!flt.empty() && searchable.find(flt)==std::string::npos) continue;
                candidates[component_category(cname)].push_back(cname);
            }
            if (candidates.empty()) ImGui::TextDisabled("No matching components.");
            ImGui::BeginChild("##add_component_categories", {0, 0}, true);
            static const std::vector<std::string> category_order = {
                "Core", "Rendering", "Physics 2D", "Camera", "Animation", "2D World",
                "Navigation", "Audio", "UI", "Gameplay", "Advanced", "Other"
            };
            const auto draw_category = [&](const std::string& category) {
                const auto found = candidates.find(category);
                if (found == candidates.end()) return;
                const std::string heading = category + " (" + std::to_string(found->second.size()) + ")";
                const ImGuiTreeNodeFlags flags = flt.empty() ? 0 : ImGuiTreeNodeFlags_DefaultOpen;
                if (!ImGui::CollapsingHeader(heading.c_str(), flags)) return;
                for (const auto& cname : found->second) {
                    const ComponentDescriptor* descriptor = component_descriptor(cname);
                    const std::string label = component_display_name(cname) + "##add_" + cname;
                    if (ImGui::Selectable(label.c_str())) {
                        const std::vector<std::string> added = add_component_with_requirements(e["components"], cname);
                        std::string summary = "Added component: " + component_display_name(cname);
                        if (added.size() > 1) {
                            summary += " (also added ";
                            bool first = true;
                            for (const auto& added_name : added) {
                                if (added_name == cname) continue;
                                if (!first) summary += ", ";
                                summary += component_display_name(added_name); first = false;
                            }
                            summary += ")";
                        }
                        st.log(summary);
                        ImGui::CloseCurrentPopup();
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted(component_description(cname).c_str());
                        if (descriptor) {
                            ImGui::TextDisabled("Status: %s", component_support_label(descriptor->support));
                            if (!descriptor->required_components.empty()) ImGui::TextDisabled("Adds required components automatically.");
                            if (!descriptor->tool_hint.empty()) ImGui::TextDisabled("Related tool: %s", descriptor->tool_hint.c_str());
                        }
                        ImGui::EndTooltip();
                    }
                }
            };
            for (const auto& category : category_order) draw_category(category);
            for (const auto& [category, _] : candidates) {
                if (std::find(category_order.begin(), category_order.end(), category) == category_order.end()) draw_category(category);
            }
            ImGui::EndChild();
            ImGui::EndPopup();
        }

        // Keep prefab overrides in sync with whatever the inspector just edited.
        // This recomputes the instance's override delta after all component
        // widgets have run, so changes made through any editor control are
        // reflected immediately without having to touch every field widget
        // individually.
        if (prefab::is_instance(e)) {
            std::string src = prefab::resolve_prefab_path(e["prefab_source"].get<std::string>(), st.asset_dir);
            Entity tpl = prefab::load(src);
            if (!tpl.is_null()) e["prefab_overrides"] = prefab::compute_overrides(e, tpl);
        }

        ImGui::End();
    }
};

// ─── Asset thumbnail cache ─────────────────────────────────────────────────────
// Loads the actual pixels of image assets (png/jpg/bmp/tga/gif) into small SDL
// textures so the Assets panel can show a real preview of each image, the way
// Unity's Project window does, instead of a generic placeholder icon.
//
// Keyed by absolute file path. Each entry also remembers the file's last
// write time so replacing an asset on disk (re-importing the same filename)
// is picked up automatically instead of showing a stale thumbnail forever.
namespace thumbnail_cache {

struct Entry {
    vkr::Texture tex;                    // GPU image + sampler
    VkDescriptorSet imgui_ds = VK_NULL_HANDLE; // what ImGui::Image() actually wants
    int w = 0, h = 0;
    std::filesystem::file_time_type mtime{};
    bool failed = false; // true = tried once and the file isn't a loadable image
};

inline bool is_image_ext(const std::string& ext_lo) {
    return ext_lo==".png"||ext_lo==".jpg"||ext_lo==".jpeg"||ext_lo==".bmp"
        ||ext_lo==".tga"||ext_lo==".gif";
}

class Cache {
public:
    void init(vkr::RendererBackend& backend) {
        _backend = &backend;
        _uploader = std::make_unique<vkr::TextureUploader>(backend.ctx());
    }

    void clear() {
        for (auto& [k, e] : _entries) _destroy_entry(e);
        _entries.clear();
    }

    ~Cache() { clear(); }

    // Returns the cached entry for `path`, loading (or reloading, if the
    // file changed on disk) as needed. Returns nullptr if the path can't be
    // read as an image — callers should fall back to the vector icon.
    const Entry* get(const std::string& path) {
        if (!_backend) return nullptr;

        std::error_code ec;
        auto mtime = fs::last_write_time(path, ec);
        if (ec) return nullptr;

        auto it = _entries.find(path);
        if (it != _entries.end()) {
            if (it->second.mtime == mtime) {
                return it->second.failed ? nullptr : &it->second;
            }
            // File changed on disk — drop the stale texture and reload below.
            _destroy_entry(it->second);
            _entries.erase(it);
        }

        Entry e;
        e.mtime = mtime;
        // stb_image.h (see engine_cpp/third_party/stb_image.h) — same
        // decoder texture_system_vk.hpp uses, replacing the old
        // TEXSYS_LOAD_SURFACE/SDL_Surface round-trip for thumbnails too.
        int w = 0, h = 0, src_channels = 0;
        stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &src_channels, 4);
        if (!pixels) {
            e.failed = true;
            return &(_entries[path] = e), nullptr;
        }

        e.tex = _uploader->upload(static_cast<const uint8_t*>(pixels),
                                   (uint32_t)w, (uint32_t)h,
                                   vkr::FilterMode::Bilinear);
        e.w = w;
        e.h = h;
        stbi_image_free(pixels);
        if (!e.tex.valid()) { e.failed = true; _entries[path] = e; return nullptr; }

        e.imgui_ds = ImGui_ImplVulkan_AddTexture(e.tex.sampler, e.tex.image.view,
                                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        auto& stored = (_entries[path] = e);
        return &stored;
    }

private:
    vkr::RendererBackend* _backend = nullptr;
    std::unique_ptr<vkr::TextureUploader> _uploader;
    std::unordered_map<std::string, Entry> _entries;

    void _destroy_entry(Entry& e) {
        if (e.imgui_ds) { ImGui_ImplVulkan_RemoveTexture(e.imgui_ds); e.imgui_ds = VK_NULL_HANDLE; }
        if (_uploader && e.tex.valid()) _uploader->destroy(e.tex);
    }
};

} // namespace thumbnail_cache

// ─── AssetsPanel ──────────────────────────────────────────────────────────────
class AssetsPanel {
    char _search[128] = {};
    char _rename_buf[256] = {};
    std::string _current_dir;
    std::string _asset_dir;
    std::string _script_dir;
    std::string _selected;
    std::string _rename_target;
    bool _rename_pending = false;
    bool _rename_just_opened = false;
    thumbnail_cache::Cache _thumbs;

    static bool _is_script_source(const fs::path& path) {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return ext == ".cpp" || ext == ".cxx" || ext == ".cc" || ext == ".h" ||
               ext == ".hpp" || ext == ".hh" || ext == ".hxx" || ext == ".inl";
    }

    static bool _is_shader_graph_asset(const fs::path& path) {
        std::string name = path.filename().string();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        constexpr const char* suffix = ".shadergraph.json";
        const size_t suffix_size = std::char_traits<char>::length(suffix);
        return name.size() >= suffix_size && name.compare(name.size() - suffix_size, suffix_size, suffix) == 0;
    }

#if defined(_WIN32)
    static fs::path _find_vscode_executable() {
        return gameengine::toolchain::vscode();
    }
#endif

    static void _open_script_in_vscode(const fs::path& script, EditorState& st) {
#if defined(_WIN32)
        // The Assets panel intentionally stores project-relative paths so it
        // can move between projects.  Passing one of those paths to Code *and*
        // also using its parent as Code's working directory makes Code resolve
        // it twice (for example games/abyss-of-hollows/scripts/games/abyss-of-hollows/scripts/foo.cpp).
        // Always hand external programs one normalized absolute filename.
        std::error_code path_error;
        // `directory_entry::path()` is project-relative in the Assets panel
        // (for example games/abyss-of-hollows/scripts/Player.cpp).  Resolve that path
        // against the engine root explicitly rather than against whatever
        // directory happened to launch editor.exe.  The latter was what let
        // Code receive games/abyss-of-hollows/... twice and open a nonexistent empty tab.
        const fs::path requested = script.lexically_normal();
        const fs::path root = find_engine_project_root();
        fs::path absolute_script;

        // Prefer a genuinely absolute input, then try the normal engine-root
        // form.  The latter covers the Assets panel's usual
        // `games/<project>/scripts/Foo.cpp` payload.
        std::vector<fs::path> candidates;
        if (requested.is_absolute()) candidates.push_back(requested);
        else candidates.push_back((root / requested).lexically_normal());

        // A few older layouts stored a script-directory prefix and then
        // appended an already project-relative payload.  That produced paths
        // such as `games/abyss-of-hollows/scripts/games/abyss-of-hollows/scripts/Foo.cpp`.  Do not
        // hand that malformed filename to VS Code.  Instead, examine every
        // `games/` boundary and accept the first suffix that resolves to a
        // real file under the engine root.  This is deliberately restricted
        // to the engine's games tree; arbitrary outside paths are never
        // rewritten or silently accepted here.
        {
            // Do this for both relative and absolute inputs.  A malformed
            // stored *absolute* filename can contain the same duplicated
            // `games/<project>` segment and is repaired by the exact same
            // trusted-root rule.
            std::vector<fs::path> parts;
            for (const auto& part : requested) parts.push_back(part);
            for (size_t i = 0; i < parts.size(); ++i) {
                if (parts[i].generic_string() != "games") continue;
                fs::path suffix;
                for (size_t j = i; j < parts.size(); ++j) suffix /= parts[j];
                const fs::path candidate = (root / suffix).lexically_normal();
                if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
                    candidates.push_back(candidate);
            }
        }

        for (const auto& candidate : candidates) {
            path_error.clear();
            if (fs::is_regular_file(candidate, path_error)) {
                absolute_script = candidate;
                break;
            }
        }
        if (absolute_script.empty()) {
            // Retain a safe fallback for callers that legitimately pass a
            // working-directory-relative external file rather than a project
            // asset.  It never overrides a valid engine-rooted script.
            path_error.clear();
            absolute_script = fs::absolute(requested, path_error).lexically_normal();
        }
        if (path_error || !fs::is_regular_file(absolute_script, path_error)) {
            st.log_error("Cannot open script: " + script.string());
            return;
        }
        const fs::path code = _find_vscode_executable();
        if (code.empty()) {
            constexpr const char* missing = "Visual Studio Code not installed, open your code editor manually.";
            st.log_warn(missing);
            MessageBoxW(nullptr, L"Visual Studio Code not installed, open your code editor manually.",
                        L"GameEngine2D Pro", MB_OK | MB_ICONWARNING);
            return;
        }
        const std::wstring arguments = L"\"" + absolute_script.wstring() + L"\"";
        const HINSTANCE result = ShellExecuteW(nullptr, L"open", code.c_str(), arguments.c_str(),
                                                absolute_script.parent_path().c_str(), SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32) {
            st.log_error("Visual Studio Code could not open: " + absolute_script.string());
            return;
        }
        st.log("Opened script in Visual Studio Code: " + absolute_script.filename().string());
#else
        (void)script;
        st.log_warn("Visual Studio Code not installed, open your code editor manually.");
#endif
    }

public:
    // Called once at startup with the live Vulkan backend so image assets can
    // be loaded into real GPU textures for previews.
    void init(vkr::RendererBackend& backend, EditorState& st) {
        _thumbs.init(backend);
        init(st);
    }

    void init(EditorState& st) {
        _asset_dir  = st.asset_dir;
        _script_dir = (fs::path(st.scene_path).parent_path() / "scripts").string();
        _current_dir= _asset_dir;
    }

    void draw(EditorState& st) {
        // Unity-style auto-registration, the proactive half: don't wait for
        // the user to press Play or click "Rebuild Scripts" — once a
        // new/changed script under a project's scripts/ folder has been
        // written and then left alone for AUTO_REBUILD_DEBOUNCE_SECONDS
        // (so we're not reacting mid-save to a half-written file), kick
        // THAT PROJECT's rebuild off on our own. This covers scripts
        // created from the editor's "New Script" AND ones dropped in
        // manually from the OS file browser — both just sit on disk under
        // scripts/, which is all this check looks at; it has no idea (and
        // doesn't need to know) which path created the file.
        //
        // Every project with a scripts/ folder is checked independently.
        // Play-mode still pauses ALL auto-rebuilds, not just the active
        // project's, since the editor doesn't support hot-reloading
        // scripts out from under a running scene no matter which project
        // that scene belongs to.
        //
        // SIMPLE ONE-SHOT LATCH: scripts_changed_since_build()'s continuous
        // per-frame re-evaluation (known_paths/pending_change/debounce
        // bookkeeping) was firing repeatedly during a single long-running
        // build no matter how the timing was patched — the underlying
        // design re-derives "is anything new" 60+ times a second from live
        // filesystem state and only fully settles once a build finishes,
        // which is inherently racy against a build that itself takes tens
        // of seconds. Replacing it with a dead-simple latch: try the
        // baseline check ONCE per project per process, and if it finds
        // anything unregistered, kick off ONE auto-rebuild and never
        // auto-trigger for this project again this session. Any rebuild
        // after that is explicit (toolbar button / Play), which already
        // has its own straightforward trigger elsewhere and isn't part of
        // this loop.
        // Do not compile every project at startup. The active project's
        // AutoHotReload watcher handles individual changed/new script files.
        // A packaged project does not contain a developer-machine build tree.
        // Reconcile the active project's native scripts once when the editor
        // opens.  The background CMake target compiles only missing/changed
        // sources; the editor modal keeps the scene read-only until the
        // module registry is safe to use.
        if (_initial_modules_loaded() && !st.playing) {
            const std::string project = project_name_from_scene_path(st.scene_path);
            if (!project.empty()) {
                auto& latch = _auto_rebuild_latch(project);
                auto& rebuild = script_rebuild_state(project);
                if (!latch.attempted && !rebuild.in_progress.load()) {
                    latch.attempted = true;
                    st.log_warn("[" + project + "] preparing native scripts for this editor session...");
                    rebuild_scripts_module(project);
                }
            }
        }

        // Do not rebuild every inactive project at startup.
        if (false) {
            for (auto& project : _all_script_project_names()) {
                auto& latch = _auto_rebuild_latch(project);
                if (latch.attempted) continue; // already handled (or handling) this project this session — never re-arms
                if (script_rebuild_state(project).in_progress.load()) continue; // a build is already running (e.g. from a previous frame's claim); wait for it, don't re-latch yet
                if (!_script_project_needs_initial_rebuild(project)) {
                    latch.attempted = true; // nothing unregistered — never check this project again
                    continue;
                }
                latch.attempted = true; // claim BEFORE starting; this can only ever happen once
                st.log_warn("[" + project + "] script(s) not yet registered — rebuilding once...");
                rebuild_scripts_module(project);
            }
        }

        // Runtime file-system watch: if a new script is dropped into a
        // project's scripts/ folder while the editor is still open,
        // treat it the same as an in-editor create and rebuild once the
        // debounce window has elapsed.
        //
        // NOTE: Skipped when AutoHotReload is active — that system
        // handles per-script rebuilds (one DLL at a time) much faster
        // than the full meta-target rebuild below. AutoHotReload is
        // started by editor_main.cpp when it detects a scripts/ folder
        // for the active scene's project.
        // A project-wide rebuild is only an explicit active-project action
        // (Play/Rebuild Scripts), never an Assets-panel background task.
        if (false) {
            for (auto& project : _all_script_project_names()) {
                auto& rb = script_rebuild_state(project);
                if (rb.in_progress.load()) continue;
                scripts_changed_since_build(project);
                if (!scripts_ready_for_auto_rebuild(project)) continue;
                st.log_warn("[" + project + "] new script(s) detected on disk — rebuilding before continuing...");
                script_staleness_tracker(project).clear_pending();
                rebuild_scripts_module(project);
            }
        }

        // Sync directories if scene changed
        std::string new_asset = st.asset_dir;
        if (new_asset != _asset_dir) {
            _asset_dir  = new_asset;
            _script_dir = (fs::path(st.scene_path).parent_path() / "scripts").string();
            _current_dir= _asset_dir;
        }

        ImGui::SetNextWindowSize({600,120}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Assets##win")) { ImGui::End(); return; }

        // Toolbar
        if (ImGui::Button("Up")) {
            fs::path p = _current_dir;
            auto parent = p.parent_path().string();
            if (parent.rfind(_asset_dir,0)==0 || parent.rfind(_script_dir,0)==0)
                _current_dir = parent;
        }
        ImGui::SameLine();
        if (ImGui::Button("Root")) _current_dir=_asset_dir;
        ImGui::SameLine();
        if (ImGui::Button("Scripts")) _current_dir=_script_dir;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180);
        ImGui::InputText("Search", _search, sizeof(_search));

        ImGui::TextDisabled("Project / %s", _display_project_path().c_str());
        ImGui::Separator();

        // The Project browser deliberately exposes both persistent roots.  A
        // script created by the editor has always gone into scripts/, but the
        // old single-grid view made that folder effectively invisible.
        ImGui::BeginChild("##assets_folders", {190,0}, true);
        ImGui::TextDisabled("PROJECT");
        ImGui::Separator();
        _draw_folder_tree("Assets", fs::path(_asset_dir));
        _draw_folder_tree("Scripts", fs::path(_script_dir));
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##assets_content", {0,0}, false);

        std::string flt = _search;
        std::transform(flt.begin(),flt.end(),flt.begin(),::tolower);

        if (!fs::exists(_current_dir)) {
            ImGui::TextDisabled("(directory not found)");
            ImGui::EndChild(); ImGui::End(); return;
        }

        // Collect entries
        std::vector<fs::directory_entry> entries;
        try {
            for (auto& de : fs::directory_iterator(_current_dir))
                entries.push_back(de);
        } catch(...) {}
        std::sort(entries.begin(),entries.end(),[](auto& a,auto& b){
            if (a.is_directory()!=b.is_directory()) return a.is_directory();
            return a.path().filename()<b.path().filename();
        });

        // Unity-style icon grid: each entry is a fixed-size cell with a
        // vector icon on top and a centered, truncated filename below.
        const float cell_w = 84.0f, cell_h = 96.0f, icon_sz = 48.0f;
        const float avail_w = ImGui::GetContentRegionAvail().x;
        const int columns = std::max(1, (int)(avail_w / cell_w));
        ImU32 win_bg = ImGui::GetColorU32(ImGuiCol_WindowBg);
        ImU32 sel_col = IM_COL32(88,155,255,90);
        ImU32 hov_col = IM_COL32(255,255,255,18);

        int idx = 0;
        for (auto& de : entries) {
            std::string fname = de.path().filename().string();
            std::string flo = fname; std::transform(flo.begin(),flo.end(),flo.begin(),::tolower);
            if (!flt.empty() && flo.find(flt)==std::string::npos) continue;

            bool is_dir = de.is_directory();
            std::string ext = de.path().extension().string();
            std::string kind = asset_icons::classify(is_dir, ext);

            if ((idx % columns) != 0) ImGui::SameLine();
            idx++;

            ImGui::PushID(fname.c_str());
            ImGui::BeginGroup();
            ImVec2 cell_min = ImGui::GetCursorScreenPos();
            ImVec2 cell_max{cell_min.x + cell_w - 6.0f, cell_min.y + cell_h - 6.0f};
            ImDrawList* dl = ImGui::GetWindowDrawList();

            ImGui::InvisibleButton("##cell", {cell_w - 6.0f, cell_h - 6.0f});
            bool hovered = ImGui::IsItemHovered();
            bool dclicked = hovered && ImGui::IsMouseDoubleClicked(0);
            if (ImGui::IsItemClicked()) {
                _selected = fname;
                st.select_asset(de.path().string());
            }

            if (_selected == fname) dl->AddRectFilled(cell_min, cell_max, sel_col, 4.0f);
            else if (hovered) dl->AddRectFilled(cell_min, cell_max, hov_col, 4.0f);

            ImVec2 icon_pos{cell_min.x + (cell_w - 6.0f - icon_sz)*0.5f, cell_min.y + 6.0f};

            // For image files: try to show the actual pixel content as a
            // thumbnail. Fall back to the vector icon if the texture isn't
            // ready yet (first load) or the file can't be decoded.
            bool drew_thumb = false;
            if (kind == "image") {
                const auto* entry = _thumbs.get(de.path().string());
                if (entry && entry->imgui_ds) {
                    // Fit the image into the icon_sz square, preserving aspect ratio.
                    float tw = (float)entry->w, th = (float)entry->h;
                    float scale = icon_sz / std::max(tw, th);
                    float dw = tw * scale, dh = th * scale;
                    ImVec2 img_min{icon_pos.x + (icon_sz - dw) * 0.5f,
                                   icon_pos.y + (icon_sz - dh) * 0.5f};
                    ImVec2 img_max{img_min.x + dw, img_min.y + dh};
                    // Thin border around thumbnail
                    dl->AddRectFilled({img_min.x-1, img_min.y-1}, {img_max.x+1, img_max.y+1},
                                      IM_COL32(80,80,80,200), 2.f);
                    dl->AddImage((ImTextureID)(intptr_t)entry->imgui_ds, img_min, img_max);
                    drew_thumb = true;
                }
            }
            if (!drew_thumb)
                asset_icons::draw(dl, icon_pos, {icon_sz, icon_sz}, kind, win_bg);

            // Truncated, centered label under the icon.
            std::string label = fname;
            float max_text_w = cell_w - 12.0f;
            ImVec2 ts = ImGui::CalcTextSize(label.c_str());
            while (ts.x > max_text_w && label.size() > 1) {
                label.pop_back();
                ts = ImGui::CalcTextSize((label+"...").c_str());
            }
            if (label != fname) label += "...";
            ImVec2 text_pos{cell_min.x + (cell_w - 6.0f - ts.x)*0.5f, icon_pos.y + icon_sz + 6.0f};
            dl->PushClipRect(cell_min, cell_max, true);
            dl->AddText(text_pos, IM_COL32(225,225,225,255), label.c_str());
            dl->PopClipRect();

            if (hovered) ImGui::SetTooltip(is_dir ? "Double-click to open, single-click to select"
                                                   : "Double-click to open/edit, single-click to select");
            if (dclicked) {
                if (is_dir) {
                    _current_dir = de.path().string();
                } else if (kind == "image") {
                    st.requested_sprite_editor_asset = de.path().string();
                    st.request_sprite_editor_open = true;
                } else if (kind == "prefab") {
                    st.requested_prefab_stage_asset = de.path().string();
                    st.request_prefab_stage_open = true;
                } else if (_is_script_source(de.path())) {
                    _open_script_in_vscode(de.path(), st);
                } else if (_is_shader_graph_asset(de.path())) {
                    st.requested_shader_graph_asset = de.path().string();
                    st.request_shader_graph_open = true;
                } else {
                    // Double-click a file: start inline rename
                    _rename_target = de.path().string();
                    _rename_just_opened = true;
                    snprintf(_rename_buf, sizeof(_rename_buf), "%s", fname.c_str());
                }
            }
            // Inline rename overlay: draw an InputText in place of the label
            if (_rename_target == de.path().string()) {
                ImGui::SetCursorScreenPos({cell_min.x, icon_pos.y + icon_sz + 4.f});
                ImGui::SetNextItemWidth(cell_w - 6.f);
                bool enter = ImGui::InputText("##assetrename", _rename_buf, sizeof(_rename_buf),
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                bool just_opened = _rename_just_opened;
                if (just_opened) { ImGui::SetKeyboardFocusHere(-1); _rename_just_opened = false; }
                bool _asset_cancel = !ImGui::IsItemActive() && !just_opened && !enter;
                if (_asset_cancel) { _rename_target.clear(); }
                else if (enter || ImGui::IsItemDeactivatedAfterEdit()) {
                    if (_rename_buf[0] != '\0' && std::string(_rename_buf) != fname) {
                        std::error_code ec;
                        fs::path oldp = de.path();
                        fs::path newp = oldp.parent_path() / _rename_buf;
                        if (!fs::exists(newp, ec)) {
                            fs::rename(oldp, newp, ec);
                            if (!ec) {
                                if (_selected == fname) _selected = std::string(_rename_buf);
                                st.log("Renamed: " + fname + " -> " + std::string(_rename_buf));
                            } else {
                                st.log_error("Rename failed: " + ec.message());
                            }
                        } else {
                            st.log_error("Rename failed: \"" + std::string(_rename_buf) + "\" already exists.");
                        }
                    }
                    _rename_target.clear();
                }
            }

            // Keep the Entity Inspector selected while this asset is being
            // assigned.  A drag is an edit operation, not a request to switch
            // the inspector into asset-preview mode.
            if (!is_dir && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                std::error_code relative_ec;
                const fs::path relative_to_assets = fs::relative(de.path(), fs::path(st.asset_dir), relative_ec);
                const std::string payload_path = (!relative_ec && !relative_to_assets.empty() &&
                    *relative_to_assets.begin() != fs::path(".."))
                    ? relative_to_assets.generic_string() : de.path().string();
                st.asset_selection_is_newer = false;
                ImGui::SetDragDropPayload("ASSET_PATH", payload_path.c_str(), payload_path.size()+1);
                asset_icons::draw(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), {24,24}, kind, win_bg);
                ImGui::Dummy({24,24});
                ImGui::SameLine();
                ImGui::Text("%s", fname.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginPopupContextItem("##assetctx")) {
                // Inline properties (Unity style)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));
                ImGui::TextUnformatted(fname.c_str());
                ImGui::PopStyleColor();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
                ImGui::Text("Type: %s", is_dir ? "Folder" : ext.c_str());
                ImGui::Text("Path: %s", de.path().string().c_str());
                ImGui::PopStyleColor();
                ImGui::Separator();
                if (ImGui::MenuItem("Rename")) _open_rename_dialog(de.path());
                if (ImGui::MenuItem("Delete")) {
                    try { fs::remove(de.path()); } catch(...) {}
                    st.log("Deleted asset: "+fname);
                    if (_selected == fname) {
                        _selected.clear();
                        if (st.selected_asset_path == de.path().string()) st.clear_asset_selection();
                    }
                }
                // ── Prefab-specific actions (Gap 3) ─────────────────────
                if (!is_dir && ext == ".prefab")
                    prefab_ui::draw_asset_prefab_menu(de.path().string(), st);
                ImGui::EndPopup();
            }
            _draw_rename_dialog(st);
            ImGui::EndGroup();
            ImGui::PopID();
        }

        // Empty-area right-click: only fires when NOT hovering over an item
        if (ImGui::BeginPopupContextWindow("##assetsctx", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
            ImGui::TextDisabled("Create");
            ImGui::Separator();
            if (ImGui::MenuItem("New Folder"))  _create_folder(st);
            if (ImGui::MenuItem("New Script"))  _create_script(st);
            if (ImGui::MenuItem("New Texture")) _create_texture(st);
            if (ImGui::MenuItem("New Material")) _create_material(st);
            if (ImGui::MenuItem("New Prefab"))   prefab_ui::create_empty_prefab(_current_dir, st);
            ImGui::Separator();
            if (ImGui::MenuItem("Refresh")) { /* directory is re-scanned every frame already */ }
            ImGui::EndPopup();
        }

        ImGui::EndChild();
        ImGui::End();
    }


    void _open_rename_dialog(const fs::path& path) {
        snprintf(_rename_buf, sizeof(_rename_buf), "%s", path.filename().string().c_str());
        // Deferred open — see HierarchyPanel::_open_rename_dialog comment.
        // Called from inside BeginPopupContextItem, which closes itself this
        // same frame, so OpenPopup must be issued outside that popup's scope.
        // The grid view's inline rename box checks _rename_target first each
        // frame (same as Hierarchy), so without _rename_just_opened it reads
        // !IsItemActive() as true on the opening frame (focus hasn't been
        // applied by ImGui yet) and the box cancels itself instantly.
        _rename_target = path.string();
        _rename_pending = true;
        _rename_just_opened = true;
    }

    void _draw_rename_dialog(EditorState& st) {
        if (_rename_pending) {
            ImGui::OpenPopup("##asset_rename");
            _rename_pending = false;
        }
        if (_rename_target.empty()) return;
        if (ImGui::BeginPopupModal("##asset_rename", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Rename Asset");
            ImGui::SetNextItemWidth(280.f);
            ImGui::InputText("##asset_newname", _rename_buf, sizeof(_rename_buf));
            if (ImGui::Button("Rename") && _rename_buf[0] != '\0') {
                std::error_code ec;
                fs::path oldp = _rename_target;
                fs::path newp = oldp.parent_path() / _rename_buf;
                if (!fs::exists(newp, ec)) {
                    fs::rename(oldp, newp, ec);
                    if (!ec) {
                        if (_selected == oldp.filename().string()) _selected = newp.filename().string();
                        st.log("Renamed asset: " + oldp.filename().string() + " -> " + newp.filename().string());
                    } else {
                        st.log_error("Rename failed: " + ec.message());
                    }
                } else {
                    st.log_error("Rename failed: destination already exists.");
                }
                _rename_target.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                _rename_target.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

private:
    std::string _display_project_path() const {
        std::error_code ec;
        const fs::path current = fs::weakly_canonical(fs::path(_current_dir), ec);
        const fs::path assets = fs::weakly_canonical(fs::path(_asset_dir), ec);
        if (!ec) {
            if (current == assets) return "Assets";
            const fs::path relative = fs::relative(current, assets, ec);
            if (!ec && !relative.empty() && *relative.begin() != fs::path(".."))
                return "Assets / " + relative.generic_string();
        }
        ec.clear();
        const fs::path scripts = fs::weakly_canonical(fs::path(_script_dir), ec);
        if (!ec) {
            if (current == scripts) return "Scripts";
            const fs::path relative = fs::relative(current, scripts, ec);
            if (!ec && !relative.empty() && *relative.begin() != fs::path(".."))
                return "Scripts / " + relative.generic_string();
        }
        return fs::path(_current_dir).filename().string();
    }

    void _draw_folder_tree(const char* root_label, const fs::path& folder) {
        std::error_code ec;
        if (!fs::is_directory(folder, ec)) {
            ImGui::TextDisabled("%s (missing)", root_label);
            return;
        }
        const bool selected = fs::path(_current_dir).lexically_normal() == folder.lexically_normal();
        ImGui::PushID(folder.string().c_str());
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                   ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                   ImGuiTreeNodeFlags_DefaultOpen;
        if (selected) flags |= ImGuiTreeNodeFlags_Selected;
        const bool open = ImGui::TreeNodeEx(root_label, flags);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) _current_dir = folder.string();
        if (open) {
            std::vector<fs::path> children;
            for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
                if (it->is_directory(ec)) children.push_back(it->path());
            }
            std::sort(children.begin(), children.end());
            for (const fs::path& child : children) _draw_folder_tree(child.filename().string().c_str(), child);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    std::string _unique_name(const std::string& dir, const std::string& base, const std::string& ext) {
        std::string name = base + ext;
        int n = 1;
        while (fs::exists(fs::path(dir) / name)) {
            name = base + std::to_string(n) + ext;
            ++n;
        }
        return name;
    }

    void _create_folder(EditorState& st) {
        try {
            fs::create_directories(_current_dir);
            std::string name = _unique_name(_current_dir, "New Folder", "");
            fs::create_directory(fs::path(_current_dir) / name);
            st.log("Created folder: " + name);
        } catch (std::exception& ex) { st.log_error(std::string("Create folder failed: ")+ex.what()); }
    }

    void _create_script(EditorState& st) {
        try {
            fs::create_directories(_script_dir);
            std::string fname = _unique_name(_script_dir, "NewScript", ".cpp");
            std::string class_name = fs::path(fname).stem().string();
            // Sanitize to a valid C++ identifier
            for (auto& c : class_name) if (!std::isalnum((unsigned char)c) && c!='_') c='_';
            if (!class_name.empty() && std::isdigit((unsigned char)class_name[0])) class_name = "_"+class_name;

            fs::path script_path = fs::path(_script_dir) / fname;
            {
                std::ofstream f(script_path);
                f << "// C++ script — save this file; it's registered automatically the\n"
                  << "// one-file hot reload finishes. The editor stays locked while it compiles.\n"
                  << "#include \"../../../engine_cpp/script_system.hpp\"\n\n"
                  << "class " << class_name << " : public ScriptBase {\n"
                  << "public:\n"
                  << "    void awake() override {}\n"
                  << "    void start() override {}\n"
                  << "    void update(float dt) override {}\n\n"
                   << "    void on_collision_enter(EntityRef other) override {}\n"
                   << "    void on_trigger_enter(EntityRef other) override {}\n"
                  << "};\n";
            }

            const std::string project = project_name_from_scene_path(st.scene_path);
            _auto_rebuild_latch(project).attempted = false;        // re-arm auto rebuild for this project
            script_staleness_tracker(project).clear_pending();     // ensure the new file is treated as a fresh change
            st.log("Created script: " + fname + " (class " + class_name + "). Building this script only.");
            if (AutoHotReload::is_active()) {
                AutoHotReload::instance().request_rebuild(script_path);
            } else if (!st.playing && _initial_modules_loaded() && !script_rebuild_state(project).in_progress.load()) {
                // Kept only for configurations where the fast watcher is not
                // active. Normal editor operation uses the single-script
                // request above rather than the project-wide meta target.
                rebuild_scripts_module(project);
            }
        } catch (std::exception& ex) { st.log_error(std::string("Create script failed: ")+ex.what()); }
    }

    void _create_texture(EditorState& st) {
        try {
            fs::create_directories(_current_dir);
            std::string fname = _unique_name(_current_dir, "NewTexture", ".bmp");
            // Write a minimal 1x1 white BMP (no PNG encoder dependency needed)
            std::ofstream f(fs::path(_current_dir) / fname, std::ios::binary);
            unsigned char bmp[] = {
                0x42,0x4D, 0x3A,0,0,0, 0,0,0,0, 0x36,0,0,0,
                0x28,0,0,0, 1,0,0,0, 1,0,0,0, 1,0, 24,0,
                0,0,0,0, 4,0,0,0, 0,0,0,0, 0,0,0,0,
                0,0,0,0, 0,0,0,0,
                0xFF,0xFF,0xFF, 0,
            };
            f.write(reinterpret_cast<char*>(bmp), sizeof(bmp));
            st.log("Created texture: " + fname);
        } catch (std::exception& ex) { st.log_error(std::string("Create texture failed: ")+ex.what()); }
    }

    // Unity-style "Create > Material": drops a new .material JSON asset
    // (engine default — Sprite-Unlit, white tint, no texture override) into
    // the current folder and selects it so its properties show up in the
    // Inspector immediately, exactly like Unity's Project window does.
    void _create_material(EditorState& st) {
        try {
            fs::create_directories(_current_dir);
            std::string fname = _unique_name(_current_dir, "New Material", ".material");
            material::MaterialAsset mat = material::default_material();
            mat.name = fs::path(fname).stem().string();
            fs::path path = fs::path(_current_dir) / fname;
            if (mat.save_to_file(path.string())) {
                st.log("Created material: " + fname);
                _selected = fname;
                st.select_asset(path.string());
            } else {
                st.log_error("Create material failed: could not write file.");
            }
        } catch (std::exception& ex) { st.log_error(std::string("Create material failed: ")+ex.what()); }
    }
};

// ─── Toolbar icons ──────────────────────────────────────────────────────────
// The viewport toolbar (Play/Pause/Step/Stop + Select/Rotate/Scale/Paint
// tools) used to rely on Unicode glyphs like ▶ ⏭ ↖ ↻ ⎺ ✏. Checked against
// every font this editor actually falls back to (Segoe UI on Windows,
// Liberation Sans / DejaVu Sans on Linux, Helvetica on macOS) — none of
// them contain these specific codepoints, so the buttons rendered blank.
// These are simple vector shapes instead, drawn the same way as the
// hierarchy/inspector/asset icons above, so they render identically on
// every platform regardless of installed fonts.
namespace toolbar_icons {
    inline void draw(ImDrawList* dl, ImVec2 c, float s, const char* kind, ImU32 col) {
        std::string k(kind);
        float h = s * 0.5f;
        if (k == "play") {
            dl->AddTriangleFilled({c.x-h*0.55f,c.y-h*0.85f},{c.x-h*0.55f,c.y+h*0.85f},{c.x+h*0.75f,c.y}, col);
        } else if (k == "pause") {
            float bw = s*0.22f;
            dl->AddRectFilled({c.x-h*0.6f,c.y-h*0.8f},{c.x-h*0.6f+bw,c.y+h*0.8f}, col);
            dl->AddRectFilled({c.x+h*0.6f-bw,c.y-h*0.8f},{c.x+h*0.6f,c.y+h*0.8f}, col);
        } else if (k == "step") {
            dl->AddTriangleFilled({c.x-h*0.8f,c.y-h*0.75f},{c.x-h*0.8f,c.y+h*0.75f},{c.x+h*0.1f,c.y}, col);
            float bw = s*0.18f;
            dl->AddRectFilled({c.x+h*0.35f,c.y-h*0.75f},{c.x+h*0.35f+bw,c.y+h*0.75f}, col);
        } else if (k == "stop") {
            dl->AddRectFilled({c.x-h*0.65f,c.y-h*0.65f},{c.x+h*0.65f,c.y+h*0.65f}, col, 1.f);
        } else if (k == "select") {
            // Classic pointer-cursor silhouette
            ImVec2 pts[7] = {
                {c.x-h*0.7f, c.y-h*0.85f}, {c.x-h*0.7f, c.y+h*0.7f}, {c.x-h*0.28f, c.y+h*0.32f},
                {c.x-h*0.02f, c.y+h*0.85f}, {c.x+h*0.2f, c.y+h*0.7f}, {c.x-h*0.08f, c.y+h*0.18f}, {c.x+h*0.42f, c.y+h*0.02f}
            };
            dl->AddConvexPolyFilled(pts, 7, col);
        } else if (k == "rotate") {
            const int seg = 20;
            const float r = h*0.72f;
            const float tau = 6.28318f;
            for (int i=0;i<seg;++i) {
                float a0 = (0.15f + (float)i/seg*0.72f) * tau;
                float a1 = (0.15f + (float)(i+1)/seg*0.72f) * tau;
                dl->AddLine({c.x+cosf(a0)*r,c.y+sinf(a0)*r},{c.x+cosf(a1)*r,c.y+sinf(a1)*r}, col, 1.8f);
            }
            float a = (0.15f+0.72f) * tau;
            ImVec2 tip{c.x+cosf(a)*r,c.y+sinf(a)*r};
            float perp = a + 1.5708f;
            dl->AddTriangleFilled(
                {tip.x + cosf(a)*5.f, tip.y + sinf(a)*5.f},
                {tip.x + cosf(perp)*4.5f - cosf(a)*2.f, tip.y + sinf(perp)*4.5f - sinf(a)*2.f},
                {tip.x - cosf(perp)*4.5f - cosf(a)*2.f, tip.y - sinf(perp)*4.5f - sinf(a)*2.f}, col);
        } else if (k == "scale") {
            dl->AddLine({c.x-h*0.8f,c.y+h*0.8f},{c.x+h*0.8f,c.y-h*0.8f}, col, 1.6f);
            dl->AddRectFilled({c.x-h*0.9f,c.y+h*0.55f},{c.x-h*0.55f,c.y+h*0.9f}, col);
            dl->AddRectFilled({c.x+h*0.55f,c.y-h*0.9f},{c.x+h*0.9f,c.y-h*0.55f}, col);
        } else if (k == "paint") {
            dl->AddLine({c.x-h*0.55f,c.y+h*0.75f},{c.x+h*0.35f,c.y-h*0.75f}, col, s*0.26f);
            dl->AddTriangleFilled({c.x-h*0.72f,c.y+h*0.92f},{c.x-h*0.55f,c.y+h*0.5f},{c.x-h*0.28f,c.y+h*0.73f}, col);
        } else if (k == "frame") {
            dl->AddCircle(c, h*0.6f, col, 16, 1.4f);
            dl->AddLine({c.x-h,c.y},{c.x-h*0.35f,c.y}, col, 1.4f);
            dl->AddLine({c.x+h*0.35f,c.y},{c.x+h,c.y}, col, 1.4f);
            dl->AddLine({c.x,c.y-h},{c.x,c.y-h*0.35f}, col, 1.4f);
            dl->AddLine({c.x,c.y+h*0.35f},{c.x,c.y+h}, col, 1.4f);
        }
    }
}

// ─── Viewport ─────────────────────────────────────────────────────────────────
class ViewportPanel {
    vkr::RendererBackend* _backend = nullptr;     // non-owning, lives in editor_main
    vkr::RenderTarget      _rt;                   // offscreen scene image
    VkDescriptorSet        _imgui_ds = VK_NULL_HANDLE; // ImGui::Image() handle for _rt
    int            _tex_w=0, _tex_h=0;
    Camera         _cam;
    std::unique_ptr<RenderSystem> _rs;
    bool           _drag_pan  = false;
    ImVec2         _prev_mouse = {0,0};
    char           _rename_buf[128] = {};
    int            _rename_target = -1;

    // Transform-drag state (move/rotate/scale) and rect-select state
    bool           _drag_left = false;
    std::string    _drag_mode;            // "move" | "rotate" | "scale" | "rect_select"
    ImVec2         _prev_drag_left = {0,0};
    ImVec2         _rect_select_start = {0,0};
    struct DragStart { float x,y,rotation,scale_x,scale_y; };
    std::unordered_map<int, DragStart> _drag_start_values;

    // Same idea as DragStart, but for UI components (UIPanel/UIButton/UIImage/
    // UIProgressBar/UIText), which live in screen-space pos_x/pos_y/width/height
    // fields instead of Transform — they need their own drag snapshot.
    struct UIDragStart { float pos_x,pos_y,width,height; };
    std::unordered_map<int, UIDragStart> _ui_drag_start_values;

    std::string    _last_asset_dir;
    std::vector<std::string> _last_sorting_layers;
    ImVec2         _last_img_pos = {0,0};
    ImVec2         _last_local_mouse = {0,0};

    // Systems for play-in-editor
    InputSystem     _input;
    AnimatorSystem  _anim_sys;
    ParticleSystem  _particle_sys;
    ScriptSystem    _script_sys;
    TilemapColliderSystem _tilemap_sys;
    EventSystem     _event_sys;
    AudioSystem     _audio_sys;
    transform::TransformSystem _transform_sys;

    float  _fps = 0.f;
    int    _frame = 0;
    // Play-in-Editor previously skipped FixedUpdate and LateUpdate entirely.
    // Keep a bounded accumulator so editor Play matches the standalone loop
    // without allowing a breakpoint/minimize stall to create a catch-up storm.
    float  _fixed_update_accumulator = 0.f;

    // ── Game resolution frame overlay ─────────────────────────────────────
    // A thin fixed border drawn in screen space showing the exact game
    // resolution rect, centred in the viewport — matches Unity 2D's grey
    // frame that stays stationary when you pan/zoom the scene view.
    bool   _show_res_frame = true;
    int    _game_res_w     = 1920;
    int    _game_res_h     = 1080;

    bool        _scene_switch_requested = false;
    std::string _scene_switch_target;

    // Standalone export invokes CMake/MSBuild and can take minutes on a first
    // build. Keep it off the UI thread, but allow exactly one export job for
    // this editor instance so export files and compiler intermediates cannot
    // race each other.
    std::thread       _standalone_build_thread;

public:
    struct FrameDebugSnapshot {
        uint32_t draw_calls = 0;
        uint32_t regular_quads = 0;
        uint32_t instanced_quads = 0;
        uint32_t instanced_batches = 0;
        uint32_t considered = 0;
        uint32_t visible = 0;
        uint32_t culled = 0;
    };

    FrameDebugSnapshot frame_debug_snapshot() const {
        FrameDebugSnapshot snapshot;
        if (!_rs) return snapshot;
        const auto& frame = _rs->frame_stats();
        const auto& cull = _rs->cull_stats();
        snapshot.draw_calls = frame.draw_calls;
        snapshot.regular_quads = frame.regular_quads;
        snapshot.instanced_quads = frame.instanced_quads;
        snapshot.instanced_batches = frame.instanced_batches;
        snapshot.considered = cull.total_considered;
        snapshot.visible = cull.visible;
        snapshot.culled = cull.culled;
        return snapshot;
    }

    ViewportPanel() : _cam(960,600) {}

    // Exposed so AutoHotReload can destroy instances before a per-script swap.
    // Only valid during play mode; in edit mode instances are empty.
    ScriptSystem& script_system() { return _script_sys; }

    ~ViewportPanel() {
        if (_standalone_build_thread.joinable()) _standalone_build_thread.join();
        if (_imgui_ds) { ImGui_ImplVulkan_RemoveTexture(_imgui_ds); _imgui_ds = VK_NULL_HANDLE; }
    }

    void init(vkr::RendererBackend& backend, const std::string& font_path = "") {
        _backend = &backend;
        _ensure_texture(960, 600);
        _rs = std::make_unique<RenderSystem>(backend, _cam);
        if (!font_path.empty()) _rs->load_default_font(font_path);
        _script_sys.set_input(&_input);
        _load_initial_scripts_modules();
    }

    // Project-level lighting is authored by the dockable Shadow 2D Settings
    // tool. Keep this bridge explicit so its controls affect the editor
    // viewport immediately instead of only after an exported-game restart.
    void set_global_lighting(bool enabled, float ambient_intensity,
                             const float* ambient_color, int max_lights,
                             float shadow_strength) {
        if (!_rs) return;
        RenderSystem::GlobalLightingSettings settings;
        settings.enabled = enabled;
        settings.ambient_intensity = ambient_intensity;
        if (ambient_color) {
            settings.ambient_color = {ambient_color[0], ambient_color[1], ambient_color[2]};
        }
        settings.max_lights = max_lights;
        settings.shadow_strength = shadow_strength;
        _rs->set_global_lighting_settings(settings);
    }

    // Called once per frame from editor_main's loop (alongside begin/end
    // input frame) — checks whether any project's background
    // rebuild_scripts_module() build has finished and, if so, performs the
    // actual module swap here on the main thread: destroy every live
    // ScriptBase instance from the currently-loaded module (their vtables
    // are about to become invalid), then LoadLibrary/dlopen the freshly
    // built one and re-register. Must run on the main thread because
    // _script_sys (whose instances are being destroyed) is also touched
    // every frame by draw()'s Play-mode update — doing this from the
    // background build thread would race.
    //
    // Iterates every project that has EVER had a rebuild kicked off this
    // session (i.e. has an entry in _all_script_rebuild_states()), not
    // just "the current scene's project" — so a background rebuild for a
    // project you've since navigated away from still gets polled and
    // swapped in correctly rather than silently stalling forever with
    // rb.done sitting true and nobody ever calling take_pending_module_path().
    void poll_script_rebuild(EditorState& st) {
        // Never replace a gameplay DLL while a Play session is running. This
        // is also a safety boundary for source timestamps changed by OneDrive
        // or another sync tool: keep the completed refresh pending and apply
        // it after Stop rather than mutating the active script registry.
        if (st.playing) return;
        for (auto& [project, state_ptr] : _all_script_rebuild_states()) {
            auto& rb = *state_ptr;

            // A script instance owns a vtable and a custom deleter inside its
            // source DLL. It is therefore vital that this main-thread fence
            // runs BEFORE FreeLibrary. Handle both the normal in-progress
            // path and an unusually fast build that completes before the next
            // frame; the old condition missed the latter and could reload on
            // top of live instances.
            if ((rb.in_progress.load() || rb.done.load()) && !rb.old_modules_unloaded.load()) {
                _script_sys.reset_all_instances();
                ScriptModuleLoader::instance().unload_project(project);
                rb.old_modules_unloaded = true;
            }

            if (!rb.done.load()) continue;
            fs::path module_path = rb.take_pending_module_path();
            fs::path module_dir = rb.take_pending_module_dir();
            if (!module_dir.empty()) {
                // Per-script DLL rebuild path — build output is a directory
                // containing one DLL per script. Load all of them.
                if (!rb.success.load()) {
                    std::string msg = rb.get_message();
                    st.log_error("[" + project + " FAILED] " + msg);
                    auto& t = script_staleness_tracker(project);
                    t.has_failed_once = true;
                    t.last_failed_at = std::chrono::steady_clock::now();
                    t.has_reloaded_once = true;
                    t.last_reload_handled_at = t.last_failed_at;
                    continue;
                }
                _script_sys.reset_all_instances();
                std::string err;
                int n = ScriptModuleLoader::instance().load_all_for_project(
                    project, module_dir,
                    find_engine_project_root() / "games" / project / "scripts", &err);
                auto& t = script_staleness_tracker(project);
                if (n > 0) {
                    st.log_warn("Scripts reloaded for '" + project + "' (" + std::to_string(n) + " per-script DLLs)");
                    mark_scripts_registered(project);
                    script_introspect::invalidate_all();
                    t.has_failed_once = false;
                } else {
                    st.log_warn("Script reload failed for '" + project + "': " + err);
                    t.has_failed_once = true;
                }
                t.has_reloaded_once = true;
                t.last_reload_handled_at = std::chrono::steady_clock::now();
                continue;
            }

            if (module_path.empty()) {
                // Configure or compile failed before a module was ever
                // produced (see rebuild_scripts_module's early returns).
                // This used to be a silent `continue` — the only failure
                // reason existed in rb.message, which nothing ever read
                // outside the (transient) "Compiling Scripts" modal. Log it
                // to the Console so it's actually visible, and only once
                // per failed attempt (rb.done is cleared right after by
                // rebuild_scripts_module's next run, so this won't repeat
                // for the same failure).
                if (!rb.success.load()) {
                    std::string msg = rb.get_message();
                    st.log_error("[" + project + " FAILED] " + msg);
                    auto& t = script_staleness_tracker(project);
                    t.has_failed_once = true;
                    t.last_failed_at = std::chrono::steady_clock::now();
                    t.has_reloaded_once = true;
                    t.last_reload_handled_at = t.last_failed_at;
                }
                continue;
            }

            // Every live instance's vtable belongs to the module about to
            // be unloaded — destroy them BEFORE ScriptModuleLoader::load()
            // calls FreeLibrary/dlclose on it, never after. reset_all_
            // instances() is global (not project-scoped), which is correct
            // in practice: only one project's scene is ever open/playing
            // at a time, so any live ScriptBase instances at all belong to
            // whichever project is currently active — see reset_all_
            // instances()'s own comment in script_system.hpp.
            _script_sys.reset_all_instances();

            std::string err;
            if (ScriptModuleLoader::instance().load(project, module_path, &err)) {
                st.log_warn("Scripts reloaded for '" + project + "' — " + module_path.filename().string());
                // The new module now has every script file that was on
                // disk at build time actually registered — advance that
                // project's staleness tracker's known-paths so none of
                // them re-trigger an auto rebuild on a later edit (only a
                // genuinely new/unregistered script file should do that).
                mark_scripts_registered(project);
                script_introspect::invalidate_all();
                auto& t = script_staleness_tracker(project);
                st.log_warn("DIAG post-reload known_paths.size()=" + std::to_string(t.known_paths.size()));
                if (!t.known_paths.empty()) {
                    st.log_warn("DIAG post-reload known_paths[0]='" + *t.known_paths.begin() + "'");
                }
                t.has_failed_once = false;
                t.has_reloaded_once = true;
                t.last_reload_handled_at = std::chrono::steady_clock::now();
            } else {
                st.log_warn("Script reload failed for '" + project + "': " + err);
                auto& t = script_staleness_tracker(project);
                t.has_failed_once = true;
                t.last_failed_at = std::chrono::steady_clock::now();
                t.has_reloaded_once = true;
                t.last_reload_handled_at = t.last_failed_at;
            }
        }
    }

private:
    // On editor startup, load whatever scripts module(s) already exist —
    // without this, a freshly launched editor has zero scripts registered
    // until the user edits something and triggers a rebuild. Scans BOTH
    // possible build locations (the normal full `cmake --build build`
    // tree, in case the project was built that way at least once, and the
    // isolated build/scripts_module_fast tree the editor's own auto-
    // rebuild writes into) for every project that has a scripts/ folder,
    // and for each one loads the most-recently-modified
    // game_scripts_<ns>* file found — since rebuilds use a timestamped
    // name, there may be several stale ones left over from previous runs;
    // "most recent" is the one actually worth loading.
    void _load_initial_scripts_modules() {
        fs::path root = find_engine_project_root();
        for (auto& project : _all_script_project_names()) {
            std::string ns = _cmake_namespace_for_project(project);
            // Combine the workspace recovery outputs with the user-local hot
            // reload cache. The loader chooses the newest DLL per source over
            // the entire set, so one newly-created script never hides the
            // rest of an otherwise working project.
            const fs::path local_build_dir = script_module_build_dir(root, ns);
            std::vector<fs::path> search_roots = {
                root / "build" / "scripts_module_fast" / ns,
                root / "build" / "scripts_module_fast" / ns / "Release",
                root / "build",
                root / "build" / "scripts_module_fast",
                root / "build" / "scripts_module_fast" / "Release",
                local_build_dir,
                local_build_dir / "Release",
            };
            bool loaded_any = ScriptModuleLoader::instance().load_all_for_project(
                project, search_roots, root / "games" / project / "scripts") > 0;

            // ── Try per-script DLLs first —— each script is its own DLL, so
            // a compile error in one script does not prevent the others from
            // loading (matching Unity's per-file model). The loader silently
            // skips any DLL that fails to load, so individual script errors
            // are isolated.

            // ── Fall back to monolithic ────────────────────────────────
            if (!loaded_any) {
                std::string target_stem = "game_scripts_" + ns;
                fs::path best_path;
                std::filesystem::file_time_type best_time{};

                for (auto& search_root : search_roots) {
                    std::error_code ec;
                    if (!fs::exists(search_root, ec)) continue;
                    for (auto it = fs::recursive_directory_iterator(search_root, ec); !ec && it != fs::recursive_directory_iterator(); ++it) {
                        if (!it->is_regular_file(ec)) continue;
#if defined(_WIN32)
                        if (it->path().extension() != ".dll") continue;
#else
                        auto ext = it->path().extension().string();
                        if (ext != ".so" && ext != ".dylib") continue;
#endif
                        std::string stem = it->path().stem().string();
#if !defined(_WIN32)
                        if (stem.rfind("lib", 0) == 0) stem = stem.substr(3);
#endif
                        bool matches = (stem == target_stem) || (stem.rfind(target_stem + "_", 0) == 0);
                        if (!matches) continue;
                        std::error_code tec;
                        auto mtime = fs::last_write_time(it->path(), tec);
                        if (tec) continue;
                        if (best_path.empty() || mtime > best_time) {
                            best_path = it->path();
                            best_time = mtime;
                        }
                    }
                }

                if (!best_path.empty()) {
                    std::string err;
                    if (ScriptModuleLoader::instance().load(project, best_path, &err))
                        loaded_any = true;
                }
            }

            if (!loaded_any) continue; // no modules found; Rebuild Scripts will create them

            // Advance the staleness tracker's known_paths to reflect what
            // is ACTUALLY compiled into the module(s) we just loaded.
            mark_scripts_registered(project);
            scripts_changed_since_build(project);
        }
        // Signal AssetsPanel that it's safe to run the auto-rebuild latch.
        // The latch calls _script_project_needs_initial_rebuild(), which
        // queries the live ScriptRegistry. If it fires before this function
        // finishes, the registry is still empty and every script looks
        // unregistered, triggering a spurious rebuild every single launch.
        _initial_modules_loaded() = true;
        // Clear any stale field-introspection cache that may have been
        // populated before the module was available (e.g. an inspector draw
        // that ran before this function finished, or a previous session).
        script_introspect::invalidate_all();
    }

public:
    void draw(EditorState& st, float dt) {
        // Build Settings can request an export while the Viewport tab is
        // hidden/collapsed; consume it before Begin() so Build always works.
        if (st.request_standalone_build || st.request_standalone_build_and_run) {
            const bool run_after_build = st.request_standalone_build_and_run;
            st.request_standalone_build = false;
            st.request_standalone_build_and_run = false;
            _start_standalone_build(st, run_after_build);
        }
        ImGui::SetNextWindowSize({960,680}, ImGuiCond_FirstUseEver);
        ImGuiWindowFlags wflags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        if (!ImGui::Begin("Viewport##win", nullptr, wflags)) { ImGui::End(); return; }

        // Handle menu-bar play/stop button clicks
        if (st._menubar_play_clicked) {
            st._menubar_play_clicked = false;
            if (!st.playing) start_play(st);
            else             stop_play(st);
        }
        if (st._menubar_stop_clicked) {
            st._menubar_stop_clicked = false;
            if (st.playing) stop_play(st);
        }

        // ── Toolbar ───────────────────────────────────────────────────────────
        _draw_toolbar(st);
        ImGui::Separator();

        // ── Viewport image ────────────────────────────────────────────────────
        ImVec2 avail = ImGui::GetContentRegionAvail();
        int vw = std::max(1, (int)avail.x);
        int vh = std::max(1, (int)avail.y);
        _ensure_texture(vw, vh);
        _cam.width  = vw;
        _cam.height = vh;

        // Update editor/play tilemap collider caches every frame so the
        // Generate Colliders toggle and collider outlines respond immediately
        // even while not in Play mode.
        viewport_grid2d_sys().update(st.entities);
        viewport_tilemap_sys().update(st.entities);

        // Run play systems
        if (st.playing && (!st.paused || st.step_once)) {
            // Feed current mouse world position into InputSystem so scripts'
            // mouse_x()/mouse_y() helpers resolve to viewport-local world coords.
            auto [mwx, mwy] = _cam.screen_to_world(_last_local_mouse.x, _last_local_mouse.y);
            _input.set_mouse_world_pos(mwx, mwy);

            // Publish the *actual* current viewport texture size via Screen,
            // and feed the mouse position in that same viewport-local pixel
            // space (not raw OS window coordinates from SDL_GetMouseState).
            // UIButton hit-testing in game scripts (e.g. abyss_menu_controller.cpp's
            // ButtonClicked()) calls Screen::Width()/Height() to reproduce
            // RenderSystem::draw_ui()'s resolve() math — so as long as both
            // sides read the same live size, clicks land correctly no matter
            // how the viewport panel is resized or docked. (Previously
            // neither side agreed: the renderer used the real texture size,
            // the script used a hardcoded constant, and the mouse position
            // was raw window coordinates — three different, independently
            // wrong numbers, which is why clicks looked "random".)
            Screen::Set(_cam.width, _cam.height);
            _input.mouse_x = (int)_last_local_mouse.x;
            _input.mouse_y = (int)_last_local_mouse.y;

            // Advance the global clock. Without this, Time::elapsed_time and
            // Time::frame_count stay frozen at whatever they were when Play
            // started (standalone's run_game() in core.cpp calls this every
            // frame, but Play-in-Editor never did) — any script logic that
            // computes a deadline as "now + duration" (e.g. AbyssFxBurst's
            // self-destruct timer) compares against a `now` that never
            // moves, so those entities never reach their deadline and pile
            // up forever instead of being cleaned up.
            Time::update(dt);
            const float simulation_scale = std::max(0.0f, (float)Time::time_scale);
            const float simulation_dt = dt * simulation_scale;
            // Fixed physics is paced by the scaled accumulator below, so the
            // physics solver itself remains at 1x to prevent double scaling.
            phys::set_physics_time_scale(1.0f);

            // Retire lines from prior frames before scripts append this
            // frame's diagnostics. This preserves Unity's one-frame default
            // for duration==0 while bounding the shared Debug queue.
            Debug::update((float)dt);

            // Only pay the network polling cost when a transport is actually
            // active. In solo editor play (no lobby, no matchmaking) this
            // entire block is a no-op: no socket polling, no event dispatch,
            // no Matchmaking overhead every frame.
            if (Network::IsConnected()) {
                Network::Update(0.f);
                for (auto& ev : Network::ConsumeEvents()) {
                    Matchmaking::HandleNetworkEvent(ev);
                    Replication::HandleNetworkEvent(st.entities, st.asset_dir, ev);
                    NetPredict::HandleNetworkEvent(ev);
                    {
                        Entity* ev_target = nullptr;
                        std::uint32_t ev_net_id = 0;
                        if (ev.data.contains("target_net_id"))
                            ev_net_id = (std::uint32_t)ev.data.value("target_net_id", 0);
                        else if (ev.data.contains("net_id"))
                            ev_net_id = (std::uint32_t)ev.data.value("net_id", 0);
                        if (ev_net_id != 0)
                            ev_target = Replication::FindByNetId(st.entities, ev_net_id);
                        EventBus::instance().emit(ev.name, ev.data, ev_target);
                    }
                }
                Matchmaking::Update((float)dt);
            }

            // Order matches standalone (engine_cpp/engine/core.cpp run_game()):
            // scripts run FIRST so anything they set this frame (Animator's
            // current_animation, Rigidbody velocity, etc.) is what physics /
            // the animator / the renderer actually see and act on this same
            // frame. The previous order ran _anim_sys before _script_sys,
            // so the Animator always rendered last frame's animation state
            // (a permanent one-frame lag) — visually this showed up as the
            // sprite looking stuck on "idle" even while walking/shooting,
            // since current_animation only gets written once per state
            // change and the Animator kept reading the stale value.
            //
            // Real per-stage timings (used by ProfilerPanel — see
            // unity_gap_features.hpp — instead of made-up numbers): each
            // block below is bracketed with std::chrono::steady_clock and
            // written into st.frame_*_ms. Measured here rather than in
            // core.cpp's STAGE() macro because that macro is about
            // exception-safety, not timing, and the standalone build has no
            // editor UI to show the numbers to anyway — this is purely an
            // editor/profiler concern.
            using ProfClock = std::chrono::steady_clock;
            auto _prof_t0 = ProfClock::now();
            crashreport::set_stage("editor play: native scripts");
            advance_destroy_timers(st.entities, simulation_dt);
            _script_sys.update(st.entities, dt);
            _script_sys.late_update(st.entities, dt);
            st.frame_script_ms = std::chrono::duration<float, std::milli>(ProfClock::now() - _prof_t0).count();

            if (Matchmaking::InMatch()) {
                NetSpawn::ReplicateLocalPlayer(st.entities);
                // Host: broadcasts transforms for every Replication::Spawn'd
                // (non-player) entity — enemies, items, projectiles — at
                // Replication's configured tick rate. No-op on clients.
                Replication::Tick(st.entities, (float)dt);
                // Client: smoothly interpolate every networked entity that
                // isn't this machine's own local player toward the latest
                // samples received above, instead of snapping on arrival.
                // No-op on host (host's own simulation is authoritative).
                NetPredict::InterpolateAllRemote(st.entities);
            }

            if (_scene_switch_requested) {
                _apply_scene_switch(st);
            }

            _event_sys.update(st.entities, simulation_dt);

            // Run the same visual-script graph in Editor Play mode as the
            // standalone runtime. The graph resolver accepts st.asset_dir as
            // well as a project root, so both launch paths share one asset.
            const std::string graph_scene = fs::path(st.scene_path).stem().string();
            if (!graph_scene.empty()) {
                crashreport::set_stage("editor play: visual scripting");
                script_graph_integration(st.entities, graph_scene, st.asset_dir, simulation_dt);
            }

            // Unity-style fixed callbacks run at a stable cadence even when
            // the viewport renders at a different FPS. Physics still owns its
            // existing adaptive substep solver below; this accumulator only
            // governs user FixedUpdate callbacks and is capped for stability.
            constexpr float kEditorFixedStep = 1.f / 60.f;
            constexpr int kMaxEditorFixedSteps = 8;
            _fixed_update_accumulator = std::min(
                _fixed_update_accumulator + simulation_dt,
                kEditorFixedStep * static_cast<float>(kMaxEditorFixedSteps));
            int fixed_steps = 0;
            while (_fixed_update_accumulator >= kEditorFixedStep && fixed_steps++ < kMaxEditorFixedSteps) {
                crashreport::set_stage("editor play: fixed scripts");
                _script_sys.fixed_update(st.entities, kEditorFixedStep);
                if (!graph_scene.empty()) {
                    script_graph_fixed_integration(st.entities, graph_scene, st.asset_dir, kEditorFixedStep);
                }
                _fixed_update_accumulator -= kEditorFixedStep;
            }

            _prof_t0 = ProfClock::now();
            crashreport::set_stage("editor play: physics");
            phys::apply_physics(st.entities, simulation_dt);
            st.frame_physics_ms = std::chrono::duration<float, std::milli>(ProfClock::now() - _prof_t0).count();

            _prof_t0 = ProfClock::now();
            crashreport::set_stage("editor play: runtime systems");
            _transform_sys.update(st.entities);
            _anim_sys.update(st.entities, simulation_dt);
            _particle_sys.update(st.entities, simulation_dt);
            _audio_sys.update(st.entities, simulation_dt);
            st.frame_other_ms = std::chrono::duration<float, std::milli>(ProfClock::now() - _prof_t0).count();
            // CaptureSnapshot deep-clones every entity every frame for rollback.
            // Skip entirely in solo play — there are no peers to rollback for.
            if (Network::IsConnected()) {
                Network::CaptureSnapshot((std::uint32_t)Time::frame_count);
            }
            st.step_once = false;
            crashreport::set_stage("editor: frame complete");
        } else if (!st.playing) {
            // Not playing: AnimatorSystem::update() above never runs (it's
            // inside the st.playing branch), so a sprite-sheet-mode
            // Animator's SpriteRenderer would otherwise never get its
            // source_rect cropped to a single frame at all — RenderSystem
            // then draws the ENTIRE multi-frame spritesheet stretched into
            // the sprite's bounds, which for e.g. a 4-frame horizontal
            // sheet looks exactly like four copies of the same enemy
            // standing in a row. apply_static_frame crops to one frame
            // (whichever the Animator last stopped on, or frame 0) without
            // advancing playback, evaluating transitions, or firing
            // animation events — purely a display fix for the edit-mode
            // scene view.
            _anim_sys.apply_static_frame(st.entities);
            // Not playing: scripts, physics, and the event/transform/anim/
            // particle/audio block above never ran this frame, so their
            // profiler entries should read 0 rather than holding the last
            // value from whenever play mode was last active.
            st.frame_script_ms  = 0.f;
            st.frame_physics_ms = 0.f;
            st.frame_other_ms   = 0.f;
        }

        // Resolve the parent/child transform hierarchy into cached world-space
        // values (transform_system.hpp) before anything reads world position
        // this frame — only needed in edit mode; the play branch above already
        // ran _transform_sys.update() as part of the simulation tick.
        if (!st.playing) _transform_sys.update(st.entities);

        // Follow the active Camera2D entity only while Playing. In Edit mode
        // the viewport camera is free-floating (user-controlled pan/zoom) —
        // calling update() unconditionally here used to snap _cam.x/_cam.y
        // (and zoom) back to the entity's Transform every single frame,
        // which silently undid middle-mouse pan and scroll-wheel zoom.
        if (st.playing) _cam.update(st.entities, dt);

        // ── Render to texture ─────────────────────────────────────────────────
        // Entities via RenderSystem (persistent — avoids reloading textures every frame)
        if (st.asset_dir != _last_asset_dir) {
            _rs->set_asset_dir(st.asset_dir);
            _last_asset_dir = st.asset_dir;
        }
        // Keep the renderer's sorting-layer order in sync with the project's
        // (editable via Scene > Sorting Layers). Cheap to compare every frame;
        // only pushes to RenderSystem when the list actually changed.
        if (st.sorting_layers != _last_sorting_layers) {
            _rs->set_sorting_layers(st.sorting_layers);
            _last_sorting_layers = st.sorting_layers;
        }

        // Resolve UILayoutGroup arrangement (writes each child's anchor/pivot/
        // pos/width/height — see feature_systems.hpp) before draw_ui reads
        // those same fields below. Runs every frame regardless of play state,
        // same as the tilemap collider cache above.
        viewport_ui_layout_sys().update(st.entities);

        auto _prof_render_t0 = std::chrono::steady_clock::now();
        VkCommandBuffer cmd = _rs->begin_render_to_target(_rt, _tex_w, _tex_h, {35,35,40,255});

        // Parallax backgrounds (drawn first, behind entities — mirrors
        // the render order in engine_cpp/engine/core.cpp). The editor viewport
        // previously never called this, so ParallaxBackground components had
        // no visible effect even though they worked in the exported game.
        _rs->draw_parallax(st.entities);

        // NOTE: the grid used to be drawn into the texture here (between
        // parallax and entities). It's now drawn as an ImGui overlay after
        // ImGui::Image() below instead — see _draw_grid's comment — since
        // there's no more immediate-mode 2D draw call to bake it into this
        // texture with. Same final pixels, same frame, just composited by
        // ImGui instead of by the renderer.

        _rs->draw(st.entities);
        // Resume any coroutine parked on CO_WAIT_END_OF_FRAME — only meaningful
        // while actually playing (script coroutines don't tick in edit mode);
        // mirrors the standalone build's call right after render.draw() in
        // engine_cpp/engine/core.cpp's run_game().
        if (st.playing) _script_sys.resume_end_of_frame(st.entities);

        // UI overlay — draw after world sprites, before gizmos.
        // Draw in BOTH play and edit modes so UI elements are visible and
        // selectable/movable in the viewport at all times.
        {
            int lmx=(int)(_last_local_mouse.x), lmy=(int)(_last_local_mouse.y);
            // In edit mode pass no mouse interaction so buttons don't fire while editing
            bool mb  = st.playing && ImGui::IsMouseDown(0);
            bool mjd = st.playing && ImGui::IsMouseClicked(0);
            _rs->draw_ui(st.entities, lmx, lmy, mb, mjd);
        }

        _rs->end_render_to_target(_rt, cmd);
        st.frame_render_ms = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - _prof_render_t0).count();

        // Update local mouse coords using LAST frame's image position (one
        // frame stale, imperceptible) — same as before, just no longer tied
        // to "before the texture is finalized" since gizmos aren't baked
        // into the texture anymore.
        {
            ImVec2 mp = ImGui::GetIO().MousePos;
            _last_local_mouse = {mp.x - _last_img_pos.x, mp.y - _last_img_pos.y};
        }

        // Upload to ImGui
        ImVec2 img_pos = ImGui::GetCursorScreenPos();
        _last_img_pos = img_pos;
        // RenderTarget stores rows bottom-to-top due to the negative-height viewport
        // Y-flip in SpriteBatch. Flip UV.y here so the image displays correctly.
        ImGui::Image((ImTextureID)(intptr_t)_imgui_ds, {(float)vw,(float)vh}, {0,1}, {1,0});

        // ── Prefab drag-drop from Assets panel (Gap 3) ─────────────────────
        // Accept ASSET_PATH payloads for .prefab files dropped onto the viewport.
        // Converts the drop screen-position to world-space so the prefab spawns
        // where the user dropped it, exactly like Unity's Project → Scene drag.
        if (!st.playing && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                const char* payload_str = static_cast<const char*>(pl->Data);
                ImVec2 drop_screen = ImGui::GetIO().MousePos;
                float local_x = drop_screen.x - img_pos.x;
                float local_y = drop_screen.y - img_pos.y;
                auto [dwx, dwy] = _cam.screen_to_world(local_x, local_y);
                prefab_ui::handle_viewport_prefab_drop(payload_str, dwx, dwy, st);
            }
            ImGui::EndDragDropTarget();
        }

        // ── Play-mode tint border (Unity-style) ─────────────────────────────
        // A bright border around the Game view while Playing is the single
        // biggest "did I actually leave Play mode" cue in Unity — without it
        // it's easy to keep editing a scene you think is in Edit mode. Drawn
        // as a thin inset rect so it doesn't obscure any pixels of the
        // actual rendered frame.
        if (st.playing) {
            ImDrawList* pdl = ImGui::GetWindowDrawList();
            ImU32 tint = st.paused ? IM_COL32(255, 200, 60, 255) : IM_COL32(80, 200, 255, 255);
            pdl->AddRect(img_pos, {img_pos.x + vw, img_pos.y + vh}, tint, 0.f, 0, 3.0f);
        }

        // Grid + gizmos (edit mode only) — drawn as a screen-space overlay
        // directly on top of the ImGui::Image() above, using the current
        // window's draw list. img_pos is this frame's actual image
        // position (not last frame's stale one), so these line up exactly
        // with what render_to_target just produced and with this frame's
        // real mouse position.
        if (st.show_grid && !st.playing) _draw_grid(st, img_pos);
        if (!st.playing) { _draw_gizmos(st, img_pos); _draw_rect_select_overlay(img_pos); }
        _draw_resolution_frame(img_pos);

        // ── FPS counter overlay (Unity Stats-style) ─────────────────────────
        // Small always-visible readout in the top-right corner of the scene
        // image, separate from the text status bar below (which scrolls out
        // of view on a small window). Drawn with a translucent backing
        // panel so it stays legible over any background colour/sprite.
        {
            ImDrawList* odl = ImGui::GetWindowDrawList();
            char fps_buf[32];
            snprintf(fps_buf, sizeof(fps_buf), "%.0f FPS", _fps);
            ImVec2 text_sz = ImGui::CalcTextSize(fps_buf);
            ImVec2 pad = {8.f, 4.f};
            ImVec2 box_max = {img_pos.x + vw - 6.f, img_pos.y + 6.f + text_sz.y + pad.y*2.f};
            ImVec2 box_min = {box_max.x - text_sz.x - pad.x*2.f, img_pos.y + 6.f};
            odl->AddRectFilled(box_min, box_max, IM_COL32(20,20,22,180), 3.f);
            ImU32 fps_col = _fps >= 50.f ? IM_COL32(120,230,140,255)
                          : _fps >= 30.f ? IM_COL32(240,200,90,255)
                                         : IM_COL32(240,90,90,255);
            odl->AddText({box_min.x + pad.x, box_min.y + pad.y}, fps_col, fps_buf);
        }

        // ── Render stats -> Console (culling / GPU instancing) ──────────────
        // Printed roughly twice a second while Playing, rather than every
        // frame, so the Console stays readable instead of being flooded.
        // _rs->cull_stats() and _rs->frame_stats() reflect whatever the
        // _rs->draw(st.entities) call above just did this frame.
        if (st.playing && !st.paused) {
            static float stats_log_timer = 0.f;
            stats_log_timer += dt;
            if (stats_log_timer >= 0.5f) {
                stats_log_timer = 0.f;
                auto& cs = _rs->cull_stats();
                auto& fs = _rs->frame_stats();
                st.log_engine(
                    "[Render] Culling: " + std::to_string(cs.visible) + "/" + std::to_string(cs.total_considered) +
                    " visible (" + std::to_string(cs.culled) + " culled)  |  " +
                    "Draw calls: " + std::to_string(fs.draw_calls) +
                    " (instanced batches: " + std::to_string(fs.instanced_batches) + ")  |  " +
                    "Quads: " + std::to_string(fs.regular_quads) + " regular, " + std::to_string(fs.instanced_quads) + " instanced");
            }
        }

        // ── Input handling ────────────────────────────────────────────────────
        bool hovered = ImGui::IsItemHovered();
        _handle_input(st, img_pos, hovered, dt);

        if (ImGui::BeginPopupContextItem("##viewportctx", ImGuiPopupFlags_MouseButtonRight)) {
            Entity* sel = st.selected_entity();
            if (!sel) {
                ImGui::TextDisabled("No entity selected");
            } else {
                int sid = st.selected_id;
                // Inline properties (Unity style)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));
                ImGui::TextUnformatted(sel->value("name","Entity").c_str());
                ImGui::PopStyleColor();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
                ImGui::Text("ID: %d  |  Parent: %d", sid, EditorState::parent_of(*sel));
                if (sel->contains("components") && (*sel)["components"].contains("Transform")) {
                    auto& t = (*sel)["components"]["Transform"];
                    ImGui::Text("Pos: (%.1f, %.1f)  Rot: %.1f", t.value("x",0.f), t.value("y",0.f), t.value("rotation",0.f));
                }
                ImGui::PopStyleColor();
                bool active = sel->value("active", true);
                if (ImGui::Checkbox("Active", &active)) {
                    st.undo.push_deep(st.entities);
                    if (Entity* cur = st.find_entity(sid)) (*cur)["active"] = active;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Rename")) _open_rename_dialog(st, sid);
            }
            ImGui::EndPopup();
        }

        // Status bar — color-coded mode label + useful stats
        {
            ImVec4 mode_col = st.playing
                ? (st.paused ? ImVec4(1.f,0.78f,0.24f,1.f) : ImVec4(0.31f,0.86f,0.47f,1.f))
                : ImVec4(0.5f,0.5f,0.55f,1.f);
            const char* mode_str = st.playing ? (st.paused ? "PAUSED" : "PLAYING") : "EDIT";
            ImGui::PushStyleColor(ImGuiCol_Text, mode_col);
            ImGui::Text("[%s]", mode_str);
            ImGui::PopStyleColor();
            ImGui::SameLine(0,8);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f,0.45f,0.45f,1.f));
            ImGui::Text("Tool: %-6s  |  Cam: (%.0f, %.0f)  Zoom: %.2f  |  FPS: %.0f  |  Entities: %d",
                st.tool.c_str(), _cam.x, _cam.y, _cam.zoom, _fps, (int)st.entities.size());
            ImGui::PopStyleColor();
        }

        ++_frame;
        if (dt > 0) _fps = _fps*0.9f + (1.f/dt)*0.1f;

        ImGui::End();
    }

    // Called once per frame from editor_main's event loop, BEFORE ImGui
    // processes events, so gameplay scripts see live input during Play mode.
    void begin_input_frame() { _input.begin_frame(); }
    void process_input_event(const SDL_Event& ev) { _input.process_event(ev); }
    void end_input_frame(float dt) { _input.end_frame(dt); }

    void start_play(EditorState& st) {
        // Unity-style auto-registration: if any script source changed since
        // the scripts module currently loaded for THIS PROJECT was built,
        // the class either doesn't exist in ScriptRegistry yet or is
        // running stale compiled code. Catch that here — the moment it
        // actually matters — rather than letting Play silently run old
        // behavior (the exact "I changed the script but nothing happened"
        // bug this whole feature exists to prevent). The rebuild runs
        // async with a blocking "Compiling Scripts" modal (drawn in
        // editor_main.cpp); Play is deferred until the user presses it
        // again after the modal clears, since the module swap
        // (poll_script_rebuild) happens on the next main-thread frame
        // after the build finishes, not synchronously within this call.
        //
        // Only the CURRENT scene's project is checked/rebuilt here — Play
        // only ever runs one scene, so there's no reason to force a
        // rebuild of every other project too just because their scripts
        // happen to also be stale.
        std::string project = project_name_from_scene_path(st.scene_path);
        if (scripts_changed_since_build(project) && !script_rebuild_state(project).in_progress.load()) {
            if (AutoHotReload::is_active()) {
                st.log_warn("Script changes are compiling one file at a time. Play will start when hot reload finishes.");
                return;
            }
            st.log_warn("New or changed script(s) detected in '" + project + "' — rebuilding before Play so they're registered...");
            script_staleness_tracker(project).clear_pending();
            rebuild_scripts_module(project);
            return;
        }
        if (script_rebuild_state(project).in_progress.load()) return; // modal is up; ignore Play presses mid-build

        // Deep-clone the scene so the snapshot is truly independent of the
        // entities that will mutate during play. A plain copy (= st.entities)
        // shares the underlying shared_ptr maps, so Stop would "restore" the
        // already-mutated state — the exact Play→Stop bug reported.
        st.scene_snapshot.clear();
        st.scene_snapshot.reserve(st.entities.size());
        for (const auto& e : st.entities)
            st.scene_snapshot.push_back(e.deep_clone());

        Debug::log("[Play] Start: " + st.scene_path + " (" + std::to_string(st.entities.size()) + " entities)");
        Debug::log("[Play] Active project: " + project);
        Debug::log("[Play] Scripts registered: " + std::to_string(ScriptRegistry::instance().all_names().size()));

        // Reserve real headroom up front, same as standalone (core.cpp
        // run_game()) does. Without this, st.entities enters Play with
        // capacity == size() (e.g. right after a previous Stop did
        // `st.entities = st.scene_snapshot`), so the very first bullet/enemy
        // spawned via Instantiate() can immediately exceed capacity and
        // reallocate the vector's buffer mid-frame. instantiate() itself
        // grows proactively before push_back, but spam-firing means many
        // Instantiate() calls can land in the same or adjacent frames, and
        // anything in the editor (viewport selection, gizmo target, drag
        // state) that resolved an Entity* earlier in that frame is left
        // dangling the instant that reallocation happens — read through
        // later in the same frame, that's a use-after-free, which is exactly
        // the kind of "works most of the time, crashes under spam" bug
        // reported. Standalone never hit this because it reserves once at
        // load and never re-enters Play with a tight buffer.
        st.entities.reserve(std::max<size_t>(st.entities.size() * 4, st.entities.size() + 1024));

        st.playing = true;
        _fixed_update_accumulator = 0.f;
        script_graph_prepare_scene(fs::path(st.scene_path).stem().string(), st.asset_dir);
        ScriptRegistry::instance().set_active_project_from_scene_path(st.scene_path);
        // Re-init scripts and immediately re-wire input (was previously lost
        // on every Play press, silently breaking all keyboard/mouse-driven
        // gameplay scripts since ScriptBase::input stayed null)
        _script_sys = ScriptSystem();
        _script_sys.set_input(&_input);
        // Clear stale collision/trigger "touching" state left over from a
        // previous Play session in this same editor process (see
        // phys::reset_contact_state's comment in physics.hpp) — without
        // this, every contact this session immediately dispatches as
        // on_*_stay instead of on_*_enter, so damage-on-contact and any
        // other Enter-only handler silently stop firing after the first
        // Play/Stop cycle, even though everything looks correct in code.
        phys::reset_contact_state();

        // Wire scripts' SceneManager::LoadScene(...) calls to an actual
        // scene swap in the editor's Play-in-Editor loop. Without this,
        // the handler is never installed during editor Play (only the
        // standalone run_game() loop in core.cpp installs it), so every
        // script-driven scene change (menu buttons, portals, etc.) just
        // logs "called before engine handler was installed" and does
        // nothing — Play-in-Editor silently can't change scenes at all.
        _scene_switch_requested = false;
        _scene_switch_target.clear();
        Network::SeedSessionFromString(st.scene_path);
        Network::BindScene(&st.entities);
        SceneManager::SetLoadSceneHandler([this](const std::string& next_scene){
            _scene_switch_requested = true;
            _scene_switch_target = next_scene;
        });

        EventBus::instance().clear_all();
        Debug::clear_lines();
        Debug::log("[Play] ScriptSystem created. Starting game loop.");
        st.log("Play In Editor started.");
    }

    void stop_play(EditorState& st) {
        st.playing = false;
        st.paused  = false;
        _fixed_update_accumulator = 0.f;
        SceneManager::ClearLoadSceneHandler();
        // Keep no live instances after Play stops. Besides making each new
        // session truly fresh, this guarantees a later script hot-rebuild
        // cannot unload a DLL still referenced by a stopped session.
        _script_sys.reset_all_instances();
        // Clear all EventBus handlers registered by scripts this session.
        // Scripts call EventBus::instance().subscribe() in Awake/Start every
        // Play press — without this clear, handlers stack up across sessions
        // so after N Play/Stop cycles every event fires N times, and the
        // growing handler list makes each emit() progressively slower.
        EventBus::instance().clear_all();
        // Clear debug draw lines — they accumulate every frame scripts call
        // draw_line() and are never flushed otherwise, growing unbounded.
        Debug::clear_lines();
        // If we're in a matchmaking lobby/match, go through Leave() so the
        // lobby is actually exited (lobby_leave broadcast, hosting/connected/
        // in_match flags reset, quickmatch state cleared) rather than just
        // calling Network::Shutdown() directly underneath it. The latter
        // silently destroys the transport while Matchmaking::IsHosting()/
        // IsConnected() keep reporting true, which desyncs the lobby UI from
        // what's actually happening on the wire (further Network::Update()
        // calls just no-op since transport_ready is now false, with nothing
        // telling the user their connection was just dropped).
        if (Matchmaking::IsHosting() || Matchmaking::IsConnected()) {
            Matchmaking::Leave();
        } else {
            Network::Shutdown();
        }
        Replication::Shutdown();
        NetPredict::ClearRemoteBuffers();
        _scene_switch_requested = false;
        _scene_switch_target.clear();
        // Restore snapshot
        if (!st.scene_snapshot.empty()) {
            st.entities = st.scene_snapshot;
            st.scene_snapshot.clear();
            transform::mark_structure_dirty();
        }
        st.log("Preview stopped.");
    }

private:
    struct TilemapGridMetrics {
        float cell_width = 32.f;
        float cell_height = 32.f;
        float stride_x = 32.f;
        float stride_y = 32.f;
    };

    // Mirrors RenderSystem's palette geometry. Mouse-to-cell conversion must
    // use the same cell size as the atlas renderer, not a stale legacy
    // tile_size left on a map before a palette was assigned.
    static TilemapGridMetrics _tilemap_grid_metrics(const EditorState& st, const Entity& tm) {
        TilemapGridMetrics out;
        const int fallback = std::max(1, tm.value("tile_size", 32));
        out.cell_width = (float)fallback;
        out.cell_height = (float)fallback;
        const std::string palette_ref = tm.value("tile_palette", std::string());
        if (!palette_ref.empty()) {
            try {
                std::ifstream input(fs::path(st.asset_dir) / palette_ref);
                nlohmann::json manifest;
                if (input && (input >> manifest) && manifest.is_object() &&
                    manifest.value("format", std::string()) == "gameengine.tile-palette") {
                    out.cell_width = (float)std::max(1, manifest.value("cell_width", fallback));
                    out.cell_height = (float)std::max(1, manifest.value("cell_height", (int)out.cell_width));
                }
            } catch (const std::exception&) {
                // Fall back to the legacy map dimensions for a malformed asset.
            }
        } else {
            out.cell_width = std::max(1.f, tm.value("_grid_cell_width", out.cell_width));
            out.cell_height = std::max(1.f, tm.value("_grid_cell_height", out.cell_height));
        }
        out.stride_x = std::max(1.f, out.cell_width + tm.value("_grid_cell_gap_x", 0.f));
        out.stride_y = std::max(1.f, out.cell_height + tm.value("_grid_cell_gap_y", 0.f));
        return out;
    }

    // Resolves a scene reference (e.g. "scene_boss.json") the same way the
    // standalone build does in core.cpp's resolve_scene_reference(): relative
    // to the *current* scene file's directory, falling back to relative to
    // the project root if that doesn't exist.
    static std::string _resolve_scene_ref(const std::string& raw_scene, const std::string& current_scene) {
        namespace fs = std::filesystem;
        if (raw_scene.empty()) return raw_scene;
        fs::path scene(raw_scene);
        if (scene.is_absolute()) return scene.lexically_normal().generic_string();

        fs::path current(current_scene);
        fs::path base = current.has_parent_path() ? current.parent_path() : fs::path{};
        std::error_code ec;
        if (!base.empty()) {
            fs::path candidate = (base / scene).lexically_normal();
            if (fs::exists(candidate, ec)) return candidate.generic_string();
        }
        if (fs::exists(scene, ec)) return fs::absolute(scene).lexically_normal().generic_string();
        return (base / scene).lexically_normal().generic_string();
    }

    void _apply_scene_switch(EditorState& st) {
        std::string target = _resolve_scene_ref(_scene_switch_target, st.scene_path);
        _scene_switch_requested = false;
        _scene_switch_target.clear();

        namespace fs = std::filesystem;
        if (!fs::exists(target)) {
            st.log_error("SceneManager::LoadScene: scene not found: " + target);
            return;
        }

        try {
            std::ifstream f(target);
            nlohmann::json j; f >> j;
            st.entities.clear();
            if (j.contains("entities") && j["entities"].is_array()) {
                for (const auto& raw : j["entities"]) {
                    if (raw.is_null()) continue;
                    st.entities.push_back(raw);
                }
            }
            st.entities.reserve(std::max<size_t>(st.entities.size() * 4, st.entities.size() + 1024));
        } catch (std::exception& ex) {
            st.log_error(std::string("SceneManager::LoadScene failed: ") + ex.what());
            return;
        }

        // This Play session is now "in" the new scene — update scene_path /
        // asset_dir / script project prefix so relative lookups (textures,
        // further LoadScene calls, project-namespaced scripts) resolve
        // against the new scene's directory instead of the one Play
        // actually started from.
        st.scene_path = target;
        st.update_asset_dir();
        st.clear_selection();
        st.resync_children_arrays();
        transform::mark_structure_dirty();
        ScriptRegistry::instance().set_active_project_from_scene_path(st.scene_path);
        Network::SeedSessionFromString(st.scene_path);
        Network::BindScene(&st.entities);

        // If this scene switch is the one driven by a match starting (see
        // Matchmaking::StartMatch -> _queue_scene_load), spawn every lobby
        // member's player entity now that the new scene is actually live.
        // Doing this here — after the swap, not from StartMatch() itself —
        // is what avoids instantiating into the scene that's about to be
        // replaced (LoadScene's handler only queues the switch; this is
        // where the real swap finishes).
        if (Matchmaking::InMatch()) {
            NetSpawn::SpawnAllPlayers(st.entities, st.asset_dir);
            // Fresh NetId allocation + tracked-entity registry for this
            // match, same moment players are (re)spawned. Must happen
            // before any script's awake()/start() calls Replication::Spawn
            // this frame, and after the scene swap so we're not clearing
            // state out from under the scene that's about to be replaced.
            Replication::Init();
            NetPredict::ClearRemoteBuffers();
            // Register every preplaced scene entity that has a HealthComponent
            // (enemies, boss, etc.) with the replication system so the host
            // assigns them net_ids and broadcasts them to clients.
            // Without this, preplaced enemies have net_id == 0 on every peer:
            // damage hits use the local `else { hp -= dmg; }` path instead of
            // RequestDamage, so kills are never sent over the network — the
            // enemy stays alive on every other player's screen.
            if (Network::IsHost()) {
                for (auto& e : st.entities) {
                    if (!Replication::IsNetworked(e) &&
                        (has_component(e, "HealthComponent") || e.contains("hp"))) {
                        Replication::RegisterExisting(e);
                    }
                }
            }
        }

        // Fresh script instances + cleared contact state for the new scene,
        // same reasoning as start_play(): stale instances/contact pairs from
        // the previous scene must not bleed into this one.
        _script_sys = ScriptSystem();
        _script_sys.set_input(&_input);
        _fixed_update_accumulator = 0.f;
        phys::reset_contact_state();

        // Re-install the handler (the lambda captures `this`, which is still
        // valid, but re-installing keeps this self-contained/explicit rather
        // than relying on the old closure surviving the scene swap).
        SceneManager::SetLoadSceneHandler([this](const std::string& next_scene){
            _scene_switch_requested = true;
            _scene_switch_target = next_scene;
        });

        st.log("Scene loaded (Play): " + target);
    }

private:

    static fs::path _find_project_root() {
        return find_engine_project_root();
    }

    static bool _copy_tree(const fs::path& src, const fs::path& dst) {
        std::error_code ec;
        if (!fs::exists(src, ec)) return false;
        fs::create_directories(dst, ec);
        fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        return !ec;
    }

static std::vector<fs::path> _collect_script_sources(const fs::path& script_dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::exists(script_dir, ec)) return out;
    for (auto& entry : fs::directory_iterator(script_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        auto ext = entry.path().extension().string();
        if ((ext == ".cpp" || ext == ".hpp") && entry.path().filename() != "game_scripts.hpp") {
            out.push_back(entry.path());
        }
    }
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b){
        return a.filename().string() < b.filename().string();
    });
    return out;
}

// Derive a stable per-project identifier from the scene path, mirroring
// ScriptSystem::detail::infer_project_from_scene_path (script_system.hpp) so
// the export folder name and the active script-registry namespace agree.
// (Kept as a thin wrapper for existing call sites in this file — the actual
// logic lives in editor_state.hpp's project_name_from_scene_path() so it's
// reachable at true file scope from editor_main.cpp too; see that function's
// comment for why this needed to move.)
static std::string _project_name_from_scene_path(const fs::path& scene_path) {
    return project_name_from_scene_path(scene_path);
}

// The folder name is the stable internal identity used by CMake and
// hot-reload modules.  Player-facing exports must instead use the authored
// Product Name, while Company Name is retained as shipped metadata.
struct StandaloneExportIdentity {
    std::string project_id;
    std::string company_name = "My Company";
    std::string product_name;
    std::string version = "0.1.0";
    std::string file_stem;
};

static std::string _safe_export_file_stem(const std::string& requested, const std::string& fallback) {
    std::string out;
    bool previous_separator = false;
    for (unsigned char c : requested) {
        if (std::isalnum(c) || c == '_' || c == '-') {
            out.push_back(static_cast<char>(c));
            previous_separator = false;
        } else if (!out.empty() && !previous_separator) {
            out.push_back('_');
            previous_separator = true;
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? fallback : out;
}

static StandaloneExportIdentity _load_export_identity(const fs::path& project_dir,
                                                       const std::string& project_id) {
    StandaloneExportIdentity identity;
    identity.project_id = project_id;
    identity.product_name = project_id;
    std::ifstream input(project_dir / "ProjectSettings" / "ProjectSettings.json");
    if (input) {
        try {
            nlohmann::json settings;
            input >> settings;
            if (settings.is_object()) {
                if (const auto it = settings.find("company_name"); it != settings.end() && it->is_string())
                    identity.company_name = it->get<std::string>();
                if (const auto it = settings.find("product_name"); it != settings.end() && it->is_string() && !it->get<std::string>().empty())
                    identity.product_name = it->get<std::string>();
                const auto number = [&](const char* key, int fallback) {
                    const auto it = settings.find(key);
                    return it != settings.end() && it->is_number_integer() ? std::max(0, it->get<int>()) : fallback;
                };
                identity.version = std::to_string(number("version_major", 0)) + "." +
                                   std::to_string(number("version_minor", 1)) + "." +
                                   std::to_string(number("version_patch", 0));
            }
        } catch (...) {
            // A malformed metadata file must not stop an otherwise valid
            // project from building; retain the stable project-id fallback.
        }
    }
    identity.file_stem = _safe_export_file_stem(identity.product_name, project_id);
    return identity;
}

static void _write_script_header(const fs::path& header_path,
                                 const std::vector<fs::path>& script_files,
                                 bool absolute_includes) {
    // Extract every class that inherits from ScriptBase or MonoBehaviour
    // (via the shared extract_script_class_names() helper). We always emit
    // REGISTER_SCRIPT here — scripts must NOT write it themselves
    // (mirroring Unity's MonoBehaviour: just save the file and the engine
    // picks it up automatically).
    std::error_code ec;
    fs::create_directories(header_path.parent_path(), ec);
    std::ofstream f(header_path);
    f << "#pragma once\n";
    f << "// Auto-generated by the editor.\n";
    f << "// This file registers all C++ scripts for this project.\n\n";
    for (const auto& p : script_files) {
        std::ifstream in(p);
        std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        fs::path inc;
        if (absolute_includes) {
            // Use a path relative to the header file so it works across machines
            std::error_code rel_ec;
            fs::path rel = fs::relative(fs::absolute(p), header_path.parent_path(), rel_ec);
            inc = (!rel_ec && !rel.empty()) ? rel : p.filename();
        } else {
            inc = p.filename();
        }
        f << "#include \"" << inc.generic_string() << "\"\n";

        for (const auto& cls : extract_script_class_names(src)) {
            f << "REGISTER_SCRIPT(" << cls << ");\n";
        }
        f << "\n";
    }
    if (script_files.empty()) {
        f << "// No script sources were found in this project.\n";
    }
}


    static fs::path _find_built_exe(const fs::path& build_dir, const std::string& project_name) {
        return find_built_exe_named(build_dir, project_name);
    }


    void _start_standalone_build(EditorState& st, bool run_after_build) {
        // Both paths compile generated script files.  Refuse an export until a
        // script reload has completed instead of producing a fragile race over
        // generated headers/intermediates.
        for (const auto& [project, state] : _all_script_rebuild_states()) {
            if (state && state->in_progress.load()) {
                st.log_warn("Finish compiling scripts for '" + project + "' before building a standalone game.");
                return;
            }
        }

        auto& build_state = standalone_build_state();
        bool expected = false;
        if (!build_state.in_progress.compare_exchange_strong(expected, true)) {
            st.log_warn("A standalone build is already running. Wait for the build popup to finish.");
            return;
        }
        if (_standalone_build_thread.joinable()) _standalone_build_thread.join();
        // Snapshot all scene/default-scene choices before the worker starts.
        // The modal prevents edits while it is active, and this also avoids a
        // worker reading EditorState while a scene is being switched.
        const fs::path opened_scene = fs::absolute(st.scene_path);
        const std::string project_name = _project_name_from_scene_path(opened_scene);
        const std::string configured_default = st.default_scene_for(project_name);
        build_state.done = false;
        build_state.success = false;
        build_state.generation.fetch_add(1, std::memory_order_relaxed);
        build_state.set_status(project_name, "Preparing export files...");
        st.log_engine("Standalone build started. The editor is locked until it completes.");

        _standalone_build_thread = std::thread([this, &st, run_after_build,
                                                opened_scene, configured_default] {
            bool succeeded = false;
            try {
                succeeded = _build_standalone(st, run_after_build, opened_scene, configured_default);
            } catch (const std::exception& error) {
                st.log_error(std::string("Standalone build stopped: ") + error.what());
                standalone_build_state().set_message(std::string("Unexpected build error: ") + error.what());
            } catch (...) {
                st.log_error("Standalone build stopped by an unknown error.");
                standalone_build_state().set_message("Unexpected unknown build error.");
            }
            auto& completed = standalone_build_state();
            completed.success = succeeded;
            completed.done = true;
            if (succeeded) completed.set_message("Export completed successfully.");
            else if (completed.get_message().empty()) completed.set_message("Export failed. Open Build Report for compiler output.");
            completed.in_progress = false;
        });
    }

    bool _build_standalone(EditorState& st, bool run_after_build,
                           const fs::path& opened_scene,
                           const std::string& configured_default) {
        fs::path root = _find_project_root();
        fs::path engine_dir = root / "engine_cpp";
        fs::path scene_dir    = opened_scene.parent_path();

        // Per-project name (e.g. "game3" from games/game3/scene.json) drives
        // the build dir, the exe filename, and the export folder, so building
        // one project never touches another project's build cache or output —
        // each project gets its own standalone exe, the same way a Unity2D
        // project export doesn't collide with a different project's Build/
        // folder.
        std::string project_name = _project_name_from_scene_path(opened_scene);

        // Unity-style "Build runs whatever scene is set as default for this
        // project, regardless of what's open in the editor". Falls back to
        // the currently open scene if the project hasn't set one yet, so
        // projects with no default configured keep building exactly like
        // before this setting existed.
        const std::string& configured = configured_default;
        fs::path boot_scene = opened_scene;
        if (!configured.empty()) {
            fs::path candidate(configured);
            if (candidate.is_relative()) {
                const fs::path project_relative = scene_dir / candidate;
                std::error_code candidate_ec;
                candidate = fs::exists(project_relative, candidate_ec) ? project_relative : fs::absolute(candidate);
            }
            boot_scene = candidate;
        }

        const StandaloneExportIdentity export_identity = _load_export_identity(scene_dir, project_name);
        fs::path build_dir  = root / "build" / "standalone_export" / project_name;
        // Keep build intermediates project-scoped, but make the user-facing
        // export folder match Product Name instead of the internal project id.
        fs::path export_dir = root / "export" / export_identity.file_stem;

        std::error_code ec;
        if (!fs::exists(boot_scene, ec)) {
            const std::string error = "Default scene not found: " + boot_scene.string();
            standalone_build_state().set_message(error);
            st.log_error(error);
            return false;
        }

        // Build Settings only accepts valid scene files under this project.
        // Exported paths retain their project-relative layout, so scene
        // references remain portable after a project move or export.
        std::vector<fs::path> scenes_to_export;
        const auto add_scene = [&](const fs::path& source) {
            std::error_code local_ec;
            const fs::path absolute = fs::absolute(source, local_ec);
            if (local_ec || !fs::is_regular_file(absolute, local_ec) || absolute.extension() != ".json") return;
            const fs::path relative = fs::relative(absolute, scene_dir, local_ec);
            if (local_ec || relative.empty() || *relative.begin() == fs::path("..")) return;
            for (const auto& existing : scenes_to_export) if (existing == absolute) return;
            scenes_to_export.push_back(absolute);
        };
        {
            std::ifstream input(scene_dir / "ProjectSettings" / "EditorBuildSettings.json");
            nlohmann::json entries;
            if (input) {
                try { input >> entries; } catch (...) { entries = nlohmann::json::array(); }
            }
            if (entries.is_array()) {
                for (const auto& entry : entries) {
                    if (!entry.is_object()) continue;
                    const auto enabled = entry.find("enabled");
                    if (enabled != entry.end() && enabled->is_boolean() && !enabled->get<bool>()) continue;
                    const auto path = entry.find("path");
                    if (path == entry.end() || !path->is_string()) continue;
                    const std::string stored_path = path->get<std::string>();
                    if (stored_path.empty()) continue;
                    fs::path candidate(stored_path);
                    if (candidate.is_relative()) {
                        // Newer settings save paths relative to the project,
                        // while older settings may contain an engine-rooted
                        // games/<project>/ path. Accept both without ever
                        // exporting a scene outside the current project.
                        const fs::path project_relative = scene_dir / candidate;
                        std::error_code candidate_ec;
                        candidate = fs::exists(project_relative, candidate_ec)
                            ? project_relative : fs::absolute(candidate, candidate_ec);
                    }
                    add_scene(candidate);
                }
            }
        }
        // Legacy projects with no saved list remain buildable; the designated
        // startup scene is always shipped even if it was omitted from the UI.
        add_scene(boot_scene);
        if (scenes_to_export.empty()) {
            standalone_build_state().set_message("No valid scenes are enabled for this build.");
            st.log_error("No valid scenes are enabled for this build.");
            return false;
        }
        fs::create_directories(build_dir, ec);
        fs::create_directories(export_dir, ec);

        {
            nlohmann::json metadata = {
                {"format_version", 1},
                {"project_id", export_identity.project_id},
                {"company_name", export_identity.company_name},
                {"product_name", export_identity.product_name},
                {"version", export_identity.version}
            };
            std::ofstream metadata_file(export_dir / "game_metadata.json", std::ios::trunc);
            if (!metadata_file) {
                const std::string error = "Could not write export metadata in " + export_dir.string();
                standalone_build_state().set_message(error);
                st.log_error(error);
                return false;
            }
            metadata_file << metadata.dump(2);
        }

        auto quote = [](const fs::path& p) {
            return std::string("\"") + p.string() + "\"";
        };
        const fs::path output_log = build_dir / "build_output.log";
        { std::ofstream clear_log(output_log, std::ios::trunc); }
        const auto run_captured = [&](const std::string& command) {
            const std::string captured = command + " >> " + quote(output_log) + " 2>&1";
            return std::system(captured.c_str());
        };
#if defined(_WIN32)
        const std::string msvc_x64_env =
            "set \"PROCESSOR_ARCHITECTURE=AMD64\" && set \"PROCESSOR_ARCHITEW6432=AMD64\" && ";
#else
        const std::string msvc_x64_env;
#endif

        auto script_files = _collect_script_sources(scene_dir / "scripts");
        _write_script_header(build_dir / "generated_game_scripts.hpp", script_files, true);
        _write_script_header(export_dir / "scripts" / "game_scripts.hpp", script_files, false);

        std::string configure = msvc_x64_env + "cmake -S " + quote(engine_dir) +
            " -B " + quote(build_dir) +
            " -DBUILD_STANDALONE=ON -DCMAKE_BUILD_TYPE=Release" +
            " -DGAME_PROJECT_NAME=" + quote(project_name);
        standalone_build_state().set_message("Configuring CMake project...");
        st.log_engine("Configuring standalone build for '" + project_name + "'...");
        if (run_captured(configure) != 0) {
            standalone_build_state().set_message("CMake configuration failed. Open Build Report for captured compiler output.");
            st.log_error("Standalone configure failed. See Build Report for the captured compiler output.");
            return false;
        }

        std::string build = msvc_x64_env + "cmake --build " + quote(build_dir);
#if defined(_WIN32)
        build += " --config Release --target " + project_name + " -- /p:PreferredToolArchitecture=x64";
#else
        build += " --target " + project_name + " --parallel";
#endif
        standalone_build_state().set_message("Compiling and linking standalone game...");
        st.log_engine("Building standalone game '" + project_name + "'...");
        if (run_captured(build) != 0) {
            standalone_build_state().set_message("Compilation/linking failed. Open Build Report for captured compiler output.");
            st.log_error("Standalone build failed. See Build Report for the captured compiler output.");
            return false;
        }

        standalone_build_state().set_message("Copying scenes, assets, shaders, and runtime files...");
        fs::create_directories(export_dir, ec);

        // Copy EVERY scene file in the project — not just the one open in
        // the editor — so SceneManager::LoadScene("scene_verdant.json") etc.
        // (portals, menu chapter-select, ...) can actually find their target
        // scenes next to the exe at runtime. Previously only st.scene_path
        // got copied, so a build only ever shipped with one playable scene
        // no matter how many the project actually had.
        int copied = 0;
        for (const auto& source : scenes_to_export) {
            std::error_code relative_ec;
            const fs::path relative = fs::relative(source, scene_dir, relative_ec);
            if (relative_ec) continue;
            fs::create_directories((export_dir / relative).parent_path(), ec);
            fs::copy_file(source, export_dir / relative, fs::copy_options::overwrite_existing, ec);
            ++copied;
        }
        // main.cpp's default boot path is "scene.json" next to the exe — write
        // the configured default/boot scene there too (in addition to its own
        // filename above, which other scenes' SceneManager::LoadScene(...)
        // calls may still reference directly, e.g. a portal pointing back at
        // "scene_home.json" by name).
        fs::copy_file(boot_scene, export_dir / "scene.json", fs::copy_options::overwrite_existing, ec);
        st.log("Copied " + std::to_string(copied) + " scene file(s) into export; boot scene = " +
               boot_scene.filename().string());

        _copy_tree(scene_dir / "assets", export_dir / "assets");
        _copy_tree(scene_dir / "scripts", export_dir / "scripts");
        // UIText's fallback/runtime atlas is an engine asset rather than a
        // project asset.  Export it explicitly so a project without its own
        // assets/fonts directory still has readable UI text in standalone.
        _copy_tree(engine_dir / "assets" / "fonts", export_dir / "assets" / "fonts");

        fs::path exe = _find_built_exe(build_dir, project_name);
        fs::path exported_exe;
        if (!exe.empty()) {
            exported_exe = export_dir / (export_identity.file_stem + exe.extension().string());
            fs::copy_file(exe, exported_exe, fs::copy_options::overwrite_existing, ec);
            // Runtime resources live next to the standalone executable. Copy
            // the complete shader folder, not a hand-picked subset: fullscreen,
            // composite and particle pipelines are loaded on demand and were
            // previously missing from exports even though sprite shaders were
            // present.
            // The target's post-build step historically copied only the two
            // sprite pipelines. Runtime features also load fullscreen,
            // composite and particle SPIR-V files on demand, so seed the
            // export from the complete engine shader directory first, then
            // overlay target-local generated shaders when present.
            _copy_tree(engine_dir / "vk_render" / "shaders", export_dir / "shaders");
            _copy_tree(exe.parent_path() / "shaders", export_dir / "shaders");
            for (const auto& runtime_file : {"SDL2.dll", "editor_symbols.map"}) {
                const fs::path source = exe.parent_path() / runtime_file;
                if (fs::is_regular_file(source, ec))
                    fs::copy_file(source, export_dir / runtime_file, fs::copy_options::overwrite_existing, ec);
            }
        } else {
            st.log_warn("Built exe not found under " + build_dir.string() + " (expected name '" + project_name + "').");
        }

        // editor_symbols.map is generated alongside editor.exe, not the
        // standalone target. It is still useful in a shipped build for
        // symbolicated crash reports, so replace any stale exported copy with
        // the current build's map on every export.
        const fs::path symbols = root / "build" / "editor" / "Release" / "editor_symbols.map";
        ec.clear();
        if (fs::is_regular_file(symbols, ec))
            fs::copy_file(symbols, export_dir / "editor_symbols.map", fs::copy_options::overwrite_existing, ec);

        if (ec) {
            st.log_warn("Standalone build finished with some copy warnings.");
        }
        st.log_success("Standalone '" + export_identity.product_name + "' exported to: " + export_dir.string());
        if (run_after_build && !exported_exe.empty() && fs::is_regular_file(exported_exe, ec)) {
#if defined(_WIN32)
            const std::string launch = "start \"\" \"" + exported_exe.string() + "\"";
            if (std::system(launch.c_str()) != 0)
                st.log_warn("Export completed, but Build & Run could not launch the executable.");
#else
            st.log_warn("Export completed. Build & Run is currently supported on Windows only.");
#endif
        }
        return true;
    }

    void _ensure_texture(int w, int h) {
        if (_backend && _tex_w==w && _tex_h==h && _imgui_ds != VK_NULL_HANDLE) return;
        if (!_backend) { _tex_w = w; _tex_h = h; return; } // called once from the ctor's default size, before init()

        _backend->wait_idle(); // RenderTarget::destroy() must not run while the GPU is still using it
        if (_imgui_ds) { ImGui_ImplVulkan_RemoveTexture(_imgui_ds); _imgui_ds = VK_NULL_HANDLE; }
        _rt.destroy();
        _rt.create(_backend->ctx(), (uint32_t)w, (uint32_t)h);
        _imgui_ds = ImGui_ImplVulkan_AddTexture(/*sampler*/ _viewport_sampler(),
                                                 _rt.image_view(),
                                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        _tex_w=w; _tex_h=h;
    }

    // One linear-filtered sampler shared by every viewport-image binding —
    // created lazily and lives for the process lifetime (no per-resize churn,
    // unlike the RenderTarget image itself which is recreated on resize).
    VkSampler _viewport_sampler() {
        static VkSampler s = VK_NULL_HANDLE;
        if (s == VK_NULL_HANDLE && _backend) {
            VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            sci.magFilter = VK_FILTER_LINEAR;
            sci.minFilter = VK_FILTER_LINEAR;
            sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            vkCreateSampler(_backend->ctx().device, &sci, nullptr, &s);
        }
        return s;
    }


    void _open_rename_dialog(EditorState& st, int eid) {
        if (Entity* e = st.find_entity(eid)) {
            snprintf(_rename_buf, sizeof(_rename_buf), "%s", e->value("name", "Entity").c_str());
            _rename_target = eid;
            ImGui::OpenPopup("##viewport_rename");
        }
    }

    void _draw_rename_dialog(EditorState& st) {
        if (_rename_target < 0) return;
        if (ImGui::BeginPopupModal("##viewport_rename", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Rename Entity");
            ImGui::SetNextItemWidth(240.f);
            ImGui::InputText("##rename_name", _rename_buf, sizeof(_rename_buf));
            if (ImGui::Button("Rename") && _rename_buf[0] != '\0') {
                if (Entity* e = st.find_entity(_rename_target)) {
                    st.undo.push_deep(st.entities);
                    (*e)["name"] = std::string(_rename_buf);
                }
                _rename_target = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                _rename_target = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // ── Tile paint sub-toolbar (shown below the main toolbar in paint mode) ───
    void _draw_paint_toolbar(EditorState& st) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(3,2));

        // ── Erase toggle ─────────────────────────────────────────────────────
        if (st.paint_erase)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f,0.2f,0.2f,1.f));
        else
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f,0.28f,0.28f,1.f));
        if (ImGui::Button(" Erase ")) st.paint_erase = !st.paint_erase;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Erase mode — paints tile -1 (empty).\nShortcut: hold E while painting.");
        ImGui::SameLine(0,4);

        // ── Brush size ───────────────────────────────────────────────────────
        ImGui::TextUnformatted("Brush:");
        ImGui::SameLine(0,3);
        ImGui::SetNextItemWidth(48.f);
        ImGui::InputInt("##bsz", &st.paint_brush_size, 1, 1);
        st.paint_brush_size = std::max(1, std::min(st.paint_brush_size, 16));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Brush size (NxN tiles). Use the +/- arrows or type a number.");
        ImGui::SameLine(0,6);

        // ── Rect-fill mode ───────────────────────────────────────────────────
        if (st.paint_rect_mode)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f,0.49f,0.91f,1.f));
        else
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f,0.28f,0.28f,1.f));
        if (ImGui::Button(" Rect ")) st.paint_rect_mode = !st.paint_rect_mode;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rectangle fill mode.\nClick-drag to fill a rectangular area with the active tile.");
        ImGui::SameLine(0,4);

        // ── Eyedropper ───────────────────────────────────────────────────────
        if (st.paint_eyedropper)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f,0.49f,0.91f,1.f));
        else
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f,0.28f,0.28f,1.f));
        if (ImGui::Button(" \xf0\x9f\x96\x8a Pick ")) st.paint_eyedropper = !st.paint_eyedropper;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Eyedropper — click a tile in the map to pick its id as the active tile.\nAuto-exits after one pick.");
        ImGui::SameLine(0,6);

        // ── Active tile id ───────────────────────────────────────────────────
        ImGui::TextUnformatted("Tile:");
        ImGui::SameLine(0,3);
        ImGui::SetNextItemWidth(52.f);
        ImGui::InputInt("##tid", &st.paint_tile, 1, 1);
        st.paint_tile = std::max(0, st.paint_tile);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Active tile id — the index into the tileset texture that will be painted.");
        ImGui::SameLine(0,10);

        // Legacy scenes retain their one-tile preset strip until migrated. A
        // Tile Palette instead exposes its reusable multi-cell brush here.
        if (st.active_tile_palette.empty()) {
        ImGui::TextDisabled("|"); ImGui::SameLine(0,6);
        ImGui::TextUnformatted("Legacy Presets:");
        ImGui::SameLine(0,4);

        // Show saved presets as clickable buttons
        int to_delete = -1;
        for (int i = 0; i < (int)st.tile_presets.size(); ++i) {
            auto& p = st.tile_presets[i];
            bool active_preset = (st.paint_tile == p.tile_id && !st.paint_erase);
            if (active_preset)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f,0.49f,0.91f,1.f));
            else
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f,0.28f,0.28f,1.f));

            // Left-click = switch to preset tile, Right-click = delete
            char lbl[32]; snprintf(lbl, sizeof(lbl), " %s ", p.name.c_str());
            if (ImGui::Button(lbl)) {
                st.paint_tile  = p.tile_id;
                st.paint_erase = false;
                // A tile index alone is not a complete paint source. Restore
                // the preset's tileset and cell size too, otherwise choosing
                // a preset silently kept painting with the old texture.
                if (Entity* selected = st.selected_entity(); selected &&
                    has_component(*selected, "Tilemap")) {
                    auto& tilemap = (*selected)["components"]["Tilemap"];
                    if (!p.tileset.empty()) tilemap["tileset"] = p.tileset;
                    if (p.tile_size > 0) tilemap["tile_size"] = p.tile_size;
                }
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Tile id: %d\nTileset: %s\nRight-click to remove this preset.",
                                  p.tile_id, p.tileset.empty() ? "current Tile Set" : p.tileset.c_str());
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) to_delete = i;
            ImGui::SameLine(0,2);
        }
        if (to_delete >= 0) st.tile_presets.erase(st.tile_presets.begin() + to_delete);

        // ── Save current tile as new preset ──────────────────────────────────
        if (ImGui::Button(" + ")) ImGui::OpenPopup("##save_preset");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save the current tile id as a named preset so you can switch back to it quickly.");

        if (ImGui::BeginPopup("##save_preset")) {
            ImGui::TextUnformatted("Preset name:");
            static char preset_name_buf[32] = "";
            ImGui::SetNextItemWidth(120.f);
            ImGui::InputText("##pname", preset_name_buf, sizeof(preset_name_buf));
            ImGui::SameLine(0,4);
            bool can_save = (preset_name_buf[0] != '\0');
            ImGui::BeginDisabled(!can_save);
            if (ImGui::Button("Save")) {
                // Overwrite if name already exists, else append
                std::string nm(preset_name_buf);
                auto it = std::find_if(st.tile_presets.begin(), st.tile_presets.end(),
                    [&](const EditorState::TilePreset& p){ return p.name == nm; });
                EditorState::TilePreset saved{nm, st.paint_tile};
                if (Entity* selected = st.selected_entity(); selected &&
                    has_component(*selected, "Tilemap")) {
                    const auto& tilemap = (*selected)["components"]["Tilemap"];
                    saved.tileset = tilemap.value("tileset", std::string());
                    saved.tile_size = std::max(1, tilemap.value("tile_size", 32));
                }
                if (it != st.tile_presets.end()) *it = std::move(saved);
                else st.tile_presets.push_back(std::move(saved));
                preset_name_buf[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }
        } else {
            ImGui::TextDisabled("|"); ImGui::SameLine(0,6);
            const std::string brush_label = st.active_tile_brush.empty()
                ? "Palette tile" : fs::path(st.active_tile_brush).stem().string();
            ImGui::Text("Brush: %s", brush_label.c_str());
            ImGui::SameLine(0, 6);
            if (ImGui::Button("Rotate")) st.paint_brush_rotation = (st.paint_brush_rotation + 90) % 360;
            ImGui::SameLine(0, 3);
            if (ImGui::Button("Flip X")) st.paint_brush_flip_x = !st.paint_brush_flip_x;
            ImGui::SameLine(0, 3);
            if (ImGui::Button("Flip Y")) st.paint_brush_flip_y = !st.paint_brush_flip_y;
        }

        // ── E shortcut: hold E key to temporarily enter erase mode ───────────
        if (ImGui::IsKeyDown(ImGuiKey_E) && !ImGui::GetIO().WantCaptureKeyboard)
            st.paint_erase = true;
        else         if (!ImGui::IsKeyDown(ImGuiKey_E) && st.tool == "paint") {
            // only reset erase if it was set by the key, not the button —
            // we can't distinguish without extra state, so leave button state alone
        }

        ImGui::PopStyleVar();
    }

    void _draw_toolbar(EditorState& st) {
        const ImVec2 btn_sz(26, 0);
        auto icon_button = [&](const char* id, const char* kind, ImU32 icon_col) -> bool {
            bool pressed = ImGui::Button(id, btn_sz);
            ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
            ImVec2 center{(mn.x+mx.x)*0.5f, (mn.y+mx.y)*0.5f};
            toolbar_icons::draw(ImGui::GetWindowDrawList(), center, 14.f, kind, icon_col);
            return pressed;
        };
        // ── Play controls ────────────────────────────────────────────────────
        if (st.playing) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f,0.55f,0.25f,1.f));
        else            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f,0.28f,0.28f,1.f));
        if (icon_button("##play", "play", IM_COL32(235,235,235,255))) { if (!st.playing) start_play(st); }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play (F5)");
        ImGui::SameLine(0,1);

        ImGui::BeginDisabled(!st.playing);
        if (st.paused) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f,0.5f,0.05f,1.f));
        else           ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f,0.28f,0.28f,1.f));
        if (icon_button("##pause", "pause", IM_COL32(235,235,235,255))) st.paused = !st.paused;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pause (F6)");
        ImGui::SameLine(0,1);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f,0.28f,0.28f,1.f));
        if (icon_button("##step", "step", IM_COL32(235,235,235,255))) st.step_once = true;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Step one frame");
        ImGui::SameLine(0,1);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f,0.28f,0.28f,1.f));
        if (icon_button("##stop", "stop", IM_COL32(235,235,235,255))) stop_play(st);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop (Escape)");
        ImGui::EndDisabled();

        ImGui::SameLine(0,4); ImGui::TextDisabled("|"); ImGui::SameLine(0,4);

        // ── Tool buttons ─────────────────────────────────────────────────────
        // Q/W/E/R/T shortcuts shown in tooltips
        struct ToolDef { const char* icon; const char* val; const char* tip; };
        static const ToolDef tools[] = {
            {"select","select","Select (Q)"},
            {nullptr, "move",  "Move (W)"},              // ↔ renders fine on every fallback font — kept as text
            {"rotate","rotate","Rotate (E)"},
            {"scale", "scale", "Scale (R)"},
            {"paint", "paint", "Paint Tile (T)"},
        };
        for (auto& t : tools) {
            bool active = (st.tool == t.val);
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f,0.49f,0.91f,1.f));
            else        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f,0.28f,0.28f,1.f));
            bool pressed;
            if (t.icon) {
                char id[16]; snprintf(id,sizeof(id),"##tool_%s", t.val);
                pressed = icon_button(id, t.icon, IM_COL32(235,235,235,255));
            } else {
                pressed = ImGui::Button(" \xe2\x86\x94 "); // ↔ move — confirmed present in all fallback fonts
            }
            if (pressed) st.tool = t.val;
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t.tip);
            ImGui::SameLine(0,1);
        }

        ImGui::SameLine(0,4); ImGui::TextDisabled("|"); ImGui::SameLine(0,4);

        // ── Scene view toggles ────────────────────────────────────────────────
        {
            bool g = st.show_grid;
            if (g) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f,0.49f,0.91f,0.6f));
            else   ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f,0.28f,0.28f,1.f));
            if (ImGui::Button("Grid")) st.show_grid = !st.show_grid;
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle grid overlay");
            ImGui::SameLine(0,2);
        }
        {
            bool s = st.snap;
            if (s) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f,0.49f,0.91f,0.6f));
            else   ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f,0.28f,0.28f,1.f));
            if (ImGui::Button("Snap")) st.snap = !st.snap;
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Grid snapping");
            ImGui::SameLine(0,2);
        }
        if (ImGui::Button("F")) _frame_selected(st);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Frame selected entity");
        ImGui::SameLine(0,2);
        {
            bool pressed = ImGui::Button("##resetcam", ImVec2(22,0));
            ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
            toolbar_icons::draw(ImGui::GetWindowDrawList(), {(mn.x+mx.x)*0.5f,(mn.y+mx.y)*0.5f}, 13.f, "frame", IM_COL32(200,200,200,255));
            if (pressed) { _cam.x=0;_cam.y=0;_cam.zoom=1.f; }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset camera to origin");
        ImGui::SameLine(0,8);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f,0.55f,0.55f,1.f));
        ImGui::Text("%d%%", (int)std::round(_cam.zoom * 100.f));
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scene zoom (scroll wheel to change)");

        ImGui::SameLine(0,4); ImGui::TextDisabled("|"); ImGui::SameLine(0,4);

        // ── Script rebuild ────────────────────────────────────────────────────
        std::string toolbar_project = project_name_from_scene_path(st.scene_path);
        bool stale = scripts_are_stale(toolbar_project);
        bool rebuilding = script_rebuild_state(toolbar_project).in_progress.load();
        if (stale && !rebuilding) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f,0.55f,0.1f,1.f));
        else                      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f,0.28f,0.28f,1.f));
        ImGui::BeginDisabled(rebuilding);
        const char* rb_label = rebuilding ? "Reloading..." : (stale ? "Reload Scripts *" : "Reload Scripts");
        if (ImGui::Button(rb_label)) {
            script_staleness_tracker(toolbar_project).clear_pending();
            rebuild_scripts_module(toolbar_project);
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            if (stale) ImGui::SetTooltip("Stale scripts in '%s' — click to rebuild (hot-reload, no restart).", toolbar_project.c_str());
            else       ImGui::SetTooltip("Rebuild scripts for '%s'.", toolbar_project.c_str());
        }
        ImGui::SameLine(0,2);
        ImGui::SameLine(0,4); ImGui::TextDisabled("|"); ImGui::SameLine(0,4);

        // ── Game resolution frame ─────────────────────────────────────────
        {
            bool rf = _show_res_frame;
            if (rf) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f,0.49f,0.91f,0.6f));
            else    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f,0.28f,0.28f,1.f));
            if (ImGui::Button("\xe2\x96\xa1")) _show_res_frame = !_show_res_frame;  // □
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle game resolution frame");
            if (_show_res_frame) {
                ImGui::SameLine(0,4);
                ImGui::SetNextItemWidth(48.f);
                ImGui::InputInt("##resw", &_game_res_w, 0, 0);
                _game_res_w = std::max(1, _game_res_w);
                ImGui::SameLine(0,2);
                ImGui::TextUnformatted("\xc3\x97");  // ×
                ImGui::SameLine(0,2);
                ImGui::SetNextItemWidth(48.f);
                ImGui::InputInt("##resh", &_game_res_h, 0, 0);
                _game_res_h = std::max(1, _game_res_h);
            }
        }
    }

    // Drawn as a screen-space ImGui overlay on top of the already-rendered
    // viewport image (see the comment in draw() where this is called) — img_pos
    // is this frame's actual on-screen top-left of that image, so
    // img_pos + (local x,y) gives the correct screen coordinate for each line.
    // Clipped to the image rect so nothing bleeds into the rest of the panel
    // (toolbar, etc.) the way it would if drawn unclipped on the window's
    // shared draw list.
    void _draw_grid(EditorState& st, ImVec2 img_pos) {
        float size = st.grid_size * _cam.zoom;
        if (size < 4.f) return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(img_pos, {img_pos.x + _tex_w, img_pos.y + _tex_h}, true);
        ImU32 grid_col = IM_COL32(60,60,68,200);
        float ox = std::fmod(-_cam.x * _cam.zoom + _tex_w*0.5f, size);
        float oy = std::fmod(-_cam.y * _cam.zoom + _tex_h*0.5f, size);
        for (float x=ox; x<_tex_w; x+=size)
            dl->AddLine({img_pos.x+x, img_pos.y}, {img_pos.x+x, img_pos.y+_tex_h}, grid_col);
        for (float y=oy; y<_tex_h; y+=size)
            dl->AddLine({img_pos.x, img_pos.y+y}, {img_pos.x+_tex_w, img_pos.y+y}, grid_col);
        // Axes
        ImU32 axis_col = IM_COL32(90,90,120,255);
        auto [ax,ay] = _cam.world_to_screen(0,0);
        dl->AddLine({img_pos.x+ax, img_pos.y}, {img_pos.x+ax, img_pos.y+_tex_h}, axis_col);
        dl->AddLine({img_pos.x, img_pos.y+ay}, {img_pos.x+_tex_w, img_pos.y+ay}, axis_col);
        dl->PopClipRect();
    }

    // ── Resolution frame ──────────────────────────────────────────────────────
    // Draws a thin, fixed-position border in screen space representing the
    // game's target resolution, centred in the viewport — exactly like
    // Unity 2D's grey resolution frame.  The rect is computed from the
    // camera's world_to_screen() so it moves with pan/zoom in world space
    // (the game-world origin is always the centre of the game window), but
    // from the user's perspective the frame feels "fixed": it stays at the
    // same world-space position regardless of how you navigate the viewport.
    //
    // A two-pass draw (slightly thicker dark outline + bright inner line)
    // keeps it readable on both dark and light scene backgrounds.
    void _draw_resolution_frame(ImVec2 img_pos) {
        if (!_show_res_frame) return;

        // The game window is centred on world origin (0,0); its half-extents
        // in world units equal half the pixel dimensions (assuming 1 px = 1
        // world unit at zoom 1, which matches the engine's sprite size convention).
        float hw = _game_res_w * 0.5f;
        float hh = _game_res_h * 0.5f;

        // World corners → screen space
        auto [sx0, sy0] = _cam.world_to_screen(-hw, -hh);  // top-left
        auto [sx1, sy1] = _cam.world_to_screen( hw,  hh);  // bottom-right

        // Screen coords relative to the viewport image top-left
        ImVec2 tl = { img_pos.x + sx0, img_pos.y + sy0 };
        ImVec2 br = { img_pos.x + sx1, img_pos.y + sy1 };

        ImDrawList* dl = ImGui::GetWindowDrawList();
        // Clip to the viewport image so the frame never bleeds into the toolbar
        dl->PushClipRect(img_pos, {img_pos.x + _tex_w, img_pos.y + _tex_h}, true);

        // Dark outer shadow (1 px expand) — improves contrast on bright scenes
        ImU32 shadow_col = IM_COL32(0, 0, 0, 140);
        dl->AddRect({tl.x-1.f, tl.y-1.f}, {br.x+1.f, br.y+1.f}, shadow_col, 0.f, 0, 1.f);

        // Main frame line — light grey, 1 px, no fill, same style as Unity
        ImU32 frame_col = IM_COL32(200, 200, 200, 210);
        dl->AddRect(tl, br, frame_col, 0.f, 0, 1.f);

        dl->PopClipRect();
    }

    // Returns the entity's positioned/sized UI component (UIPanel/UIButton/
    // UIImage/UIProgressBar/UIText), or nullptr if it has none. UICanvas is
    // deliberately excluded — it has no anchor/pos/size fields of its own.
    Entity* _ui_component(Entity& e) {
        static const char* kUiTypes[] = {
            "UIPanel","UIButton","UIImage","UIProgressBar","UIText"
        };
        for (const char* name : kUiTypes)
            if (has_component(e, name)) return &get_component(e, name);
        return nullptr;
    }

    // Screen-space rect a UI component resolves to. Mirrors
    // RenderSystem::draw_ui()'s `resolve()` lambda exactly (anchor fraction of
    // the viewport + pixel offset, minus pivot * size) so the gizmo lines up
    // with what's actually drawn. UI is screen-space and ignores camera
    // pan/zoom entirely, same as the real renderer.
    struct UIScreenRect { int x,y,w,h; bool has_size; };
    UIScreenRect _ui_screen_rect(Entity& c) {
        float ax=c.value("anchor_x",0.5f), ay=c.value("anchor_y",0.5f);
        float px=c.value("pivot_x",0.5f), py=c.value("pivot_y",0.5f);
        bool has_size = c.contains("width") && c.contains("height");
        float scale = 1.0f;
        if (c.value("responsive", false)) {
            const float rw = (float)std::max(1, c.value("reference_width", 1280));
            const float rh = (float)std::max(1, c.value("reference_height", 720));
            const float raw = std::min((float)_cam.width / rw, (float)_cam.height / rh);
            if (c.value("responsive_fit", false)) {
                // Match RenderSystem::draw_ui(): full-screen game overlays
                // are allowed to shrink below their readability floor in a
                // narrow embedded viewport so their editor gizmo remains
                // exactly aligned with the rendered UI.
                scale = std::max(0.10f, std::min(c.value("max_scale", 1.5f), raw));
            } else {
                scale = std::max(c.value("min_scale", 0.55f),
                                 std::min(c.value("max_scale", 1.5f), raw));
            }
        }
        int w = std::max(1, (int)std::lround(c.value("width", 200) * scale));
        int h = std::max(1, (int)std::lround(c.value("height", 40) * scale));
        int x = (int)std::lround(ax*_cam.width  + c.value("pos_x",0.f) * scale - px*w);
        int y = (int)std::lround(ay*_cam.height + c.value("pos_y",0.f) * scale - py*h);
        return {x,y,w,h,has_size};
    }

    // Same screen-space-overlay approach as _draw_grid above — img_pos is
    // this frame's actual on-screen top-left of the viewport image; every
    // local (x,y) gizmo coordinate below is offset by it before drawing.
    void _draw_gizmos(EditorState& st, ImVec2 img_pos) {
        if (st.selected_ids.empty()) return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(img_pos, {img_pos.x + _tex_w, img_pos.y + _tex_h}, true);
        auto P = [&](float x, float y) -> ImVec2 { return {img_pos.x + x, img_pos.y + y}; };

        // Highlight every selected entity with a thin box (multi-select feedback)
        for (int id : st.selected_ids) {
            Entity* e = st.find_entity(id);
            if (!e) continue;

            if (Entity* ui = _ui_component(*e)) {
                auto rect = _ui_screen_rect(*ui);
                bool is_primary = (id == st.selected_id);
                ImU32 col = is_primary ? IM_COL32(255,220,80,255) : IM_COL32(100,180,255,255);
                dl->AddRect(P(rect.x-2, rect.y-2), P(rect.x-2+rect.w+4, rect.y-2+rect.h+4), col);
                continue;
            }

            if (!has_component(*e,"Transform")) continue;
            auto wt = transform::cached_world(*e);
            auto [px,py] = _cam.world_to_screen(wt.x, wt.y);

            int w=32, h=32;
            if (has_component(*e,"BoxCollider2D")) {
                auto& b = (*e)["components"]["BoxCollider2D"];
                w=(int)(b.value("width",32.f)*_cam.zoom);
                h=(int)(b.value("height",32.f)*_cam.zoom);
            } else {
                w=h=(int)(32*_cam.zoom);
            }
            bool is_primary = (id == st.selected_id);
            ImU32 col = is_primary ? IM_COL32(255,220,80,255) : IM_COL32(100,180,255,255);
            float rx=(int)px-w/2-2, ry=(int)py-h/2-2;
            dl->AddRect(P(rx, ry), P(rx+w+4, ry+h+4), col);
        }

        Entity* sel = st.selected_entity();
        if (!sel || !has_component(*sel,"Transform")) { dl->PopClipRect(); return; }
        auto wt = transform::cached_world(*sel);
        float wx=wt.x, wy=wt.y;
        auto [sx,sy] = _cam.world_to_screen(wx,wy);
        int ax=(int)sx, ay=(int)sy;

        // Origin cross
        ImU32 origin_col = IM_COL32(255,200,0,255);
        dl->AddLine(P(ax-10,ay), P(ax+10,ay), origin_col);
        dl->AddLine(P(ax,ay-10), P(ax,ay+10), origin_col);

        if (st.tool=="move") {
            int len = (int)(40 * std::pow(_cam.zoom, 0.3f));
            // X axis (red) with arrowhead
            ImU32 xcol = IM_COL32(255,60,60,255);
            dl->AddLine(P(ax,ay), P(ax+len,ay), xcol);
            dl->AddLine(P(ax+len,ay), P(ax+len-8,ay-5), xcol);
            dl->AddLine(P(ax+len,ay), P(ax+len-8,ay+5), xcol);
            // Y axis (green) with arrowhead
            ImU32 ycol = IM_COL32(60,220,60,255);
            dl->AddLine(P(ax,ay), P(ax,ay+len), ycol);
            dl->AddLine(P(ax,ay+len), P(ax-5,ay+len-8), ycol);
            dl->AddLine(P(ax,ay+len), P(ax+5,ay+len-8), ycol);
        }
        else if (st.tool=="rotate") {
            int radius = (int)(40 * std::pow(_cam.zoom, 0.3f));
            ImU32 rcol = IM_COL32(255,120,255,255);
            for (int ang=0;ang<360;ang+=4) {
                float a1=ang*3.14159f/180.f, a2=(ang+4)*3.14159f/180.f;
                dl->AddLine(
                    P(ax+(int)(std::cos(a1)*radius), ay+(int)(std::sin(a1)*radius)),
                    P(ax+(int)(std::cos(a2)*radius), ay+(int)(std::sin(a2)*radius)), rcol);
            }
            // Handle at current rotation angle
            float rot_rad = Mathf::deg2rad(wt.rotation);
            int hx = ax + (int)(radius*std::cos(rot_rad));
            int hy = ay + (int)(radius*std::sin(rot_rad));
            dl->AddRectFilled(P(hx-4,hy-4), P(hx+4,hy+4), rcol);
        }
        else if (st.tool=="scale") {
            int size = (int)(40 * std::pow(_cam.zoom, 0.3f));
            ImU32 scol = IM_COL32(255,255,100,255);
            dl->AddLine(P(ax,ay), P(ax+size,ay), scol);
            dl->AddRectFilled(P(ax+size-5,ay-5), P(ax+size-5+10,ay-5+10), scol);
            dl->AddLine(P(ax,ay), P(ax,ay+size), scol);
            dl->AddRectFilled(P(ax-5,ay+size-5), P(ax-5+10,ay+size-5+10), scol);
        }

        // Box collider outline
        if (has_component(*sel,"BoxCollider2D")) {
            auto& box = (*sel)["components"]["BoxCollider2D"];
            float bw=box.value("width",32.f)*_cam.zoom;
            float bh=box.value("height",32.f)*_cam.zoom;
            float ox=box.value("offset_x",0.f), oy=box.value("offset_y",0.f);
            auto [cx,cy]=_cam.world_to_screen(wx+ox,wy+oy);
            ImU32 bcol = IM_COL32(0,255,100,200);
            dl->AddRect(P(cx-bw/2, cy-bh/2), P(cx-bw/2+bw, cy-bh/2+bh), bcol);
        }

        // Circle collider outline
        if (has_component(*sel,"CircleCollider2D")) {
            auto& circ = (*sel)["components"]["CircleCollider2D"];
            float cr=circ.value("radius",16.f)*_cam.zoom;
            float ox=circ.value("offset_x",0.f), oy=circ.value("offset_y",0.f);
            auto [cx2,cy2]=_cam.world_to_screen(wx+ox,wy+oy);
            ImU32 ccol = IM_COL32(0,255,100,200);
            int ir=(int)cr, icx=(int)cx2, icy=(int)cy2;
            for (int ang=0;ang<360;ang+=4) {
                float a1=ang*3.14159f/180.f, a2=(ang+4)*3.14159f/180.f;
                dl->AddLine(
                    P(icx+(int)(std::cos(a1)*ir),icy+(int)(std::sin(a1)*ir)),
                    P(icx+(int)(std::cos(a2)*ir),icy+(int)(std::sin(a2)*ir)), ccol);
            }
        }

        // Camera2D frustum outline — draws the rotated rectangle when angle != 0
        if (has_component(*sel,"Camera2D")) {
            auto& cam2d = (*sel)["components"]["Camera2D"];
            float size = cam2d.value("orthographic_size",5.f)*100.f;
            float fw = size*1.78f, fh = size; // half-extents in world units
            float angle_deg = cam2d.value("angle", 0.f);
            float angle_rad = angle_deg * (3.14159265f / 180.f);
            ImU32 fcol = IM_COL32(255,255,100,255);

            if (std::abs(angle_rad) < 0.001f) {
                // Axis-aligned: fast path, same as before
                auto [tlx,tly] = _cam.world_to_screen(wx - fw, wy - fh);
                auto [brx,bry] = _cam.world_to_screen(wx + fw, wy + fh);
                dl->AddRect(P(tlx, tly), P(brx, bry), fcol, 0.f, 0, 1.5f);
            } else {
                // Rotated rect: compute four corners in world space, map each to screen
                float ca = std::cos(angle_rad), sa = std::sin(angle_rad);
                // Local half-extents rotated by camera angle to world offsets
                float corners_wx[4] = {
                    wx + (-fw)*ca - (-fh)*sa,  wx + ( fw)*ca - (-fh)*sa,
                    wx + ( fw)*ca - ( fh)*sa,  wx + (-fw)*ca - ( fh)*sa
                };
                float corners_wy[4] = {
                    wy + (-fw)*sa + (-fh)*ca,  wy + ( fw)*sa + (-fh)*ca,
                    wy + ( fw)*sa + ( fh)*ca,  wy + (-fw)*sa + ( fh)*ca
                };
                ImVec2 pts[4];
                for (int ci = 0; ci < 4; ++ci) {
                    auto [scx, scy] = _cam.world_to_screen(corners_wx[ci], corners_wy[ci]);
                    pts[ci] = P(scx, scy);
                }
                dl->AddQuad(pts[0], pts[1], pts[2], pts[3], fcol, 1.5f);
            }
        }

        // Tilemap collider outlines — one green box per tile that the physics
        // system would generate a collider for, so "generate colliders" gives
        // immediate visual feedback instead of only affecting invisible physics.
        // Mirrors collect_shapes() in physics.cpp exactly (same col-origin math)
        // so the outline always matches what actually collides at runtime.
        if (has_component(*sel,"Tilemap")) {
            auto& tm = (*sel)["components"]["Tilemap"];
            if (tm.value("generate_colliders", false)) {
                const TilemapGridMetrics metrics = _tilemap_grid_metrics(st, tm);
                const float tile_width = metrics.stride_x;
                const float tile_height = metrics.stride_y;
                int origin_x = tm.value("origin_x", 0);
                int origin_y = tm.value("origin_y", 0);
                auto& grid = tm["grid"];

                std::unordered_set<int> col_set;
                if (tm.contains("collider_tile_ids")) {
                    for (auto& id : tm["collider_tile_ids"])
                        if (id.is_number()) col_set.insert(id.get<int>());
                }

                ImU32 tcol = IM_COL32(0,255,100,200);
                for (int row = 0; row < (int)grid.size(); ++row) {
                    for (int col = 0; col < (int)grid[row].size(); ++col) {
                        if (grid[row][col].is_null() || !grid[row][col].is_number()) continue;
                        int tid = grid[row][col].get<int>();
                        if (tid < 0) continue;
                        if (!col_set.empty() && !col_set.count(tid)) continue;

                        float wx2 = wx + (col + origin_x) * tile_width + metrics.cell_width * 0.5f;
                        float wy2 = wy + (row + origin_y) * tile_height + metrics.cell_height * 0.5f;
                        auto [tcx,tcy] = _cam.world_to_screen(wx2, wy2);
                        const float half_w = metrics.cell_width * 0.5f * _cam.zoom;
                        const float half_h = metrics.cell_height * 0.5f * _cam.zoom;
                        dl->AddRect(P(tcx-half_w, tcy-half_h), P(tcx+half_w, tcy+half_h), tcol);
                    }
                }
            }
        }

        // Light2D radius ring — faint yellow circle showing light extent
        if (has_component(*sel,"Light2D")) {
            auto& l = (*sel)["components"]["Light2D"];
            if (l.value("enabled",true)) {
                float r = l.value("radius",200.f) * _cam.zoom;
                ImU32 lcol = IM_COL32(255,230,80,80);
                dl->AddCircle(P(ax,ay), r, lcol, 64, 1.5f);
                // Intensity tick at top
                dl->AddLine(P(ax, ay-(int)r), P(ax, ay-(int)r-8), IM_COL32(255,230,80,180));
            }
        }

        // ConstantForce2D direction arrow — shows world + relative force visually
        if (has_component(*sel,"ConstantForce2D")) {
            auto& cf = (*sel)["components"]["ConstantForce2D"];
            float fx = cf.value("force_x",0.f), fy = cf.value("force_y",0.f);
            // Relative force: rotate by entity angle
            float rfx = cf.value("relative_force_x",0.f), rfy = cf.value("relative_force_y",0.f);
            float ang = wt.rotation * 3.14159f / 180.f;
            fx += rfx*std::cos(ang) - rfy*std::sin(ang);
            fy += rfx*std::sin(ang) + rfy*std::cos(ang);
            float mag = std::sqrt(fx*fx+fy*fy);
            if (mag > 0.001f) {
                float nx = fx/mag, ny = fy/mag;
                float len = std::min(60.f, 20.f + mag * 0.08f) * (float)std::pow(_cam.zoom,0.3f);
                int ex = ax + (int)(nx*len), ey = ay + (int)(ny*len);
                ImU32 fcol = IM_COL32(255,140,0,220);
                dl->AddLine(P(ax,ay), P(ex,ey), fcol, 2.f);
                // Arrowhead
                float px2 = -ny*6.f, py2 = nx*6.f;
                dl->AddTriangleFilled(P(ex,ey), P(ex-(int)(nx*10+px2),ey-(int)(ny*10+py2)),
                                               P(ex-(int)(nx*10-px2),ey-(int)(ny*10-py2)), fcol);
            }
        }

        // Waypoint2D path lines — draw line between ordered waypoints
        if (has_component(*sel,"Waypoint2D")) {
            auto& wp = (*sel)["components"]["Waypoint2D"];
            auto& path = wp["path"];
            if (path.is_array() && path.size() > 0) {
                ImU32 wpcol = IM_COL32(0,255,150,180);
                float prev_sx = (float)ax, prev_sy = (float)ay;
                for (auto& wid : path) {
                    int weid = wid.get<int>();
                    Entity* we = st.find_entity(weid);
                    if (!we || !has_component(*we,"Transform")) continue;
                    auto [wpx,wpy] = _cam.world_to_screen(transform::world_x(*we), transform::world_y(*we));
                    dl->AddLine(P(prev_sx,prev_sy), P(wpx,wpy), wpcol, 1.5f);
                    dl->AddCircleFilled(P(wpx,wpy), 4.f, wpcol);
                    prev_sx = wpx; prev_sy = wpy;
                }
                // Close loop
                if (wp.value("loop",true) && path.size() > 1) {
                    dl->AddLine(P(prev_sx,prev_sy), P((float)ax,(float)ay), IM_COL32(0,255,150,80), 1.f);
                }
            }
        }

        dl->PopClipRect();
    }

    void _draw_rect_select_overlay(ImVec2 img_pos) {
        if (!(_drag_left && _drag_mode == "rect_select")) return;
        float x1=_rect_select_start.x, y1=_rect_select_start.y;
        float x2=_last_local_mouse.x,  y2=_last_local_mouse.y;
        float rx=std::min(x1,x2), ry=std::min(y1,y2);
        float rw=std::abs(x2-x1), rh=std::abs(y2-y1);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 a{img_pos.x+rx, img_pos.y+ry}, b{img_pos.x+rx+rw, img_pos.y+ry+rh};
        // ImDrawList colors carry their own alpha, so this is a direct port of
        // the old SDL_BLENDMODE_BLEND fill+outline pair — no explicit blend
        // mode setup needed the way SDL_Renderer required.
        dl->AddRectFilled(a, b, IM_COL32(100,160,255,60));
        dl->AddRect(a, b, IM_COL32(100,160,255,255));
    }

    void _frame_selected(EditorState& st) {
        Entity* sel = st.selected_entity();
        if (sel && has_component(*sel,"Transform")) {
            auto wt = transform::cached_world(*sel);
            _cam.x = wt.x;
            _cam.y = wt.y;
        } else {
            _cam.x=0; _cam.y=0;
        }
    }

    void _handle_input(EditorState& st, ImVec2 img_pos, bool hovered, float dt) {
        ImGuiIO& io = ImGui::GetIO();

        // During Play mode, the viewport should only display the running game.
        // Editing gestures, tool hotkeys, and gizmo manipulation are disabled so
        // game input (WASD, arrows, etc.) does not fight the editor.
        if (st.playing) {
            _drag_pan = false;
            _drag_left = false;
            _drag_mode.clear();
            return;
        }

        if (!hovered && !_drag_pan && !_drag_left) return;

        ImVec2 mp = io.MousePos;
        float lx = mp.x - img_pos.x;
        float ly = mp.y - img_pos.y;

        // ── Zoom on wheel ────────────────────────────────────────────────────
        if (hovered && io.MouseWheel!=0) {
            _cam.zoom_to_point(lx, ly, io.MouseWheel * 0.1f);
        }

        // ── Pan on middle mouse drag ─────────────────────────────────────────
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            if (!_drag_pan) { _drag_pan=true; _prev_mouse=mp; }
            else {
                float dx=mp.x-_prev_mouse.x, dy=mp.y-_prev_mouse.y;
                _cam.x -= dx/_cam.zoom;
                _cam.y -= dy/_cam.zoom;
            }
            _prev_mouse = mp;
        } else {
            _drag_pan = false;
        }

        // ── Left mouse: click (pick/paint) or drag (move/rotate/scale/rect-select) ─
        bool left_down    = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        bool left_clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

        std::vector<Entity*> targets = _selected_entities(st);
        bool transform_tool = (st.tool=="move" || st.tool=="rotate" || st.tool=="scale");

        if (left_down && transform_tool && !targets.empty()) {
            if (!_drag_left) {
                // Drag just started: snapshot starting transform of every selected entity
                _drag_left = true;
                _drag_mode = st.tool;
                _prev_drag_left = mp;
                st.undo.push_deep(st.entities);   // one undo step per drag, not per pixel
                _drag_start_values.clear();
                _ui_drag_start_values.clear();
                for (auto* e : targets) {
                    // Snapshot UI screen-space fields for UI entities
                    if (Entity* ui = _ui_component(*e)) {
                        _ui_drag_start_values[e->value("id",0)] = {
                            ui->value("pos_x",0.f), ui->value("pos_y",0.f),
                            (float)ui->value("width",200), (float)ui->value("height",40)
                        };
                        continue;
                    }
                    if (!has_component(*e,"Transform")) continue;
                    auto& t = (*e)["components"]["Transform"];
                    // x/y here are snapshotted in WORLD space for the move
                    // tool (so dragging always tracks the mouse 1:1 in screen
                    // space even for a parented/rotated/scaled entity);
                    // rotation/scale stay local, matching Unity's Inspector.
                    auto wt = transform::cached_world(*e);
                    _drag_start_values[e->value("id",0)] = {
                        wt.x, wt.y,
                        t.value("rotation",0.f), t.value("scale_x",1.f), t.value("scale_y",1.f)
                    };
                }
            } else {
                float dx = mp.x - _prev_drag_left.x;
                float dy = mp.y - _prev_drag_left.y;

                // ── UI entity drag (move / scale in screen-space) ────────────
                for (auto* e : targets) {
                    Entity* ui = _ui_component(*e);
                    if (!ui) continue;
                    int eid = e->value("id",0);
                    auto uit = _ui_drag_start_values.find(eid);
                    if (uit == _ui_drag_start_values.end()) continue;

                    if (_drag_mode == "move") {
                        float nx = uit->second.pos_x + dx;
                        float ny = uit->second.pos_y + dy;
                        if (st.snap && st.grid_size > 0) {
                            nx = std::round(nx / st.grid_size) * st.grid_size;
                            ny = std::round(ny / st.grid_size) * st.grid_size;
                        }
                        (*ui)["pos_x"] = nx;
                        (*ui)["pos_y"] = ny;
                        uit->second.pos_x = nx;
                        uit->second.pos_y = ny;
                    } else if (_drag_mode == "scale") {
                        float nw = std::max(4.f, uit->second.width  + dx);
                        float nh = std::max(4.f, uit->second.height + dy);
                        if (st.snap && st.grid_size > 0) {
                            nw = std::round(nw / st.grid_size) * st.grid_size;
                            nh = std::round(nh / st.grid_size) * st.grid_size;
                        }
                        // Capture old values before overwriting snapshot
                        float old_w = std::max(1.f, (float)(*ui).value("width", 200));
                        int   old_fs = (*ui).value("font_size", 16);
                        (*ui)["width"]  = (int)nw;
                        (*ui)["height"] = (int)nh;
                        uit->second.width  = nw;
                        uit->second.height = nh;
                        // For UIText, grow/shrink font_size proportionally so text
                        // actually fills the resized bounds rather than just clipping.
                        if (has_component(*e, "UIText")) {
                            int new_fs = std::max(6, (int)std::round(old_fs * (nw / old_w)));
                            (*ui)["font_size"] = new_fs;
                        }
                    } else if (_drag_mode == "rotate") {
                        // Horizontal drag → degrees, stored as "rotation" on the UI component.
                        float cur = (*ui).value("rotation", 0.f);
                        float nr  = std::fmod(cur + dx * 0.5f, 360.f);
                        if (nr < 0) nr += 360.f;
                        if (st.snap) nr = std::round(nr / 15.f) * 15.f;
                        (*ui)["rotation"] = nr;
                    }
                }

                if (_drag_mode == "move") {
                    for (auto* e : targets) {
                        if (_ui_component(*e)) continue;  // handled above
                        if (!has_component(*e,"Transform")) continue;
                        auto it = _drag_start_values.find(e->value("id",0));
                        auto wt = transform::cached_world(*e);
                        float base_wx = it!=_drag_start_values.end() ? it->second.x : wt.x;
                        float base_wy = it!=_drag_start_values.end() ? it->second.y : wt.y;
                        float nx = base_wx + dx/_cam.zoom;
                        float ny = base_wy + dy/_cam.zoom;
                        if (st.snap && st.grid_size>0) {
                            nx = std::round(nx/st.grid_size)*st.grid_size;
                            ny = std::round(ny/st.grid_size)*st.grid_size;
                        }
                        _set_world_xy(st, *e, nx, ny);
                        if (it!=_drag_start_values.end()) { it->second.x = nx; it->second.y = ny; }
                    }
                } else if (_drag_mode == "rotate") {
                    // Horizontal drag distance maps to degrees, matching Python's dx*0.5.
                    // Rotation is edited in LOCAL space (Unity's Inspector also shows
                    // and edits localRotation for a parented object).
                    for (auto* e : targets) {
                        if (_ui_component(*e)) continue;  // UI rotation handled above
                        if (!has_component(*e,"Transform")) continue;
                        auto& t = (*e)["components"]["Transform"];
                        float r = std::fmod(t.value("rotation",0.f) + dx*0.5f, 360.f);
                        if (r < 0) r += 360.f;
                        if (st.snap) r = std::round(r/15.f)*15.f;
                        t["rotation"]=r;
                        transform::mark_local_dirty(e->value("id",0));
                    }
                } else if (_drag_mode == "scale") {
                    float factor = 1.f + (dx - dy) * 0.005f;
                    factor = std::max(0.05f, factor);
                    for (auto* e : targets) {
                        if (_ui_component(*e)) continue;  // UI scale handled above
                        if (!has_component(*e,"Transform")) continue;
                        auto& t = (*e)["components"]["Transform"];
                        t["scale_x"] = std::max(0.01f, t.value("scale_x",1.f)*factor);
                        t["scale_y"] = std::max(0.01f, t.value("scale_y",1.f)*factor);
                        transform::mark_local_dirty(e->value("id",0));
                    }
                }
                _prev_drag_left = mp;
            }
        }
        else if (left_down && st.tool=="paint" && hovered) {
            auto [wx,wy] = _cam.screen_to_world(lx,ly);
            if (st.paint_rect_mode) {
                // Rect-fill: on first press record start tile; hold to preview, release to fill
                Entity* sel = st.selected_entity();
                if (sel && has_component(*sel,"Tilemap") && has_component(*sel,"Transform")) {
                    auto& tm = (*sel)["components"]["Tilemap"];
                    const TilemapGridMetrics metrics = _tilemap_grid_metrics(st, tm);
                    auto wt = transform::cached_world(*sel);
                    int col = (int)std::floor((wx - wt.x) / metrics.stride_x);
                    int row = (int)std::floor((wy - wt.y) / metrics.stride_y);
                    if (!st._tile_rect_dragging) {
                        st._tile_rect_dragging = true;
                        st._tile_rect_col0 = col;
                        st._tile_rect_row0 = row;
                    }
                    // Live preview: store current end in transient fields
                    st._tile_rect_dragging = true;
                    // end coords stored implicitly via mouse position; fill on release below
                }
            } else if (!left_clicked) {
                // The initial click is handled exactly once below. Calling
                // paint from both paths used to make Pick Tile select a tile
                // and then stamp it immediately on that same mouse click.
                _paint_tile(st, wx, wy);
            }
        }
        else if (!left_down && st._tile_rect_dragging && st.tool=="paint") {
            // Mouse released: commit the rect fill
            auto [wx,wy] = _cam.screen_to_world(lx,ly);
            Entity* sel = st.selected_entity();
            if (sel && has_component(*sel,"Tilemap") && has_component(*sel,"Transform")) {
                auto& tm = (*sel)["components"]["Tilemap"];
                const TilemapGridMetrics metrics = _tilemap_grid_metrics(st, tm);
                auto wt = transform::cached_world(*sel);
                int col1 = (int)std::floor((wx - wt.x) / metrics.stride_x);
                int row1 = (int)std::floor((wy - wt.y) / metrics.stride_y);
                _paint_tile_rect(st, st._tile_rect_col0, st._tile_rect_row0, col1, row1);
            }
            st._tile_rect_dragging = false;
        }
        else if (left_down && st.tool=="select" && hovered) {
            if (!_drag_left) {
                _drag_left = true;
                _drag_mode = "rect_select";
                _rect_select_start = {lx,ly};
                _prev_drag_left = mp;
            }
        }
        else {
            // Mouse released or tool/condition no longer applies — finish any drag
            if (_drag_left && _drag_mode == "rect_select") {
                _do_rect_select(st, _rect_select_start, {lx,ly});
            }
            _drag_left = false;
            _drag_mode.clear();
        }

        // ── Plain click (no drag): pick or paint ────────────────────────────────
        if (left_clicked && !transform_tool) {
            auto [wx,wy] = _cam.screen_to_world(lx,ly);
            if (st.tool=="select") {
                bool additive = io.KeyCtrl || io.KeyShift;
                _pick(st, wx, wy, additive, lx, ly);
            } else if (st.tool=="paint" && !st.paint_rect_mode) {
                _paint_tile(st, wx, wy);
            }
        }

        // ── Arrow-key nudge (single or multi-select) ────────────────────────────
        if (hovered) {
            float step = (st.snap && st.grid_size>0) ? (float)st.grid_size : 1.f;
            for (auto* e : targets) {
                if (!has_component(*e,"Transform")) continue;
                auto wt = transform::cached_world(*e);
                float wx = wt.x, wy = wt.y;
                bool moved = false;
                if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))  { wx -= step; moved=true; }
                if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) { wx += step; moved=true; }
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))    { wy -= step; moved=true; }
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))  { wy += step; moved=true; }
                if (moved) _set_world_xy(st, *e, wx, wy);
            }

            // ── Paint-mode shortcuts ─────────────────────────────────────────
            if (st.tool == "paint" && !ImGui::GetIO().WantCaptureKeyboard) {
                // [ = decrease brush size,  ] = increase brush size
                if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket))
                    st.paint_brush_size = std::max(1, st.paint_brush_size - 1);
                if (ImGui::IsKeyPressed(ImGuiKey_RightBracket))
                    st.paint_brush_size = std::min(16, st.paint_brush_size + 1);
                // Hold E = erase; release = back to paint
                // (Toggleing via the button is separate — this only fires while hovering the viewport)
                if (ImGui::IsKeyDown(ImGuiKey_E))       st.paint_erase = true;
                else if (!ImGui::IsKeyDown(ImGuiKey_E)) st.paint_erase = false;
                // F = eyedropper (pick once then auto-exit)
                if (ImGui::IsKeyPressed(ImGuiKey_F)) st.paint_eyedropper = !st.paint_eyedropper;
            }
        }
    }

    std::vector<Entity*> _selected_entities(EditorState& st) {
        std::vector<Entity*> out;
        for (int id : st.selected_ids) {
            Entity* e = st.find_entity(id);
            if (e) out.push_back(e);
        }
        return out;
    }

    // Writes Transform.x/y such that the entity's WORLD position becomes
    // (wx, wy), converting through the parent's world transform if any.
    // Used by gizmo/mouse dragging so moving a parented child in the
    // viewport tracks the mouse 1:1 regardless of the parent's own
    // position/rotation/scale.
    void _set_world_xy(EditorState& st, Entity& e, float wx, float wy) {
        auto& t = e["components"]["Transform"];
        int pid = EditorState::parent_of(e);
        Entity* parent = pid >= 0 ? st.find_entity(pid) : nullptr;
        if (!parent) { t["x"]=wx; t["y"]=wy; transform::mark_local_dirty(e.value("id",0)); return; }

        transform::WorldTRS parent_world = transform::cached_world(*parent);
        transform::WorldTRS target; target.x=wx; target.y=wy;
        auto wt = transform::cached_world(e);
        target.rotation = wt.rotation;
        target.scale_x  = wt.scale_x;
        target.scale_y  = wt.scale_y;
        transform::WorldTRS local = transform::world_to_local(parent_world, target);
        t["x"] = local.x; t["y"] = local.y;
        transform::mark_local_dirty(e.value("id",0));
    }

    void _pick(EditorState& st, float wx, float wy, bool additive, float lx=-1, float ly=-1) {
        Entity* hit = _find_entity_at(st, wx, wy, lx, ly);
        if (additive) {
            if (hit) {
                int id = hit->value("id",0);
                auto it = std::find(st.selected_ids.begin(),st.selected_ids.end(),id);
                if (it != st.selected_ids.end()) st.selected_ids.erase(it);
                else { st.selected_ids.push_back(id); st.selected_id = id; st.asset_selection_is_newer = false; }
            }
            // additive click on empty space: keep existing selection
        } else {
            if (hit) {
                int id = hit->value("id",0);
                st.selected_ids = {id};
                st.selected_id  = id;
                st.log(std::string("Selected: ") + hit->value("name", "Entity"));
            } else {
                st.clear_selection();
            }
        }
    }

    // lx/ly are viewport-local screen coords (for UI hit-test).
    // wx/wy are world coords (for world-entity hit-test).
    Entity* _find_entity_at(EditorState& st, float wx, float wy, float lx=-1, float ly=-1) {
        for (int i=(int)st.entities.size()-1;i>=0;--i) {
            auto& e = st.entities[i];
            if (e.value("_runtime_only",false)) continue;

            // ── UI entity hit-test (screen-space) ───────────────────────────
            if (Entity* ui = _ui_component(const_cast<Entity&>(e))) {
                if (lx < 0) continue;  // no screen coords available
                auto rect = _ui_screen_rect(*ui);
                if (lx >= rect.x && lx <= rect.x + rect.w &&
                    ly >= rect.y && ly <= rect.y + rect.h)
                    return const_cast<Entity*>(&e);
                continue;
            }

            // ── World entity hit-test ────────────────────────────────────────
            if (!has_component(e,"Transform")) continue;
            auto wt = transform::cached_world(e);
            float ex=wt.x, ey=wt.y;
            float hw=16,hh=16;
            if (has_component(e,"BoxCollider2D")) {
                auto& b=e["components"]["BoxCollider2D"];
                hw=b.value("width",32.f)*0.5f; hh=b.value("height",32.f)*0.5f;
                ex+=b.value("offset_x",0.f); ey+=b.value("offset_y",0.f);
            } else if (has_component(e,"CircleCollider2D")) {
                auto& c=e["components"]["CircleCollider2D"];
                float r=c.value("radius",16.f);
                float cx=ex+c.value("offset_x",0.f), cy=ey+c.value("offset_y",0.f);
                if ((wx-cx)*(wx-cx)+(wy-cy)*(wy-cy) <= r*r) return const_cast<Entity*>(&e);
                continue;
            }
            if (wx>=ex-hw && wx<=ex+hw && wy>=ey-hh && wy<=ey+hh) return const_cast<Entity*>(&e);
        }
        return nullptr;
    }

    void _do_rect_select(EditorState& st, ImVec2 start_screen, ImVec2 end_screen) {
        // Treat tiny drags as a click, not a rect-select (already handled by _pick)
        if (std::abs(end_screen.x-start_screen.x) < 3.f && std::abs(end_screen.y-start_screen.y) < 3.f)
            return;
        auto [wx1,wy1] = _cam.screen_to_world(std::min(start_screen.x,end_screen.x), std::min(start_screen.y,end_screen.y));
        auto [wx2,wy2] = _cam.screen_to_world(std::max(start_screen.x,end_screen.x), std::max(start_screen.y,end_screen.y));

        std::vector<int> hit_ids;
        for (auto& e : st.entities) {
            if (!has_component(e,"Transform")) continue;
            auto wt = transform::cached_world(e);
            float x=wt.x, y=wt.y;
            if (x>=wx1 && x<=wx2 && y>=wy1 && y<=wy2) hit_ids.push_back(e.value("id",0));
        }

        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl) {
            for (int id : hit_ids)
                if (std::find(st.selected_ids.begin(),st.selected_ids.end(),id)==st.selected_ids.end())
                    st.selected_ids.push_back(id);
        } else {
            st.selected_ids = hit_ids;
        }
        if (!st.selected_ids.empty()) { st.selected_id = st.selected_ids.back(); st.asset_selection_is_newer = false; }
        st.log("Selected " + std::to_string(hit_ids.size()) + " entity(ies).");
    }

    // ── Low-level: write one tile id at grid coords (gx,gy) on the tilemap, ──
    // expanding the grid/origin as needed. tile_id == -1 erases.
    void _paint_tile_at(Entity& tm, int col, int row, int tile_id) {
        int origin_x = tm.value("origin_x", 0);
        int origin_y = tm.value("origin_y", 0);
        auto& grid = tm["grid"];
        if (!grid.is_array()) grid = nlohmann::json::array();

        int gx = col - origin_x;
        int gy = row - origin_y;

        auto grid_width = [&]() -> int {
            int w = 0;
            for (auto& r : grid) w = std::max(w, (int)r.size());
            return w;
        };
        auto make_row = [&](int w) {
            Entity r = Entity::array();
            for (int i = 0; i < w; ++i) r.push_back(-1);
            return r;
        };

        if (grid.size() == 0) {
            grid.push_back(make_row(1));
            origin_x = col; origin_y = row;
            gx = 0; gy = 0;
        }

        if (gx < 0) {
            int add = -gx;
            for (int r = 0; r < (int)grid.size(); ++r)
                for (int i = 0; i < add; ++i) grid[r].insert((std::size_t)0, -1);
            origin_x -= add;
            gx = 0;
        }
        int w = std::max(1, grid_width());
        if (gy < 0) {
            int add = -gy;
            for (int i = 0; i < add; ++i) grid.insert((std::size_t)0, make_row(w));
            origin_y -= add;
            gy = 0;
        }
        w = std::max(1, grid_width());
        while ((int)grid.size() <= gy) grid.push_back(make_row(w));
        while ((int)grid[gy].size() <= gx) grid[gy].push_back(-1);

        tm["origin_x"] = origin_x;
        tm["origin_y"] = origin_y;
        grid[gy][gx] = tile_id;
    }

    // Stamp the active saved Tile Palette brush.  Legacy square brushes remain
    // supported for old scenes that do not have a palette yet.
    void _stamp_tile_brush(EditorState& st, Entity& tm, int col, int row) {
        if (st.paint_erase) {
            _paint_tile_at(tm, col, row, -1);
            return;
        }
        if (st.paint_brush_cells.empty()) {
            const int half = st.paint_brush_size / 2;
            for (int dr = -half; dr < st.paint_brush_size - half; ++dr)
                for (int dc = -half; dc < st.paint_brush_size - half; ++dc)
                    _paint_tile_at(tm, col + dc, row + dr, st.paint_tile);
            return;
        }
        for (const auto& cell : st.paint_brush_cells) {
            int x = cell.x, y = cell.y;
            if (st.paint_brush_flip_x) x = -x;
            if (st.paint_brush_flip_y) y = -y;
            switch (((st.paint_brush_rotation % 360) + 360) % 360) {
                case 90:  { const int old_x = x; x = -y; y = old_x; break; }
                case 180: x = -x; y = -y; break;
                case 270: { const int old_x = x; x = y; y = -old_x; break; }
                default: break;
            }
            _paint_tile_at(tm, col + x, row + y, cell.tile_id);
        }
    }

    // ── World-coord paint: applies the current brush (size + erase) ──────────
    void _paint_tile(EditorState& st, float wx, float wy) {
        Entity* sel = st.selected_entity();
        if (!sel) return;
        if (!has_component(*sel,"Tilemap") || !has_component(*sel,"Transform")) return;
        auto& tm = (*sel)["components"]["Tilemap"];
        const TilemapGridMetrics metrics = _tilemap_grid_metrics(st, tm);
        auto wt = transform::cached_world(*sel);

        int col = (int)std::floor((wx - wt.x) / metrics.stride_x);
        int row = (int)std::floor((wy - wt.y) / metrics.stride_y);

        // Eyedropper: pick tile id from existing map, don't paint
        if (st.paint_eyedropper) {
            int origin_x = tm.value("origin_x", 0);
            int origin_y = tm.value("origin_y", 0);
            int gx = col - origin_x, gy = row - origin_y;
            auto& grid = tm["grid"];
            if (grid.is_array() && gy >= 0 && gy < (int)grid.size() && grid[gy].is_array()
                && gx >= 0 && gx < (int)grid[gy].size() && grid[gy][gx].is_number_integer()) {
                int picked = grid[gy][gx].get<int>();
                if (picked >= 0) {
                    st.paint_tile = picked;
                    st.paint_brush_cells = {{0, 0, picked}};
                    st.active_tile_brush.clear();
                    st.active_tile_palette = tm.value("tile_palette", std::string());
                    st.log("Eyedropper picked tile " + std::to_string(picked));
                } else st.log("Eyedropper: the selected cell is empty.");
            } else st.log("Eyedropper: no valid tile exists at that cell.");
            st.paint_eyedropper = false; // auto-exit eyedropper after one pick
            return;
        }

        _stamp_tile_brush(st, tm, col, row);
    }

    // ── Rect-fill: paint all tiles between two corners ────────────────────────
    void _paint_tile_rect(EditorState& st, int col0, int row0, int col1, int row1) {
        Entity* sel = st.selected_entity();
        if (!sel) return;
        if (!has_component(*sel,"Tilemap") || !has_component(*sel,"Transform")) return;
        auto& tm = (*sel)["components"]["Tilemap"];
        int c0 = std::min(col0,col1), c1 = std::max(col0,col1);
        int r0 = std::min(row0,row1), r1 = std::max(row0,row1);
        if (st.paint_brush_cells.empty() || st.paint_erase) {
            for (int r = r0; r <= r1; ++r)
                for (int c = c0; c <= c1; ++c)
                    _stamp_tile_brush(st, tm, c, r);
            return;
        }
        int brush_w = 1, brush_h = 1;
        for (const auto& cell : st.paint_brush_cells) {
            brush_w = std::max(brush_w, std::abs(cell.x) + 1);
            brush_h = std::max(brush_h, std::abs(cell.y) + 1);
        }
        for (int r = r0; r <= r1; r += brush_h)
            for (int c = c0; c <= c1; c += brush_w)
                _stamp_tile_brush(st, tm, c, r);
    }
};
