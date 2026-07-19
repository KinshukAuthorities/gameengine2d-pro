/*
 * editor_main.cpp — C++ Editor Application
 * Replaces editor_main.py + viewport.py entirely.
 * Uses SDL2 + Dear ImGui with dockspace layout.
 */

#include "src/panels.hpp"
#include "src/editor_tools.hpp"
#include "src/matchmaking_panel.hpp"
#include "src/unity_gap_features.hpp"
#include "src/tile_palette_panel.hpp"
#include "src/animator_panel.hpp"
#include "src/editor_state.hpp"
#include "src/scene_io.hpp"
#include "src/auto_hot_reload.hpp"
#include "../hub/project_manifest.hpp"
#include "../engine_cpp/crash_reporter.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_vulkan.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <nlohmann/json.hpp>

#include "vk_render/vk_context.hpp"
#include "vk_render/vk_renderer_backend.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>
#include <algorithm>
#include <functional>
#include <memory>
#include <cstdio>
#include <cmath>
#include <unordered_set>
#include <unordered_set>
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <commdlg.h>
#endif

// Generates the editor's window/taskbar icon procedurally — no external
// image file, matching how every other icon in this editor is drawn (see
// hier_icons / comp_icons / asset_icons in src/panels.hpp, all pure vector
// shapes). Colors match apply_unity_theme()'s accent palette (Header/
// SliderGrab blue + the physics-icon orange) so it reads as the same visual
// identity as the rest of the UI: a rounded blue badge, a white diamond
// (a simple, bold mark that stays legible even scaled down to 16x16 for the
// taskbar), and a small orange corner dot for a bit of color.
static SDL_Surface* make_app_icon_surface(int size) {
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return nullptr;
    if (SDL_LockSurface(surf) != 0) { SDL_FreeSurface(surf); return nullptr; }

    const float bg_r=52.f, bg_g=96.f, bg_b=170.f;         // accent blue
    const float mark_r=255.f, mark_g=255.f, mark_b=255.f; // white diamond
    const float acc_r=240.f, acc_g=150.f, acc_b=50.f;     // orange corner dot

    const float cx = size * 0.5f, cy = size * 0.5f;
    const float half = size * 0.5f;
    const float corner_r = size * 0.22f;
    const float diamond_r = size * 0.30f;
    const float dot_r = size * 0.135f;
    const float dot_cx = size * 0.72f, dot_cy = size * 0.72f;

    auto badge_cov = [&](float x, float y) -> float {
        float dx = fabsf(x - cx) - (half - corner_r);
        float dy = fabsf(y - cy) - (half - corner_r);
        if (dx <= 0.f || dy <= 0.f) return 1.f;
        return (dx*dx + dy*dy <= corner_r*corner_r) ? 1.f : 0.f;
    };
    auto diamond_cov = [&](float x, float y) -> float {
        return (fabsf(x - cx) + fabsf(y - cy) <= diamond_r) ? 1.f : 0.f;
    };
    auto dot_cov = [&](float x, float y) -> float {
        float dx = x - dot_cx, dy = y - dot_cy;
        return (dx*dx + dy*dy <= dot_r*dot_r) ? 1.f : 0.f;
    };

    const int SS = 3; // 3x3 supersample per pixel — keeps edges smooth even at 16x16
    for (int y = 0; y < size; ++y) {
        Uint32* row = reinterpret_cast<Uint32*>(static_cast<Uint8*>(surf->pixels) + y * surf->pitch);
        for (int x = 0; x < size; ++x) {
            float bg = 0.f, dm = 0.f, dt = 0.f;
            for (int sy = 0; sy < SS; ++sy) {
                for (int sx = 0; sx < SS; ++sx) {
                    float px = x + (sx + 0.5f) / SS;
                    float py = y + (sy + 0.5f) / SS;
                    bg += badge_cov(px, py);
                    dm += diamond_cov(px, py);
                    dt += dot_cov(px, py);
                }
            }
            const float inv = 1.f / (SS*SS);
            bg *= inv; dm *= inv; dt *= inv;

            float r = bg_r, g = bg_g, b = bg_b, a = bg;
            if (dm > 0.f)          { r = mark_r; g = mark_g; b = mark_b; a = std::max(a, dm * bg); }
            if (dt > 0.f && bg>0.f){ r = acc_r;  g = acc_g;  b = acc_b;  a = std::max(a, dt * bg); }

            row[x] = SDL_MapRGBA(surf->format,
                (Uint8)r, (Uint8)g, (Uint8)b, (Uint8)(a * 255.f));
        }
    }
    SDL_UnlockSurface(surf);
    return surf;
}

// Returns the absolute directory the running executable lives in. Used so
// runtime asset lookups (e.g. the "shaders" directory holding
// sprite.vert.spv/sprite.frag.spv) work regardless of the process's current
// working directory — i.e. whether editor.exe is launched via double-click,
// a desktop shortcut, a different terminal cwd, or a debugger with its own
// "Working Directory" setting. Without this, a relative "shaders" path only
// resolves correctly when launched from inside build/editor/Release/.
static std::string get_executable_dir() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return "."; // fallback to cwd
    std::filesystem::path exe_path(buf);
    return exe_path.parent_path().string();
#else
    // Linux/macOS fallback; not required for the Windows build but keeps
    // this portable.
    std::error_code ec;
    auto p = std::filesystem::canonical("/proc/self/exe", ec);
    if (!ec) return p.parent_path().string();
    return ".";
#endif
}

// ImGui's Vulkan backend wants to know about (and abort on) any VkResult
// failure that happens inside its own draw-data upload/render calls — the
// SDL_Renderer backend had no equivalent since SDL itself silently no-oped
// on most failures. Mirrors vk_check()'s behavior in vk_context.hpp.
static void imgui_vk_check_result(VkResult err) {
    if (err == VK_SUCCESS) return;
    std::cerr << "[ImGui Vulkan] VkResult error: " << (int)err << "\n";
    if (err < 0) std::abort();
}

static bool shortcut(bool ctrl, bool shift, SDL_Keycode key, SDL_Event& ev) {
    if (ev.type != SDL_KEYDOWN) return false;
    bool c = (ev.key.keysym.mod & KMOD_CTRL)  != 0;
    bool s = (ev.key.keysym.mod & KMOD_SHIFT) != 0;
    return c==ctrl && s==shift && ev.key.keysym.sym==key;
}

static void save_prefs(const EditorState& st) {
    try {
        nlohmann::json j;
        j["scene_path"]    = st.scene_path;
        j["recent_scenes"] = st.recent_scenes;
        j["show_grid"]     = st.show_grid;
        j["snap"]          = st.snap;
        j["grid_size"]     = st.grid_size;
        j["default_scene_by_project"] = st.default_scene_by_project;
        std::ofstream f("editor_prefs.json");
        f << j.dump(2);
    } catch(...) {}
}

static void load_prefs(EditorState& st) {
    try {
        std::ifstream f("editor_prefs.json");
        if (!f) return;
        nlohmann::json j; f >> j;
        if (j.contains("scene_path"))    st.scene_path    = j["scene_path"];
        if (j.contains("recent_scenes")) st.recent_scenes = j["recent_scenes"].get<std::vector<std::string>>();
        if (j.contains("show_grid"))     st.show_grid     = j["show_grid"];
        if (j.contains("snap"))          st.snap          = j["snap"];
        if (j.contains("grid_size"))     st.grid_size     = j["grid_size"];
        if (j.contains("default_scene_by_project"))
            st.default_scene_by_project = j["default_scene_by_project"].get<std::unordered_map<std::string,std::string>>();
    } catch(...) {}
}

// The original visual graph was named "Event Graph".  Its dock record was
// therefore orphaned when the production window was renamed to "Visual
// Scripting", making it float or disappear on existing workspaces despite a
// correct fresh-install layout.  Migrate only that window identifier in place:
// every other user-selected dock, size, position, and collapsed state is left
// exactly as it was.
static void migrate_visual_scripting_layout() {
    namespace fs = std::filesystem;
    const fs::path layout_path = "editor_layout.ini";
    std::ifstream in(layout_path, std::ios::binary);
    if (!in) return;
    std::string layout((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    constexpr std::string_view old_name = "Event Graph##evgraph";
    constexpr std::string_view new_name = "Visual Scripting##evgraph";
    bool changed = false;
    for (size_t pos = layout.find(old_name); pos != std::string::npos;
         pos = layout.find(old_name, pos + new_name.size())) {
        layout.replace(pos, old_name.size(), new_name);
        changed = true;
    }
    if (!changed) return;

    const fs::path temporary = layout_path.string() + ".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out.write(layout.data(), static_cast<std::streamsize>(layout.size()));
    out.close();
    if (!out) { std::error_code ignored; fs::remove(temporary, ignored); return; }
    std::error_code ec;
    fs::rename(temporary, layout_path, ec);
    if (ec) {
        ec.clear();
        fs::remove(layout_path, ec);
        if (!ec) fs::rename(temporary, layout_path, ec);
        if (ec) { std::error_code ignored; fs::remove(temporary, ignored); }
    }
}

// ── Project-root detection ────────────────────────────────────────────────────
// Walk up from the executable's directory (or cwd) until we find the folder
// that contains both engine_cpp/CMakeLists.txt and editor/CMakeLists.txt.
// This lets the editor be run from anywhere (double-click, CLI, IDE) and still
// resolve "games/", "editor_prefs.json", "editor_layout.ini" correctly.
static std::filesystem::path find_and_set_project_root() {
    namespace fs = std::filesystem;
    // Start from the executable's own directory if we can determine it,
    // otherwise fall back to the current working directory.
    fs::path start;
#if defined(_WIN32)
    {
        char buf[4096] = {};
        DWORD len = GetModuleFileNameA(nullptr, buf, sizeof(buf));
        if (len > 0) start = fs::path(buf).parent_path();
    }
#endif
    if (start.empty()) start = fs::current_path();

    fs::path cur = start;
    for (int i = 0; i < 12 && !cur.empty(); ++i) {
        if (fs::exists(cur / "engine_cpp" / "CMakeLists.txt") &&
            fs::exists(cur / "editor"     / "CMakeLists.txt")) {
            std::error_code ec;
            fs::current_path(cur, ec);   // chdir to project root
            return cur;
        }
        cur = cur.parent_path();
    }
    // Fallback: stay in whatever directory we started from
    return start;
}

// Return the direct games/<project> parent for a scene path, if there is one.
// Keeping this in the editor makes command-line launches and direct launches
// obey the same lock rule as launches made from hub.exe.
static std::filesystem::path project_root_for_scene(const std::filesystem::path& engine_root,
                                                    const std::string& scene_path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path games = fs::absolute(engine_root / "games", ec).lexically_normal();
    const fs::path scene = fs::absolute(fs::path(scene_path), ec).lexically_normal();
    if (ec) return {};
    const fs::path relative = scene.lexically_relative(games);
    if (relative.empty() || relative == "." || relative.is_absolute()) return {};
    auto part = relative.begin();
    if (part == relative.end() || !gamehub::is_valid_project_id(part->string())) return {};
    const fs::path project = games / *part;
    return fs::is_directory(project, ec) ? project : fs::path{};
}

static bool browse_for_scene_file(const std::filesystem::path& project_root,
                                  std::string& out_scene_path) {
#if defined(_WIN32)
    namespace fs = std::filesystem;
    char file_buf[MAX_PATH * 4] = {};
    std::string initial_dir = (project_root / "games").string();

    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = "Scene Files (*.json)\0*.json\0All Files (*.*)\0*.*\0\0";
    ofn.lpstrFile = file_buf;
    ofn.nMaxFile = static_cast<DWORD>(sizeof(file_buf));
    ofn.lpstrTitle = "Open Scene";
    ofn.lpstrInitialDir = initial_dir.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameA(&ofn)) return false;

    fs::path chosen = fs::path(file_buf);
    std::error_code ec;
    fs::path rel = fs::relative(chosen, project_root, ec);
    if (!ec) {
        std::string rel_str = rel.generic_string();
        if (rel_str.rfind("..", 0) != 0) {
            out_scene_path = rel_str;
            return true;
        }
    }
    out_scene_path = chosen.generic_string();
    return true;
#else
    (void)project_root;
    (void)out_scene_path;
    return false;
#endif
}

// Prompts for a brand-new scene file location (Save-As style) and points
// out_scene_path at it. Used by "New Scene" so it creates a genuinely
// separate file instead of silently reusing whatever scene was open —
// previously "New Scene" only cleared st.entities in memory without ever
// touching st.scene_path, so the very next Save (or autosave) would write
// the cleared/new scene straight over the previously-open project's file.
static bool browse_for_new_scene_file(const std::filesystem::path& project_root,
                                       std::string& out_scene_path) {
#if defined(_WIN32)
    namespace fs = std::filesystem;
    char file_buf[MAX_PATH * 4] = {};
    std::string initial_dir = (project_root / "games").string();

    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = "Scene Files (*.json)\0*.json\0All Files (*.*)\0*.*\0\0";
    ofn.lpstrFile = file_buf;
    ofn.nMaxFile = static_cast<DWORD>(sizeof(file_buf));
    ofn.lpstrTitle = "New Scene — choose a location";
    ofn.lpstrInitialDir = initial_dir.c_str();
    ofn.lpstrDefExt = "json";
    // No OFN_FILEMUSTEXIST here (unlike Open) — we're creating a new file.
    // OFN_OVERWRITEPROMPT still warns if the chosen name already exists,
    // so a same-named-file overwrite is at least an explicit user choice,
    // not something New Scene does silently on the next unrelated Save.
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT;

    if (!GetSaveFileNameA(&ofn)) return false;

    fs::path chosen = fs::path(file_buf);
    if (chosen.extension().empty()) chosen += ".json";
    std::error_code ec;
    fs::path rel = fs::relative(chosen, project_root, ec);
    if (!ec) {
        std::string rel_str = rel.generic_string();
        if (rel_str.rfind("..", 0) != 0) {
            out_scene_path = rel_str;
            return true;
        }
    }
    out_scene_path = chosen.generic_string();
    return true;
#else
    (void)project_root;
    (void)out_scene_path;
    return false;
#endif
}

int editor_main_impl(int argc, char* argv[]) {
    // Resolve working directory to the project root so that relative paths
    // like "games/abyss-of-hollows/scene.json" and "editor_prefs.json" always work,
    // regardless of where editor.exe was launched from.
    auto project_root = find_and_set_project_root();
    crashreport::install("editor", project_root);
    // Ensure games/ directory exists at project root
    {
        std::error_code ec;
        std::filesystem::create_directories(project_root / "games", ec);
    }

    // Args: an optional scene path (positional, no leading "--"), the Hub
    // launch form --project <folder-id> [--scene <relative-scene>], and a
    // legacy "--cleanup-old <path>" pair. Positional scenes remain fully
    // supported for existing shortcuts and scripts.
    // Script rebuilds no longer relaunch editor.exe at all — scripts now
    // live in separate hot-reloadable modules, one PER PROJECT
    // (game_scripts_<project>.dll/.so, see editor/scripts_module/ and
    // ViewportPanel::poll_script_rebuild in panels.hpp), each loaded and
    // swapped in-process independently without restarting the editor.
    // This flag is a no-op in practice now (nothing in this codebase
    // passes it anymore), left in only so an old shortcut/script that still
    // launches the editor with it doesn't fail outright.
    std::string positional_scene;
    std::string requested_project;
    std::string project_scene_override;
    std::filesystem::path cleanup_old_exe;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--cleanup-old" && i + 1 < argc) {
            cleanup_old_exe = argv[++i];
        } else if (a == "--project" && i + 1 < argc) {
            requested_project = argv[++i];
        } else if (a == "--scene" && i + 1 < argc) {
            project_scene_override = argv[++i];
        } else if (!a.empty() && a[0] != '-') {
            positional_scene = a;
        }
    }
    if (!cleanup_old_exe.empty()) {
        std::error_code ec;
        for (int attempt = 0; attempt < 20; ++attempt) {
            if (!std::filesystem::exists(cleanup_old_exe, ec)) break;
            if (std::filesystem::remove(cleanup_old_exe, ec)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        // If it's still locked after ~3s, just leave it — harmless stale
        // file, not a correctness issue.
    }

    std::string requested_scene = positional_scene;
    if (!requested_project.empty()) {
        if (!gamehub::is_valid_project_id(requested_project)) {
            std::cerr << "[Editor] Invalid --project folder ID.\n";
            return 1;
        }
        const std::filesystem::path project_dir = project_root / "games" / requested_project;
        std::string manifest_error;
        const auto manifest = gamehub::load_manifest(project_dir, true, &manifest_error);
        if (!manifest) {
            std::cerr << "[Editor] Could not open project '" << requested_project << "': " << manifest_error << "\n";
            return 1;
        }
        const std::string scene = project_scene_override.empty() ? manifest->default_scene : project_scene_override;
        if (!gamehub::is_safe_relative_path(scene) ||
            !std::filesystem::is_regular_file(project_dir / scene)) {
            std::cerr << "[Editor] Project scene is missing or is outside the project: " << scene << "\n";
            return 1;
        }
        requested_scene = (std::filesystem::path("games") / requested_project / scene).generic_string();
    }
    if (requested_scene.empty()) {
        // Resolve the direct-launch default before the graphics stack starts,
        // so its project can be locked for the whole editor lifetime too.
        EditorState preference_preview;
        load_prefs(preference_preview);
        requested_scene = preference_preview.scene_path;
    }

    std::unique_ptr<gamehub::ProjectLock> active_project_lock;
    const std::filesystem::path lock_project = project_root_for_scene(project_root, requested_scene);
    if (!lock_project.empty()) {
        active_project_lock = std::make_unique<gamehub::ProjectLock>();
        std::string lock_error;
        if (!active_project_lock->acquire(lock_project, &lock_error)) {
            std::cerr << "[Editor] " << lock_error << "\n";
            return 1;
        }
    }

    // The editor owns the same component runtime as a standalone build.
    // Initialising only video made an otherwise valid AudioSource fallback
    // fail at runtime and also prevented controller discovery in Play mode.
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS |
                 SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::cerr << "[Editor] SDL_Init failed: " << SDL_GetError() << "\n";
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "GameEngine2D Pro — Editor",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1600, 900,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_VULKAN
    );
    if (!window) { std::cerr << SDL_GetError(); return 1; }

    // Window/taskbar icon — see make_app_icon_surface() above. Freed right
    // after the call: SDL_SetWindowIcon copies the pixel data internally,
    // it doesn't keep a reference to the surface we pass in.
    if (SDL_Surface* icon = make_app_icon_surface(64)) {
        SDL_SetWindowIcon(window, icon);
        SDL_FreeSurface(icon);
    }

    // Vulkan backend owning the swapchain this window presents to. The
    // editor's own UI (ImGui) renders straight into this swapchain's render
    // pass; ViewportPanel renders the scene separately into its own
    // offscreen vkr::RenderTarget and is displayed as a sampled image inside
    // an ImGui::Image() call (see src/panels.hpp).
    const std::string shader_dir = get_executable_dir() + "/shaders";
    vkr::RendererBackend vk_backend(window, shader_dir, false /*vsync*/);

    // ── ImGui descriptor pool ───────────────────────────────────────────────
    // Dedicated pool for ImGui's own font atlas + every ImGui_ImplVulkan_AddTexture()
    // call (viewport image, asset thumbnails, animator thumbnails). Must allow
    // individually freed sets since thumbnails come and go as files change on
    // disk (thumbnail_cache::Cache reloads on mtime change).
    VkDescriptorPool imgui_desc_pool = VK_NULL_HANDLE;
    {
        VkDescriptorPoolSize pool_sizes[] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256 } };
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pi.maxSets = 256;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = pool_sizes;
        if (vkCreateDescriptorPool(vk_backend.ctx().device, &pi, nullptr, &imgui_desc_pool) != VK_SUCCESS) {
            std::cerr << "[Editor] vkCreateDescriptorPool (ImGui) failed\n";
            return 1;
        }
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Required authoring tools (Project Settings, Build Report, Prefab Stage,
    // Animator, profilers, import editors, etc.) must be able to leave the
    // main dockspace as real OS windows. SDL2 + Vulkan both provide the
    // viewport backend in this build. Docked daily panels stay docked; a
    // floating tool becomes its own resizable/minimizable desktop window.
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    // Tool windows are top-level, resizable desktop windows rather than
    // parented overlays.  This keeps Project Settings/Animator/etc. usable on
    // a second monitor and gives Windows normal minimize/taskbar behavior.
    io.ConfigViewportsNoDefaultParent = true;
    io.ConfigViewportsNoTaskBarIcon = false;
    io.ConfigViewportsNoAutoMerge = true;
    // A detached authoring tool must behave like a normal Windows window:
    // dragging controls/content must not move the whole tool.  ImGui defaults
    // to allowing a drag from empty window space, which is especially easy to
    // trigger in Project Settings and the palette editors.
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    migrate_visual_scripting_layout();
    io.IniFilename  = "editor_layout.ini";
    apply_unity_theme();

    // Scale text and hit targets to the active monitor. A fixed 13px UI is
    // difficult to use on current high-density laptop and desktop displays.
    float ui_scale = 1.f;
    float display_dpi = 0.f;
    const int display_index = SDL_GetWindowDisplayIndex(window);
    if (display_index >= 0 && SDL_GetDisplayDPI(display_index, nullptr, &display_dpi, nullptr) == 0 && display_dpi > 0.f)
        ui_scale = std::clamp(display_dpi / 96.f, 1.f, 1.5f);
    ImGui::GetStyle().ScaleAllSizes(ui_scale);

    // Use ImGui's built-in ProggyClean pixel font — it's the sharpest,
    // most readable option at small editor panel sizes. The bundled Roboto
    // Medium looked chunky and blurry at 13px; ProggyClean is pixel-perfect.
    // System fonts (Segoe UI etc) are kept as a secondary option at a
    // comfortable size for users who prefer anti-aliased text.
    const char* system_font_paths[] = {
        "C:/Windows/Fonts/segoeui.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
    };
    bool font_loaded = false;
    for (auto* fp : system_font_paths) {
        if (std::filesystem::exists(fp)) {
            io.Fonts->AddFontFromFileTTF(fp, 15.f * ui_scale);
            font_loaded = true; break;
        }
    }
    if (!font_loaded) {
        ImFontConfig cfg;
        cfg.SizePixels = 15.f * ui_scale;
        io.Fonts->AddFontDefault(&cfg);
    }

    ImGui_ImplSDL2_InitForVulkan(window);

    ImGui_ImplVulkan_InitInfo vk_init{};
    vk_init.ApiVersion       = VK_API_VERSION_1_0;
    vk_init.Instance         = vk_backend.ctx().instance;
    vk_init.PhysicalDevice   = vk_backend.ctx().physical_device;
    vk_init.Device           = vk_backend.ctx().device;
    vk_init.QueueFamily      = *vk_backend.ctx().queue_families.graphics;
    vk_init.Queue            = vk_backend.ctx().graphics_queue;
    vk_init.DescriptorPool   = imgui_desc_pool;
    vk_init.MinImageCount    = 2;
    vk_init.ImageCount       = std::max<uint32_t>(2, vk_backend.swapchain_image_count());
    vk_init.PipelineInfoMain.RenderPass = vk_backend.swapchain_render_pass();
    vk_init.PipelineInfoMain.Subpass    = 0;
    vk_init.UseDynamicRendering = false;
    vk_init.CheckVkResultFn  = imgui_vk_check_result;
    ImGui_ImplVulkan_Init(&vk_init);

    EditorState st;
    load_prefs(st);
    if (!requested_scene.empty()) st.scene_path = requested_scene;
    st.update_asset_dir();
    SceneIO::load(st);
    // Patch nova advanced component defaults into the shared singleton
    { auto& d = const_cast<nlohmann::json&>(component_defaults()); patch_nova_component_defaults(d); patch_nova_component_defaults_b2(d); }

    HierarchyPanel hierarchy;
    InspectorPanel inspector;
    ConsolePanel   console;
    // These three own GPU resources registered with ImGui's Vulkan backend
    // (thumbnail textures, the viewport's offscreen RenderTarget image) —
    // held by pointer and explicitly destroyed further down, BEFORE the
    // ImGui descriptor pool / ImGui_ImplVulkan_Shutdown() run. If they were
    // plain locals instead, they'd be destroyed during stack unwind AFTER
    // those teardown calls (locals destruct in reverse declaration order,
    // and these are declared after vk_backend but the pool/shutdown calls
    // are statements that run before any destructor) — freeing descriptor
    // sets from an already-destroyed pool and calling Vulkan functions
    // after the backend that provides them is already torn down.
    auto assets         = std::make_unique<AssetsPanel>();
    auto viewport       = std::make_unique<ViewportPanel>();
    auto animator_panel = std::make_unique<AnimatorPanel>();
    LobbyMatchmakingPanel lobby_panel;

    // ── Unity-gap feature panels ──────────────────────────────────────────────
    ProjectSettingsPanel proj_settings;
    SpriteSlicer         sprite_slicer;
    ObjectPickerPanel    object_picker;
    SceneStatsPanel      scene_stats;
    ColorPalettePanel    color_palette;
    TilePalettePanel     tile_palette;
    TemplateLibraryPanel template_library;
    EventGraphPanel      event_graph;
    LightingPanel        lighting_panel;
    ProfilerPanel        profiler_panel;
    AudioMixerPanel      audio_mixer;
    InputManagerPanel    input_manager;
    AssetImporterPanel   asset_importer;
    // ── New major feature panels ──────────────────────────────────────────────
    PrefabManagerPanel   prefab_manager;
    PrefabStagePanel     prefab_stage;
    NavMeshPanel         navmesh_panel;
    TimelinePanel        timeline_panel;
    ScriptableObjectPanel scriptable_obj_panel;
    PhysicsDebugger2D    physics_debugger;
    MemoryProfilerPanel  memory_profiler;
    PackageManagerPanel  package_manager;
    // ── Nova Advanced Panels ─────────────────────────────────────────────────
    SpriteEditorPanel    sprite_editor;
    GradientEditorPanel  gradient_editor;
    ShaderGraphPanel     shader_graph;
    FrameDebuggerPanel   frame_debugger;
    UndoHistoryPanel     undo_history;
    UIBuilderPanel       ui_builder;
    BuildReportPanel     build_report;
    GitVersionControlPanel git_vcs;
    GizmoOverlayPanel    gizmo_overlay;
    Shadow2DSettingsPanel shadow2d_settings;
    EffectorChainDebugger effector_debugger;
    BatchRenamerPanel    batch_renamer;
    ComponentSearchPanel component_search;
    HotkeysPanel         hotkeys_panel;
    PathFollowerPanel    path_follower_panel;

    // Window + command-palette tools are registered once, next to panel
    // ownership.  This is intentionally data-driven: adding a tool no longer
    // requires maintaining two independent hand-written menus.
    EditorToolRegistry tool_registry;
    const auto always_available = [] { return true; };
    tool_registry.add({"project-settings", "Project", "Project Settings", "Ctrl+,",
                       "Project-wide layers, tags and build settings.", ToolMaturity::Production,
                       [&] { proj_settings.open(); }, {}, always_available, {}});
    tool_registry.add({"input-manager", "Project", "Input Manager", "",
                       "Configure named input actions for the project.", ToolMaturity::Production,
                       [&] { input_manager.open(); }, {}, always_available, {}});
    tool_registry.add({"audio-mixer", "Project", "Audio Mixer", "",
                       "Mix and preview project audio sources.", ToolMaturity::Production,
                       [&] { audio_mixer.open(); }, {}, always_available, {}});
    tool_registry.add({"capabilities", "Project", "Capabilities", "Ctrl+Shift+P",
                       "View engine capabilities and optional feature modules.", ToolMaturity::Experimental,
                       [&] { package_manager.open(); }, {}, always_available, {}});
    tool_registry.add({"build-report", "Project", "Build Report", "Ctrl+Shift+B",
                       "Review the most recent project build output.", ToolMaturity::Production,
                       [&] { build_report.open(); }, {}, always_available, {}});
    tool_registry.add({"package-manager", "Project", "Package Manager", "",
                       "Review built-in and optional engine feature packages.", ToolMaturity::Experimental,
                       [&] { package_manager.open(); }, {}, always_available, {}});

    tool_registry.add({"template-library", "Assets", "Template Library", "",
                       "Add ready-made 2D gameplay objects to the active scene.", ToolMaturity::Production,
                       [&] { template_library.open(); }, {}, always_available, {}});
    tool_registry.add({"prefab-manager", "Assets", "Prefab Manager", "",
                       "Create, inspect and instantiate reusable prefabs.", ToolMaturity::Production,
                       [&] { prefab_manager.open(); }, {}, always_available, {}});
    tool_registry.add({"prefab-stage", "Assets", "Prefab Stage", "",
                       "Open the selected prefab in its isolated editable stage.", ToolMaturity::Production,
                       [&] {
                           st.requested_prefab_stage_asset = st.selected_asset_path;
                           st.request_prefab_stage_open = true;
                       }, {},
                       [&] { return fs::path(st.selected_asset_path).extension() == ".prefab"; },
                       [&] { return std::string("Select a .prefab asset in Assets first."); }});
    tool_registry.add({"import-settings", "Assets", "Import Settings", "",
                       "Configure import settings for the selected asset.", ToolMaturity::Production,
                       [&] { asset_importer.open(st.selected_asset_path); }, {},
                       [&] { return !st.selected_asset_path.empty(); },
                       [&] { return std::string("Select an asset in Assets first."); }});

    tool_registry.add({"animator", "Animation", "Animator", "",
                       "Author sprite animation clips and state transitions.", ToolMaturity::Production,
                       [&] { *animator_panel->open_flag() = true; }, [&] { return *animator_panel->open_flag(); }, always_available, {}});
    tool_registry.add({"animator-layers", "Animation", "Animator Layers", "",
                       "Edit layered animation blending.", ToolMaturity::Production,
                       [&] { *animator_panel->layers_open_flag() = true; }, [&] { return *animator_panel->layers_open_flag(); }, always_available, {}});
    tool_registry.add({"timeline", "Animation", "Timeline", "Ctrl+T",
                       "Author sequenced animation and scene events.", ToolMaturity::Production,
                       [&] { timeline_panel.open(); }, {}, always_available, {}});

    tool_registry.add({"visual-scripting", "Visual Tools", "Visual Scripting", "Ctrl+Alt+G",
                       "Create and debug the selected entity's node graph.", ToolMaturity::Production,
                       [&] { event_graph.open(st); }, {}, always_available, {}});
    tool_registry.add({"shader-graph", "Visual Tools", "Shader Graph", "Ctrl+G",
                       "Build connected node graphs into compiled SpriteRenderer material assets.", ToolMaturity::Production,
                       [&] { shader_graph.open(); }, {}, always_available, {}});
    tool_registry.add({"ui-builder", "Visual Tools", "UI Builder", "Ctrl+Shift+U",
                       "Lay out and create real UICanvas/UI entities for the active scene.", ToolMaturity::Production,
                       [&] { ui_builder.open(); }, {}, always_available, {}});
    tool_registry.add({"scriptable-objects", "Visual Tools", "Scriptable Objects", "",
                       "Edit reusable project data assets.", ToolMaturity::Production,
                       [&] { scriptable_obj_panel.open(); }, {}, always_available, {}});

    tool_registry.add({"sprite-editor", "2D", "Sprite Editor", "",
                       "Edit sprite settings for the selected image asset.", ToolMaturity::Production,
                       [&] { sprite_editor.open(st.selected_asset_path); }, {},
                       [&] { return !st.selected_asset_path.empty(); },
                       [&] { return std::string("Select an image in Assets first."); }});
    tool_registry.add({"sprite-slicer", "2D", "Sprite Slicer", "",
                       "Slice the selected image into sprite regions.", ToolMaturity::Production,
                       [&] { sprite_slicer.open(st.selected_asset_path, 512, 512); }, {},
                       [&] { return !st.selected_asset_path.empty(); },
                       [&] { return std::string("Select an image in Assets first."); }});
    tool_registry.add({"sprite-atlas", "2D", "Sprite Atlas", "",
                       "Pack image folders into a runtime-readable texture atlas.", ToolMaturity::Production,
                       [&] { proj_settings.open_sprite_atlas(); }, {}, always_available, {}});
    tool_registry.add({"rule-tile-editor", "2D", "Rule Tile Editor", "",
                       "Create neighbor-aware tiles for Tilemap painting.", ToolMaturity::Production,
                       [&] { proj_settings.open_rule_tile_editor(); }, {}, always_available, {}});
    tool_registry.add({"physics-material-2d", "2D", "Physics Material 2D", "",
                       "Create reusable friction and bounciness material assets.", ToolMaturity::Production,
                       [&] { proj_settings.open_physics_material_editor(); }, {}, always_available, {}});
    tool_registry.add({"lighting", "2D", "Lighting", "",
                       "Tune 2D lights and scene lighting.", ToolMaturity::Production,
                       [&] { lighting_panel.open(); }, {}, always_available, {}});
    tool_registry.add({"physics-debugger", "2D", "Physics Debugger 2D", "Ctrl+Shift+D",
                       "Inspect colliders, velocities and physics contacts.", ToolMaturity::Production,
                       [&] { physics_debugger.open(); }, {}, always_available, {}});
    tool_registry.add({"navmesh", "2D", "NavMesh 2D", "Ctrl+Shift+N",
                       "Build and inspect 2D navigation data.", ToolMaturity::Production,
                       [&] { navmesh_panel.open(); }, {}, always_available, {}});
    tool_registry.add({"path-follower", "2D", "Path Follower", "",
                       "Author and preview a selected entity's movement path.", ToolMaturity::Production,
                       [&] { path_follower_panel.open(); }, {}, always_available, {}});
    tool_registry.add({"color-palette", "2D", "Color Palette", "",
                       "Manage reusable project color swatches.", ToolMaturity::Production,
                       [&] { color_palette.open(); }, {}, always_available, {}});
    tool_registry.add({"tile-palette", "2D", "Tile Palette", "",
                       "Import tile images, build a palette atlas, and save multi-tile paint brushes.", ToolMaturity::Production,
                       [&] { tile_palette.open(st); }, [&] { return tile_palette.is_open(); }, always_available, {}});
    tool_registry.add({"gizmo-overlay", "2D", "Gizmo Overlay", "",
                       "Choose debug overlays visible in the Scene view.", ToolMaturity::Production,
                       [&] { gizmo_overlay.open(); }, [&] { return gizmo_overlay.is_open(); }, always_available, {}});
    tool_registry.add({"shadow-settings", "2D", "Shadow 2D Settings", "",
                       "Tune live and exported global 2D lighting and shadow settings.", ToolMaturity::Production,
                       [&] { shadow2d_settings.open(); }, [&] { return shadow2d_settings.is_open(); }, always_available, {}});

    tool_registry.add({"profiler", "Analysis", "Profiler", "Ctrl+7",
                       "Inspect real frame timing from the active editor session.", ToolMaturity::Production,
                       [&] { profiler_panel.open(); }, {}, always_available, {}});
    tool_registry.add({"memory-profiler", "Analysis", "Memory Profiler", "Ctrl+8",
                       "Capture scene memory information.", ToolMaturity::Production,
                       [&] { memory_profiler.open(); }, {}, always_available, {}});
    tool_registry.add({"frame-debugger", "Analysis", "Frame Debugger", "Ctrl+Shift+F",
                       "Capture renderer passes while the scene is playing.", ToolMaturity::Production,
                       [&] { frame_debugger.open(); }, {}, [&] { return st.playing; },
                       [&] { return std::string("Enter Play mode to capture a real frame."); }});
    tool_registry.add({"scene-statistics", "Analysis", "Scene Statistics", "",
                       "Review entity and component counts for the current scene.", ToolMaturity::Production,
                       [&] { scene_stats.open(); }, {}, always_available, {}});

    tool_registry.add({"find-object", "Utilities", "Find Object", "Ctrl+F",
                       "Search entities in the current scene.", ToolMaturity::Production,
                       [&] { object_picker.open(); }, {}, always_available, {}});
    tool_registry.add({"component-search", "Utilities", "Component Search", "Ctrl+Shift+C",
                       "Find which entities use a component.", ToolMaturity::Production,
                       [&] { component_search.open(); }, {}, always_available, {}});
    tool_registry.add({"undo-history", "Utilities", "Undo History", "Ctrl+Shift+H",
                       "Review scene editing history.", ToolMaturity::Production,
                       [&] { undo_history.open(); }, {}, always_available, {}});
    tool_registry.add({"batch-renamer", "Utilities", "Batch Renamer", "Ctrl+Shift+R",
                       "Rename a selected set of scene entities.", ToolMaturity::Production,
                       [&] { batch_renamer.open(); }, {}, always_available, {}});
    tool_registry.add({"effector-debugger", "Utilities", "Effector Chain Debugger", "",
                       "Inspect physics effector composition on the selected entity.", ToolMaturity::Production,
                       [&] { effector_debugger.open(); }, [&] { return effector_debugger.is_open(); }, always_available, {}});
    tool_registry.add({"version-control", "Utilities", "Version Control", "",
                       "Inspect local Git status when the project is under source control.", ToolMaturity::Experimental,
                       [&] { git_vcs.open(); }, [&] { return git_vcs.is_open(); }, always_available, {}});
    tool_registry.add({"hotkeys", "Utilities", "Hotkeys", "F1",
                       "View editor shortcut reference.", ToolMaturity::Production,
                       [&] { hotkeys_panel.open(); }, {}, always_available, {}});

    const std::string default_font_path = get_executable_dir() + "/assets/fonts/default.ttf";
    viewport->init(vk_backend, default_font_path);
    tile_palette.init(vk_backend);
    inspector.set_tile_palette_picker([&tile_palette](EditorState& state, Entity& tilemap) {
        tile_palette.draw_inline_picker(state, tilemap);
    });
    assets->init(vk_backend, st);
    animator_panel->init(vk_backend);
    sprite_editor.init(vk_backend);
    prefab_stage.init(vk_backend);
    lobby_panel.sync_from_state();

    // ── Auto hot-reload: watch the active project's scripts directory ─────
    {
        std::string project = project_name_from_scene_path(std::filesystem::absolute(st.scene_path));
        if (!project.empty()) {
            fs::path scripts_dir = fs::path(st.scene_path).parent_path() / "scripts";
            if (fs::exists(scripts_dir)) {
                AutoHotReload::instance().start_watching(
                    project, scripts_dir,
                    fs::absolute("build/editor/scripts_module"),
                    fs::absolute(".")
                );
                st.log_engine("Auto hot-reload active: " + scripts_dir.string());
            }
        }
    }

    st.log_engine("GameEngine2D Pro Editor — C++ Edition");
    st.log_engine("Scene: " + st.scene_path);

    // Route Debug::log / log_warning / log_error into the editor's Console
    // panel so NetSpawn, network, and script logs show up in-editor rather
    // than only in the terminal. Level: 0=log, 1=warn, 2=error.
    Debug::set_log_callback([&st](const std::string& msg, int level) {
        if (level == 2) st.log_error("[Debug] " + msg);
        else if (level == 1) st.log_warn("[Debug] " + msg);
        else st.log_engine("[Debug] " + msg);
    });

    using Clock = std::chrono::high_resolution_clock;
    auto last = Clock::now();
    bool running = true;

    // Keep a malformed legacy asset or an individual tool defect from
    // terminating the entire editor process.  The first exception identifies
    // its phase in both stderr and Console, then that phase is skipped for
    // the rest of this session while the rest of the workspace remains usable.
    std::unordered_set<std::string> quarantined_panel_phases;
    auto draw_panel_safely = [&](const char* phase, auto&& draw) {
        if (quarantined_panel_phases.count(phase)) return;
        try {
            draw();
        } catch (const std::exception& ex) {
            quarantined_panel_phases.insert(phase);
            const std::string message = std::string("[Editor] ") + phase +
                " was disabled after an exception: " + ex.what();
            st.log_error(message);
            std::cerr << message << '\n';
        } catch (...) {
            quarantined_panel_phases.insert(phase);
            const std::string message = std::string("[Editor] ") + phase +
                " was disabled after an unknown exception.";
            st.log_error(message);
            std::cerr << message << '\n';
        }
    };

    // Rolling FPS counter shown in the window title bar
    double fps_accum = 0.0;
    int    fps_count  = 0;
    double fps_value  = 0.0;
    // A compact, keyboard-first entry point for the editor's most common
    // actions.  The editor has grown a substantial toolset; relying solely
    // on nested menus makes that power hard to discover for new users.
    bool command_palette_requested = false;
    bool close_confirmation_requested = false;

    // The OS close button, Alt+F4/SDL_QUIT, and File > Quit all share this
    // path.  Compare the complete authoring scene to disk at close time so
    // every edit route is covered, including property edits made by panels
    // that do not use the Undo stack.  Play-mode mutations are first restored
    // to the pre-play authoring snapshot and are never offered as scene edits.
    auto request_editor_close = [&] {
        if (st.playing) viewport->stop_play(st);
        if (SceneIO::has_unsaved_changes(st)) {
            close_confirmation_requested = true;
            return;
        }
        running = false;
    };

    while (running) {
        auto now = Clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        dt = std::min(dt, 0.05f);
        last = now;

        // ── FPS title bar ────────────────────────────────────────────────────
        fps_accum += dt;
        ++fps_count;
        if (fps_accum >= 0.25) {   // update 4x/sec for a responsive but stable readout
            fps_value = fps_count / fps_accum;
            fps_accum = 0.0;
            fps_count = 0;
            char title[128];
            snprintf(title, sizeof(title),
                "GameEngine2D Pro — Editor  |  %.0f fps", fps_value);
            SDL_SetWindowTitle(window, title);
        }

        viewport->begin_input_frame();

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            // Feed the game first.  In Play mode the viewport is the input
            // owner: forwarding Tab/Escape/WASD to ImGui as well makes ImGui
            // keyboard navigation steal those actions (Tab changes focus,
            // Escape dismisses an editor window) before the running game can
            // reliably react.  Keep Ctrl/Alt shortcuts available to the
            // editor transport, but leave ordinary game keys exclusively to
            // the runtime until Play stops.
            viewport->process_input_event(ev);
            const bool key_event = ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP;
            const SDL_Keymod key_mods = key_event
                ? (SDL_Keymod)ev.key.keysym.mod : KMOD_NONE;
            const bool editor_modified_key = (key_mods & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) != 0;
            const bool runtime_owns_key = st.playing && key_event && !editor_modified_key;
            if (!runtime_owns_key)
                ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) request_editor_close();
            if (ev.type == SDL_WINDOWEVENT &&
                ev.window.event == SDL_WINDOWEVENT_CLOSE &&
                ev.window.windowID == SDL_GetWindowID(window))
                request_editor_close();

            if (ev.type == SDL_KEYDOWN) {
                // Play input is owned entirely by the running game.  In
                // particular Escape is Abyss' pause key, not "stop Play",
                // and W/E/R/F/arrows must never trigger editor tools or
                // mutate the authoring scene while a game is running.
                // Keep editor transport controls out of the game's action
                // map: F5 starts Play only from Edit mode; stopping/pausing
                // a live run requires Ctrl+F5 / Ctrl+F6 respectively.
                const bool ctrl = (ev.key.keysym.mod & KMOD_CTRL) != 0;
                if (!st.playing && ev.key.keysym.sym == SDLK_F5) {
                    viewport->start_play(st);
                    continue;
                }
                if (st.playing && ctrl && ev.key.keysym.sym == SDLK_F5) {
                    viewport->stop_play(st);
                    continue;
                }
                if (st.playing && ctrl && ev.key.keysym.sym == SDLK_F6) {
                    st.paused = !st.paused;
                    continue;
                }

                // Editor shortcuts only run in Edit mode and only while the user
                // isn't actively typing into a text field (renaming an object,
                // editing a number, etc).
                //
                // NOTE: this intentionally checks io.WantTextInput, not
                // io.WantCaptureKeyboard. With ImGuiConfigFlags_NavEnableKeyboard
                // enabled (set above), ImGui sets io.WantCaptureKeyboard = true
                // any time a window has keyboard nav focus - which is basically
                // always, since some panel (Hierarchy/Inspector/Viewport/...)
                // has focus the moment you click anywhere. Gating on
                // WantCaptureKeyboard therefore swallowed every shortcut
                // (Ctrl+S, Ctrl+Z, W/E/R tool keys, Delete, ...) almost all the
                // time. io.WantTextInput is only true while an actual text/number
                // edit field has keyboard focus, which is the behavior we want.
                if (!st.playing && !io.WantTextInput) {
                    // Ctrl+K Command Palette. Keep this before Ctrl+P and
                    // other editor shortcuts so it remains available from
                    // every docked panel when the user is not typing.
                    if (shortcut(true,false,SDLK_k,ev)) { command_palette_requested = true; continue; }
                    // Ctrl+S Save. Saving is not an edit, so it must not add a
                    // duplicate undo state or consume the user's undo history.
                    if (shortcut(true,false,SDLK_s,ev)) { SceneIO::save(st); continue; }
                    // Ctrl+O Open
                    if (shortcut(true,false,SDLK_o,ev)) { if (browse_for_scene_file(project_root, st.scene_path)) SceneIO::load(st); continue; }
                    // Ctrl+N New Scene. Match the File-menu path chooser: the
                    // old shortcut cleared the current scene in memory and a
                    // later Save could overwrite it without warning.
                    if (shortcut(true,false,SDLK_n,ev)) {
                        std::string new_path;
                        if (browse_for_new_scene_file(project_root, new_path)) {
                            st.scene_path = new_path;
                            st.entities.clear();
                            st.clear_selection();
                            st.scene_snapshot.clear();
                            transform::mark_structure_dirty();
                            ScriptRegistry::instance().set_active_project_from_scene_path(st.scene_path);
                            SceneIO::save(st);
                            st.log("New scene: " + st.scene_path);
                        }
                        continue;
                    }
                    // Ctrl+, Project Settings
                    if (shortcut(true,false,SDLK_COMMA,ev)) { proj_settings.open(); continue; }
                    // Ctrl+F Find Object
                    if (shortcut(true,false,SDLK_f,ev)) { object_picker.open(); continue; }
                    // Ctrl+7 Profiler
                    if (shortcut(true,false,SDLK_7,ev)) { profiler_panel.open(); continue; }
                    // Ctrl+8 Memory Profiler
                    if (shortcut(true,false,SDLK_8,ev)) { memory_profiler.open(); continue; }
                    // Ctrl+P Prefab Manager
                    if (shortcut(true,false,SDLK_p,ev)) { prefab_manager.open(); continue; }
                    // Ctrl+T Timeline
                    if (shortcut(true,false,SDLK_t,ev)) { timeline_panel.open(); continue; }
                    // Ctrl+Shift+N NavMesh
                    if (shortcut(true,true,SDLK_n,ev)) { navmesh_panel.open(); continue; }
                    // Ctrl+Shift+P Package Manager
                    if (shortcut(true,true,SDLK_p,ev)) { package_manager.open(); continue; }
                    // Ctrl+Shift+D Physics Debugger
                    if (shortcut(true,true,SDLK_d,ev)) { physics_debugger.open(); continue; }
                    // Nova Advanced Panels shortcuts
                    if (shortcut(true,false,SDLK_g,ev)) { shader_graph.open(); continue; }
                    if (shortcut(true,true,SDLK_f,ev)) { frame_debugger.open(); continue; }
                    if (shortcut(true,true,SDLK_h,ev)) { undo_history.open(); continue; }
                    if (shortcut(true,true,SDLK_u,ev)) { ui_builder.open(); continue; }
                    if (shortcut(true,true,SDLK_b,ev)) { build_report.open(); continue; }
                    if (shortcut(true,true,SDLK_g,ev)) { git_vcs.open(); continue; }
                    if (shortcut(true,true,SDLK_o,ev)) { gizmo_overlay.open(); continue; }
                    if (shortcut(true,true,SDLK_s,ev)) { shadow2d_settings.open(); continue; }
                    if (shortcut(true,true,SDLK_e,ev)) { effector_debugger.open(); continue; }
                    if (shortcut(true,true,SDLK_r,ev)) { batch_renamer.open(); continue; }
                    if (shortcut(true,true,SDLK_c,ev)) { component_search.open(); continue; }
                    if (shortcut(false,false,SDLK_F1,ev)) { hotkeys_panel.open(); continue; }
                    // Ctrl+Z Undo
                    if (shortcut(true,false,SDLK_z,ev) && st.undo.can_undo()) {
                        st.entities=st.undo.undo(st.entities); st.clear_selection(); transform::mark_structure_dirty(); st.log("Undo."); undo_history.notify_undo(); continue; }
                    // Ctrl+Y Redo
                    if ((shortcut(true,false,SDLK_y,ev)||shortcut(true,true,SDLK_z,ev)) && st.undo.can_redo()) {
                        st.entities=st.undo.redo(st.entities); st.clear_selection(); transform::mark_structure_dirty(); st.log("Redo."); undo_history.notify_redo(); continue; }
                    // Ctrl+A Select All authoring entities. This is a global
                    // scene command (not a Hierarchy-only feature), so it
                    // works from the Viewport, Inspector, Assets and every
                    // detached tool window whenever no text field is active.
                    if (shortcut(true,false,SDLK_a,ev)) {
                        st.clear_selection();
                        for (const auto& entity : st.entities) {
                            if (entity.value("_runtime_only", false)) continue;
                            const int id = entity.value("id", -1);
                            if (id >= 0) st.selected_ids.push_back(id);
                        }
                        if (!st.selected_ids.empty()) st.selected_id = st.selected_ids.back();
                        st.log("Selected " + std::to_string(st.selected_ids.size()) + " entity(ies).");
                        continue;
                    }
                    // Ctrl+C Copy
                    if (shortcut(true,false,SDLK_c,ev)) {
                        if (st.selected_id>=0) {
                            st.clipboard.clear();
                            for (int id : st.selected_ids) {
                                if (Entity* e = st.find_entity(id))
                                    st.clipboard.push_back(e->deep_clone());
                            }
                            st.log("Copied " + std::to_string(st.clipboard.size()) + " entity(ies).");
                        }
                        continue;
                    }
                    // Ctrl+X Cut mirrors Copy followed by one undoable scene
                    // delete. It intentionally keeps the same complete
                    // hierarchy behavior as Delete, rather than leaving
                    // children orphaned when a parent is cut.
                    if (shortcut(true,false,SDLK_x,ev)) {
                        if (st.selected_id >= 0) {
                            st.clipboard.clear();
                            std::unordered_set<int> to_cut;
                            std::vector<int> pending = st.selected_ids;
                            if (pending.empty()) pending.push_back(st.selected_id);
                            for (size_t i = 0; i < pending.size(); ++i) {
                                const int id = pending[i];
                                if (!to_cut.insert(id).second) continue;
                                if (Entity* entity = st.find_entity(id)) st.clipboard.push_back(entity->deep_clone());
                                for (int child : st.children_of(id)) pending.push_back(child);
                            }
                            st.undo.push(st.entities);
                            st.entities.erase(std::remove_if(st.entities.begin(), st.entities.end(),
                                [&](const Entity& entity) { return to_cut.count(entity.value("id", -1)) != 0; }),
                                st.entities.end());
                            st.clear_selection();
                            st.resync_children_arrays();
                            transform::rebuild_registry(st.entities);
                            st.log("Cut " + std::to_string(st.clipboard.size()) + " entity(ies).");
                        }
                        continue;
                    }
                    // Ctrl+V Paste
                    if (shortcut(true,false,SDLK_v,ev)) {
                        if (!st.clipboard.empty()) {
                            st.undo.push(st.entities);
                            st.clear_selection();
                            for (auto& clip_e : st.clipboard) {
                                // Assign fresh id
                                int new_id = 0;
                                for (auto& e : st.entities) new_id = std::max(new_id, e.value("id",0));
                                ++new_id;
                                auto copy = clip_e.deep_clone();
                                copy["id"] = new_id;
                                // Offset position slightly so it's visible
                                if (copy.contains("components") && copy["components"].contains("Transform")) {
                                    copy["components"]["Transform"]["x"] = copy["components"]["Transform"].value("x",0.f) + 16.f;
                                    copy["components"]["Transform"]["y"] = copy["components"]["Transform"].value("y",0.f) + 16.f;
                                }
                                st.entities.push_back(copy);
                                st.selected_ids.push_back(new_id);
                                st.selected_id = new_id;
                            }
                            transform::mark_structure_dirty();
                            st.log("Pasted " + std::to_string(st.clipboard.size()) + " entity(ies).");
                        }
                        continue;
                    }
                    // Ctrl+D Duplicate
                    if (shortcut(true,false,SDLK_d,ev)) {
                        if (st.selected_id>=0) {
                            st.undo.push(st.entities);
                            int new_id = SceneIO::duplicate_with_descendants(st.selected_id, st);
                            if (new_id >= 0) st.select(new_id);
                        }
                        continue;
                    }
                    // Del — skip if Animator window has focus (it handles its own state deletion)
                    if (ev.key.keysym.sym==SDLK_DELETE && st.selected_id>=0 && !animator_panel->has_focus()) {
                        st.undo.push(st.entities);
                        std::vector<int> to_del;
                        {
                            std::vector<int> stack{st.selected_id};
                            while (!stack.empty()) {
                                int id = stack.back(); stack.pop_back();
                                to_del.push_back(id);
                                for (int kid : st.children_of(id)) stack.push_back(kid);
                            }
                        }
                        st.entities.erase(std::remove_if(st.entities.begin(),st.entities.end(),
                            [&](const Entity& e){ return std::find(to_del.begin(),to_del.end(),e.value("id",0))!=to_del.end(); }),st.entities.end());
                        st.clear_selection();
                        st.resync_children_arrays();
                        transform::rebuild_registry(st.entities);
                        st.log("Deleted entity.");
                        continue;
                    }
                    // Tool shortcuts — only in edit mode (typing Q/W/E/R/T while
                    // playtesting a game that uses those keys shouldn't swap tools)
                    if (ev.key.keysym.sym==SDLK_q) st.tool="select";
                    if (ev.key.keysym.sym==SDLK_w) st.tool="move";
                    if (ev.key.keysym.sym==SDLK_e) st.tool="rotate";
                    if (ev.key.keysym.sym==SDLK_r) st.tool="scale";
                    if (ev.key.keysym.sym==SDLK_t) st.tool="paint";
                }
            }
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        if (close_confirmation_requested) {
            ImGui::OpenPopup("Save changes before closing?");
            close_confirmation_requested = false;
        }

        // Full-window dockspace
        ImGuiViewport* mvp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(mvp->Pos);
        ImGui::SetNextWindowSize(mvp->Size);
        ImGui::SetNextWindowViewport(mvp->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.f,0.f});
        ImGui::Begin("##DockRoot", nullptr,
            ImGuiWindowFlags_MenuBar|ImGuiWindowFlags_NoDocking|
            ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoCollapse|
            ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|
            ImGuiWindowFlags_NoBringToFrontOnFocus|ImGuiWindowFlags_NoNavFocus);
        ImGui::PopStyleVar(3);
        ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
        ImGui::DockSpace(dockspace_id, {0,0}, ImGuiDockNodeFlags_PassthruCentralNode);

        // ── Build default Unity-style layout on first run ───────────────────────
        // Without this, ImGui has no idea where to dock each panel and they all
        // open stacked/overlapping at the same spot — making the editor look
        // broken (hidden menus, panels on top of each other, etc).
        // This only runs once: either there's no saved editor_layout.ini yet,
        // or the dockspace node itself doesn't exist in the (freshly created)
        // ImGui context, which is true on every first frame regardless of
        // whether a layout file was loaded for individual windows.
        static bool dock_layout_built = false;
        static bool visual_scripting_layout_migrated = false;
        if (!dock_layout_built) {
            dock_layout_built = true;
            ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspace_id);
            if (node == nullptr || node->IsEmpty()) {
                ImGui::DockBuilderRemoveNode(dockspace_id);
                ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_PassthruCentralNode);
                ImGui::DockBuilderSetNodeSize(dockspace_id, mvp->Size);

                ImGuiID dock_main   = dockspace_id;
                ImGuiID dock_right  = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Right, 0.20f, nullptr, &dock_main);
                ImGuiID dock_left   = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Left,  0.16f, nullptr, &dock_main);
                ImGuiID dock_bottom = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Down,  0.25f, nullptr, &dock_main);

                ImGui::DockBuilderDockWindow("Hierarchy###win", dock_left);
                ImGui::DockBuilderDockWindow("Inspector##win", dock_right);
                ImGui::DockBuilderDockWindow("Viewport##win",  dock_main);
                ImGui::DockBuilderDockWindow("Visual Scripting##evgraph", dock_main);
                ImGui::DockBuilderDockWindow("Console##win",   dock_bottom);
                ImGui::DockBuilderDockWindow("Assets##win",    dock_bottom);

                ImGui::DockBuilderFinish(dockspace_id);
            }
        }
        // Existing workspaces created before Visual Scripting became a daily
        // panel left it floating and easy to miss. Migrate only a floating or
        // not-yet-created graph window; an already docked user workspace is
        // left untouched.
        if (!visual_scripting_layout_migrated) {
            visual_scripting_layout_migrated = true;
            ImGuiWindow* graph_window = ImGui::FindWindowByName("Visual Scripting##evgraph");
            if (!graph_window || graph_window->DockId == 0) {
                if (ImGuiDockNode* center = ImGui::DockBuilderGetCentralNode(dockspace_id)) {
                    ImGui::DockBuilderDockWindow("Visual Scripting##evgraph", center->ID);
                    ImGui::DockBuilderFinish(dockspace_id);
                }
            }
        }

        // Menu bar
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("New Scene",  "Ctrl+N")) {
                    std::string new_path;
                    if (browse_for_new_scene_file(project_root, new_path)) {
                        st.scene_path = new_path;
                        st.entities.clear();
                        st.clear_selection();
                        st.scene_snapshot.clear();
                        transform::mark_structure_dirty();
                        ScriptRegistry::instance().set_active_project_from_scene_path(st.scene_path);
                        SceneIO::save(st); // create the file immediately so it exists on disk
                        st.log("New scene: " + st.scene_path);
                    }
                }
                if (ImGui::MenuItem("Open Scene", "Ctrl+O")) { if (browse_for_scene_file(project_root, st.scene_path)) SceneIO::load(st); }
                if (ImGui::MenuItem("Save",       "Ctrl+S")) { SceneIO::save(st); }
                ImGui::Separator();
                if (ImGui::BeginMenu("Recent")) {
                    for (auto& p:st.recent_scenes)
                        if (ImGui::MenuItem(p.c_str())) { st.scene_path=p; SceneIO::load(st); }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Quit")) request_editor_close();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit")) {
                if (ImGui::MenuItem("Undo","Ctrl+Z",false,st.undo.can_undo()))
                    { st.entities=st.undo.undo(st.entities); st.clear_selection(); transform::mark_structure_dirty(); }
                if (ImGui::MenuItem("Redo","Ctrl+Y",false,st.undo.can_redo()))
                    { st.entities=st.undo.redo(st.entities); st.clear_selection(); transform::mark_structure_dirty(); }
                ImGui::Separator();
                if (ImGui::MenuItem("Copy","Ctrl+C", false, st.selected_id>=0)) {
                    st.clipboard.clear();
                    for (int id : st.selected_ids) {
                        if (Entity* e = st.find_entity(id)) st.clipboard.push_back(e->deep_clone());
                    }
                    st.log("Copied " + std::to_string(st.clipboard.size()) + " entity(ies).");
                }
                if (ImGui::MenuItem("Paste","Ctrl+V", false, !st.clipboard.empty())) {
                    st.undo.push(st.entities);
                    st.clear_selection();
                    for (auto& clip_e : st.clipboard) {
                        int new_id = 0;
                        for (auto& e : st.entities) new_id = std::max(new_id, e.value("id",0));
                        ++new_id;
                        auto copy = clip_e.deep_clone();
                        copy["id"] = new_id;
                        if (copy.contains("components") && copy["components"].contains("Transform")) {
                            copy["components"]["Transform"]["x"] = copy["components"]["Transform"].value("x",0.f) + 16.f;
                            copy["components"]["Transform"]["y"] = copy["components"]["Transform"].value("y",0.f) + 16.f;
                        }
                        st.entities.push_back(copy);
                        st.selected_ids.push_back(new_id);
                        st.selected_id = new_id;
                    }
                    transform::mark_structure_dirty();
                    st.log("Pasted " + std::to_string(st.clipboard.size()) + " entity(ies).");
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Duplicate","Ctrl+D")) {
                    if (st.selected_id>=0) {
                        st.undo.push(st.entities);
                        int new_id = SceneIO::duplicate_with_descendants(st.selected_id, st);
                        if (new_id >= 0) st.select(new_id);
                    }
                }
                if (ImGui::MenuItem("Delete","Del") && st.selected_id>=0) {
                    st.undo.push(st.entities);
                    std::vector<int> to_del;
                    {
                        std::vector<int> stack{st.selected_id};
                        while (!stack.empty()) {
                            int id = stack.back(); stack.pop_back();
                            to_del.push_back(id);
                            for (int kid : st.children_of(id)) stack.push_back(kid);
                        }
                    }
                    st.entities.erase(std::remove_if(st.entities.begin(),st.entities.end(),
                        [&](const Entity& e){ return std::find(to_del.begin(),to_del.end(),e.value("id",0))!=to_del.end(); }),st.entities.end());
                    st.clear_selection();
                    st.resync_children_arrays();
                    transform::rebuild_registry(st.entities);
                }
                if (ImGui::MenuItem("Select All","Ctrl+A")) {
                    st.selected_ids.clear();
                    for (auto& e:st.entities) st.selected_ids.push_back(e.value("id",0));
                    if (!st.selected_ids.empty()) st.selected_id=st.selected_ids.back();
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Scene")) {
                if (ImGui::MenuItem("Create Empty"))    { auto e=SceneIO::make_entity("Entity",st);  st.entities.push_back(e); transform::mark_structure_dirty(); st.select(e.value("id",0)); }
                if (ImGui::MenuItem("Create Sprite"))   { auto e=SceneIO::make_entity("Sprite",st);  e["components"]["SpriteRenderer"]=component_defaults()["SpriteRenderer"]; st.entities.push_back(e); transform::mark_structure_dirty(); st.select(e.value("id",0)); }
                if (ImGui::MenuItem("Create Camera"))   { auto e=SceneIO::make_entity("Camera",st);  e["components"]["Camera2D"]=component_defaults()["Camera2D"]; st.entities.push_back(e); transform::mark_structure_dirty(); st.select(e.value("id",0)); }
                if (ImGui::MenuItem("Create Tilemap"))  { auto e=SceneIO::make_entity("Tilemap",st); e["components"]["Tilemap"]=component_defaults()["Tilemap"]; st.entities.push_back(e); transform::mark_structure_dirty(); st.select(e.value("id",0)); }
                // ── New component quick-create ────────────────────────────────
                if (ImGui::BeginMenu("Create 2D Object")) {
                    auto make = [&](const char* name, auto setup_fn) {
                        auto e = SceneIO::make_entity(name, st);
                        e["components"]["Transform"] = component_defaults()["Transform"];
                        e["components"]["Transform"]["x"] = st.cam_x;
                        e["components"]["Transform"]["y"] = st.cam_y;
                        setup_fn(e);
                        st.undo.push_deep(st.entities);
                        st.entities.push_back(e);
                        transform::mark_structure_dirty();
                        st.select(e.value("id", 0));
                    };
                    if (ImGui::MenuItem("Virtual Camera (Cinemachine)"))
                        make("Virtual Camera", [](Entity& e){ e["components"]["Cinemachine2D"] = component_defaults()["Cinemachine2D"]; });
                    if (ImGui::MenuItem("TextMesh Pro 2D"))
                        make("Text", [](Entity& e){ e["components"]["TextMeshPro2D"] = component_defaults()["TextMeshPro2D"]; });
                    if (ImGui::MenuItem("Line Renderer 2D"))
                        make("Line", [](Entity& e){ e["components"]["LineRenderer2D"] = component_defaults()["LineRenderer2D"]; });
                    if (ImGui::MenuItem("Nav Mesh Agent"))
                        make("NavAgent", [](Entity& e){
                            e["components"]["NavMeshAgent2D"] = component_defaults()["NavMeshAgent2D"];
                            e["components"]["Rigidbody2D"]    = component_defaults()["Rigidbody2D"];
                            e["components"]["BoxCollider2D"]  = component_defaults()["BoxCollider2D"];
                        });
                    if (ImGui::MenuItem("Nav Mesh Obstacle"))
                        make("NavObstacle", [](Entity& e){
                            e["components"]["NavMeshObstacle2D"] = component_defaults()["NavMeshObstacle2D"];
                            e["components"]["BoxCollider2D"]     = component_defaults()["BoxCollider2D"];
                        });
                    if (ImGui::MenuItem("Waypoint"))
                        make("Waypoint", [](Entity& e){ e["components"]["Waypoint2D"] = component_defaults()["Waypoint2D"]; });
                    if (ImGui::MenuItem("Grid + Tilemap")) {
                        // Grid parent
                        auto grid = SceneIO::make_entity("Grid", st);
                        grid["components"]["Transform"] = component_defaults()["Transform"];
                        grid["components"]["Grid2D"]    = component_defaults()["Grid2D"];
                        int grid_id = grid.value("id", 0);
                        st.undo.push_deep(st.entities);
                        st.entities.push_back(grid);
                        // Tilemap child
                        auto tm = SceneIO::make_entity("Tilemap", st);
                        tm["components"]["Transform"]           = component_defaults()["Transform"];
                        tm["components"]["Transform"]["parent"] = grid_id;
                        tm["components"]["Tilemap"]             = component_defaults()["Tilemap"];
                        st.entities.push_back(tm);
                        transform::mark_structure_dirty();
                        st.resync_children_arrays();
                        st.select(grid_id);
                    }
                    if (ImGui::MenuItem("Composite Collider 2D"))
                        make("CompositeCol", [](Entity& e){
                            e["components"]["CompositeCollider2D"] = component_defaults()["CompositeCollider2D"];
                            e["components"]["Rigidbody2D"]         = component_defaults()["Rigidbody2D"];
                            e["components"]["Rigidbody2D"]["body_type"] = "static";
                        });
                    if (ImGui::MenuItem("Hinge Joint 2D"))
                        make("HingeJoint", [](Entity& e){
                            e["components"]["HingeJoint2D"] = component_defaults()["HingeJoint2D"];
                            e["components"]["Rigidbody2D"]  = component_defaults()["Rigidbody2D"];
                        });
                    if (ImGui::MenuItem("Wheel Joint 2D (Vehicle)")) {
                        make("Wheel", [](Entity& e){
                            e["components"]["WheelJoint2D"] = component_defaults()["WheelJoint2D"];
                            e["components"]["Rigidbody2D"]  = component_defaults()["Rigidbody2D"];
                            e["components"]["CircleCollider2D"] = component_defaults()["CircleCollider2D"];
                        });
                    }
                    if (ImGui::MenuItem("Scriptable Object Ref"))
                        make("SORef", [](Entity& e){ e["components"]["ScriptableObjectRef"] = component_defaults()["ScriptableObjectRef"]; });
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                ImGui::InputInt("Grid Size",&st.grid_size);
                ImGui::Checkbox("Grid",&st.show_grid);
                ImGui::Checkbox("Snap",&st.snap);
                ImGui::Separator();
                // Unity-style "Build Settings": pick which scene Build Standalone
                // actually boots, independent of whatever scene happens to be
                // open in the editor right now. Per-project, since each
                // games/<project> folder is its own independent game.
                // Unity-style "Sorting Layers" manager (Edit > Project Settings >
                // Tags and Layers, simplified to fit this engine's single-window
                // editor). Lets the project define an ordered list of named
                // layers that SpriteRenderer/Tilemap/SortingGroup components
                // pick from via a dropdown in the Inspector, instead of raw
                // integers — exactly Unity2D's two-tier sorting model.
                if (ImGui::BeginMenu("Sorting Layers")) {
                    ImGui::TextDisabled("Bottom = drawn first (furthest back)");
                    ImGui::Separator();
                    int remove_idx = -1, move_up_idx = -1, move_down_idx = -1;
                    for (int i = 0; i < (int)st.sorting_layers.size(); ++i) {
                        ImGui::PushID(i);
                        ImGui::Text("%d.", i);
                        ImGui::SameLine();
                        ImGui::TextUnformatted(st.sorting_layers[i].c_str());
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Up") && i > 0) move_up_idx = i;
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Down") && i+1 < (int)st.sorting_layers.size()) move_down_idx = i;
                        ImGui::SameLine();
                        if (st.sorting_layers[i] != "Default" && ImGui::SmallButton("X")) remove_idx = i;
                        ImGui::PopID();
                    }
                    if (remove_idx >= 0) { st.remove_sorting_layer(st.sorting_layers[remove_idx]); }
                    if (move_up_idx >= 0) st.move_sorting_layer(move_up_idx, move_up_idx-1);
                    if (move_down_idx >= 0) st.move_sorting_layer(move_down_idx, move_down_idx+1);
                    ImGui::Separator();
                    static char new_layer_buf[64] = {};
                    ImGui::SetNextItemWidth(150);
                    ImGui::InputText("##newlayer", new_layer_buf, sizeof(new_layer_buf));
                    ImGui::SameLine();
                    if (ImGui::Button("Add Layer") && new_layer_buf[0] != '\0') {
                        if (st.add_sorting_layer(new_layer_buf)) {
                            st.log(std::string("Added sorting layer: ") + new_layer_buf);
                            new_layer_buf[0] = '\0';
                        }
                    }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                // Build configuration lives solely in Project Settings.  The
                // old Scene-menu chooser only changed a default-scene value
                // and looked like a second, incomplete build workflow.
                if (false && ImGui::BeginMenu("Build Settings")) {
                    std::string project_name = project_name_from_scene_path(std::filesystem::absolute(st.scene_path));
                    std::string current_default = st.default_scene_for(project_name);
                    std::filesystem::path scene_dir = std::filesystem::path(st.scene_path).parent_path();

                    ImGui::TextDisabled("Project: %s", project_name.c_str());
                    ImGui::TextDisabled("Default scene: %s",
                        current_default.empty() ? "(none — using currently open scene)"
                                                  : std::filesystem::path(current_default).filename().string().c_str());
                    ImGui::Separator();

                    std::error_code ec;
                    bool any = false;
                    for (auto it = std::filesystem::directory_iterator(scene_dir, ec);
                         !ec && it != std::filesystem::directory_iterator(); ++it) {
                        if (!it->is_regular_file(ec)) continue;
                        const std::filesystem::path& p = it->path();
                        if (p.extension() != ".json") continue;
                        any = true;
                        std::string rel = p.generic_string();
                        bool is_current_default = (rel == current_default);
                        if (ImGui::MenuItem(p.filename().string().c_str(), nullptr, is_current_default)) {
                            st.set_default_scene_for(project_name, rel);
                            st.log("Default scene for '" + project_name + "' set to: " + p.filename().string());
                        }
                    }
                    if (!any) ImGui::TextDisabled("No scene files found.");
                    ImGui::Separator();
                    if (ImGui::MenuItem("Clear Default (use currently open scene)")) {
                        st.default_scene_by_project.erase(project_name);
                        st.log("Default scene for '" + project_name + "' cleared.");
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Window")) {
                tool_registry.draw_window_menu();
                ImGui::Separator();
                if (ImGui::MenuItem("Reset Workspace Layout")) {
                    ImGui::ClearIniSettings();
                    // Rebuild on the next frame as well as clearing the saved
                    // layout. A workspace command that only takes effect after
                    // a restart feels broken compared with Unity/Unreal.
                    ImGui::DockBuilderRemoveNode(dockspace_id);
                    dock_layout_built = false;
                    st.log_success("Workspace layout restored to the 2D default.");
                }
                ImGui::Separator();
                // Legacy hand-maintained entries remain compiled temporarily so
                // their panel code stays available, but the registry above is
                // the single visible source for this menu.
                if (false) {
                bool anim_open = *animator_panel->open_flag();
                if (ImGui::MenuItem("Animator", nullptr, anim_open)) *animator_panel->open_flag() = !anim_open;
                bool layers_open = *animator_panel->layers_open_flag();
                if (ImGui::MenuItem("Animator Layers", nullptr, layers_open)) *animator_panel->layers_open_flag() = !layers_open;
                bool lobby_open = lobby_panel.open;
                if (ImGui::MenuItem("Play", nullptr, lobby_open)) lobby_panel.open = !lobby_open;
                ImGui::Separator();
                if (ImGui::MenuItem("Project Settings...", "Ctrl+,")) proj_settings.open();
                if (ImGui::MenuItem("Scene Statistics"))              scene_stats.open();
                if (ImGui::MenuItem("Find Object",        "Ctrl+F"))  object_picker.open();
                ImGui::Separator();
                if (ImGui::MenuItem("Sprite Slicer")) {
                    // Open with any currently selected image asset
                    if (!st.selected_asset_path.empty()) {
                        auto ext = fs::path(st.selected_asset_path).extension().string();
                        std::transform(ext.begin(),ext.end(),ext.begin(),::tolower);
                        if (ext==".png"||ext==".jpg"||ext==".bmp"||ext==".tga")
                            sprite_slicer.open(st.selected_asset_path, 512, 512); // real dims from cache in full impl
                        else
                            st.log_warn("Select an image asset in the Assets panel first.");
                    } else {
                        st.log_warn("Select an image asset in the Assets panel first.");
                    }
                }
                if (ImGui::MenuItem("Asset Import Settings")) {
                    // Mirrors Unity's per-asset Inspector import tab (texture
                    // PPU/filter/compression, audio format/quality, etc.) —
                    // opens for whatever's selected in the Assets panel, same
                    // selection source Sprite Slicer above already uses.
                    if (!st.selected_asset_path.empty())
                        asset_importer.open(st.selected_asset_path);
                    else
                        st.log_warn("Select an asset in the Assets panel first.");
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Color Palette"))      color_palette.open();
                if (ImGui::MenuItem("Templates"))          template_library.open();
                if (ImGui::MenuItem("Visual Scripting"))  event_graph.open(st);
                if (ImGui::MenuItem("Lighting"))           lighting_panel.open();
                if (ImGui::MenuItem("Audio Mixer"))        audio_mixer.open();
                if (ImGui::MenuItem("Input Manager"))      input_manager.open();
                ImGui::Separator();
                // ── New major feature panels ──────────────────────────────────
                if (ImGui::MenuItem("Prefab Manager"))        prefab_manager.open();
                if (ImGui::MenuItem("NavMesh 2D"))            navmesh_panel.open();
                if (ImGui::MenuItem("Timeline"))              timeline_panel.open();
                if (ImGui::MenuItem("Scriptable Objects"))    scriptable_obj_panel.open();
                if (ImGui::MenuItem("Physics Debugger 2D"))   physics_debugger.open();
                if (ImGui::MenuItem("Memory Profiler"))       memory_profiler.open();
                if (ImGui::MenuItem("Package Manager"))       package_manager.open();
                ImGui::Separator();
                if (ImGui::MenuItem("Sprite Editor"))         sprite_editor.open(st.selected_asset_path);
                if (ImGui::MenuItem("Shader Graph",   "Ctrl+G"))  shader_graph.open();
                if (ImGui::MenuItem("UI Builder",  "Ctrl+Shift+U")) ui_builder.open();
                if (ImGui::MenuItem("Gradient Editor"))       gradient_editor.open("Gradient",Gradient{});
                ImGui::Separator();
                if (ImGui::MenuItem("Frame Debugger", "Ctrl+Shift+F")) frame_debugger.open();
                if (ImGui::MenuItem("Undo History",   "Ctrl+Shift+H")) undo_history.open();
                if (ImGui::MenuItem("Build Report",   "Ctrl+Shift+B")) build_report.open();
                if (ImGui::MenuItem("Git VCS",        "Ctrl+Shift+G")) git_vcs.open();
                ImGui::Separator();
                if (ImGui::MenuItem("Gizmo Overlay",  "Ctrl+Shift+O")) gizmo_overlay.open();
                if (ImGui::MenuItem("Shadow 2D Settings","Ctrl+Shift+S")) shadow2d_settings.open();
                if (ImGui::MenuItem("Effector Debugger","Ctrl+Shift+E")) effector_debugger.open();
                ImGui::Separator();
                if (ImGui::MenuItem("Batch Renamer",  "Ctrl+Shift+R")) batch_renamer.open();
                if (ImGui::MenuItem("Component Search","Ctrl+Shift+C")) component_search.open();
                if (ImGui::MenuItem("Hotkeys",        "F1"))           hotkeys_panel.open();
                ImGui::Separator();
                if (ImGui::MenuItem("Profiler", "Ctrl+7")) profiler_panel.open();
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Multiplayer")) {
                bool lobby_open = lobby_panel.open;
                if (ImGui::MenuItem("Play", nullptr, lobby_open)) lobby_panel.open = !lobby_open;
                ImGui::Separator();
                if (ImGui::MenuItem("Leave Lobby", nullptr, false, Matchmaking::IsHosting() || Matchmaking::IsConnected())) Matchmaking::Leave();
                ImGui::Separator();
                ImGui::TextDisabled("Status: %s", Matchmaking::LobbySummary().c_str());
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Help")) {
                if (ImGui::MenuItem("Command Palette", "Ctrl+K")) command_palette_requested = true;
                ImGui::Separator();
                ImGui::SeparatorText("Viewport Tools");
                ImGui::TextDisabled("Q  Select    W  Move    E  Rotate");
                ImGui::TextDisabled("R  Scale     T  Paint Tile");
                ImGui::Spacing();
                ImGui::SeparatorText("Play Controls");
                ImGui::TextDisabled("F5  Play / Stop     F6  Pause");
                ImGui::TextDisabled("Escape  Stop Play");
                ImGui::Spacing();
                ImGui::SeparatorText("Editing");
                ImGui::TextDisabled("Ctrl+S  Save         Ctrl+O  Open Scene");
                ImGui::TextDisabled("Ctrl+N  New Scene    Ctrl+Z  Undo");
                ImGui::TextDisabled("Ctrl+Y  Redo         Ctrl+D  Duplicate");
                ImGui::TextDisabled("Ctrl+C  Copy         Ctrl+V  Paste");
                ImGui::TextDisabled("Ctrl+A  Select All   Del     Delete");
                ImGui::Spacing();
                ImGui::SeparatorText("Viewport Navigation");
                ImGui::TextDisabled("Middle Mouse  Pan camera");
                ImGui::TextDisabled("Scroll Wheel  Zoom in/out");
                ImGui::TextDisabled("Arrow Keys    Nudge selected entity");
                ImGui::TextDisabled("F             Frame selected entity");
                ImGui::EndMenu();
            }
            // Auto hot-reload status indicator (right side of menu bar).
            // Anchor is captured ONCE, before either status block runs — both
            // SameLine() offsets below are measured from this same fixed point.
            // (Re-calling GetContentRegionAvail() after the first Text() draws
            // would read a cursor that has already moved, making the second
            // block's target position drift left of the first one instead of
            // sitting to its right — see git history for the visible bug this
            // caused: the scene name ended up left of the hot-reload label,
            // and the two could overlap at narrower window widths.)
            float status_anchor_x = ImGui::GetContentRegionAvail().x;
            ImGui::SameLine(status_anchor_x - 360.f);
            if (AutoHotReload::instance().is_building()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.68f, 0.24f, 1.f));
                ImGui::Text("Rebuilding...");
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.65f, 0.35f, 1.f));
                ImGui::Text("Hot-reload ready");
                ImGui::PopStyleColor();
            }

            // Scene name + status on the right
            ImGui::SameLine(status_anchor_x - 180.f);
            if (st.playing) {
                ImGui::PushStyleColor(ImGuiCol_Text, st.paused ?
                    ImVec4(1.f,0.78f,0.24f,1.f) : ImVec4(0.31f,0.86f,0.47f,1.f));
                ImGui::Text(st.paused ? "PAUSED" : "PLAYING");
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f,0.5f,0.5f,1.f));
                ImGui::Text("EDIT");
                ImGui::PopStyleColor();
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f,0.65f,0.65f,1.f));
            ImGui::Text("| %s%s",
                std::filesystem::path(st.scene_path).filename().string().c_str(),
                st.undo.can_undo() ? " *" : "");
            ImGui::PopStyleColor();
            ImGui::EndMenuBar();
        }

        // ── Command Palette ────────────────────────────────────────────────
        // This intentionally contains actions rather than a second copy of
        // every menu. It covers the high-frequency paths and makes advanced
        // tooling discoverable by name without cluttering the main chrome.
        static char command_filter[128] = {};
        static bool focus_filter = true;
        if (command_palette_requested) {
            ImGui::OpenPopup("Command Palette##editor");
            command_palette_requested = false;
            focus_filter = true;
        }
        ImGui::SetNextWindowSize({560.f, 0.f}, ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                ImGuiCond_Appearing, ImVec2(0.5f, 0.22f));
        if (ImGui::BeginPopupModal("Command Palette##editor", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
            if (focus_filter) { ImGui::SetKeyboardFocusHere(); focus_filter = false; }
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##command_filter", "Type an action...", command_filter, sizeof(command_filter));
            ImGui::Separator();

            struct Command {
                std::string name;
                std::string shortcut;
                std::function<void()> run;
            };
            std::vector<Command> commands;
            commands.push_back({"Save Scene", "Ctrl+S", [&] { SceneIO::save(st); }});
            commands.push_back({st.playing ? "Stop Play Mode" : "Play Scene", "F5", [&] {
                if (st.playing) st._menubar_stop_clicked = true;
                else st._menubar_play_clicked = true;
            }});
            commands.push_back({"Pause / Resume Play Mode", "F6", [&] { if (st.playing) st.paused = !st.paused; }});
            commands.push_back({"Open Scene", "Ctrl+O", [&] {
                if (browse_for_scene_file(project_root, st.scene_path)) SceneIO::load(st);
            }});
            commands.push_back({"New Scene", "Ctrl+N", [&] {
                std::string new_path;
                if (browse_for_new_scene_file(project_root, new_path)) {
                    st.scene_path = new_path;
                    st.entities.clear(); st.clear_selection(); st.scene_snapshot.clear();
                    transform::mark_structure_dirty();
                    ScriptRegistry::instance().set_active_project_from_scene_path(st.scene_path);
                    SceneIO::save(st);
                }
            }});
            commands.push_back({"Create Empty Entity", "", [&] {
                st.undo.push_deep(st.entities);
                auto e = SceneIO::make_entity("Entity", st);
                st.entities.push_back(e); transform::mark_structure_dirty(); st.select(e.value("id", 0));
            }});
            commands.push_back({"Create Sprite", "", [&] {
                st.undo.push_deep(st.entities);
                auto e = SceneIO::make_entity("Sprite", st);
                e["components"]["SpriteRenderer"] = component_defaults()["SpriteRenderer"];
                st.entities.push_back(e); transform::mark_structure_dirty(); st.select(e.value("id", 0));
            }});
            commands.push_back({"Open Project Settings", "Ctrl+,", [&] { proj_settings.open(); }});
            commands.push_back({"Find Entity", "Ctrl+F", [&] { object_picker.open(); }});
            commands.push_back({"Open Timeline", "Ctrl+T", [&] { timeline_panel.open(); }});
            commands.push_back({"Open Profiler", "Ctrl+7", [&] { profiler_panel.open(); }});
            commands.push_back({"Open Input Manager", "", [&] { input_manager.open(); }});
            commands.push_back({"Open Audio Mixer", "", [&] { audio_mixer.open(); }});
            commands.push_back({"Open Package Manager", "Ctrl+Shift+P", [&] { package_manager.open(); }});
            commands.push_back({"Open Shader Graph", "Ctrl+G", [&] { shader_graph.open(); }});
            commands.push_back({"Open UI Builder", "Ctrl+Shift+U", [&] { ui_builder.open(); }});
            commands.push_back({"Open Hotkeys Reference", "F1", [&] { hotkeys_panel.open(); }});
            for (const auto& tool : tool_registry.entries()) {
                if (tool.is_available && !tool.is_available()) continue;
                commands.push_back({"Open " + tool.title, tool.shortcut, [open = tool.open] { open(); }});
            }

            std::string filter = command_filter;
            std::transform(filter.begin(), filter.end(), filter.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            int shown = 0;
            for (const auto& command : commands) {
                std::string haystack = command.name;
                std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (!filter.empty() && haystack.find(filter) == std::string::npos) continue;

                ImGui::PushID(command.name.c_str());
                bool activate = ImGui::Selectable(command.name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick,
                                                  ImVec2(410.f, 0.f));
                ImGui::SameLine(430.f);
                ImGui::TextDisabled("%s", command.shortcut.c_str());
                if (activate) {
                    command.run();
                    command_filter[0] = '\0';
                    focus_filter = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopID();
                ++shown;
            }
            if (shown == 0) ImGui::TextDisabled("No matching actions.");
            ImGui::Separator();
            ImGui::TextDisabled("Esc closes  •  Start typing to filter");
            // A command palette left open in Edit mode must not consume the
            // running game's pause key. Raw game keys are already withheld
            // from ImGui in the event pump, and this explicit state guard
            // also covers an ImGui key state retained from the frame Play
            // began.
            if (!st.playing && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                command_filter[0] = '\0';
                focus_filter = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::End(); // DockRoot

        viewport->end_input_frame(dt);

        // Performs the actual scripts-module swap (destroy stale instances,
        // LoadLibrary/dlopen the freshly built module, re-register) if a
        // background rebuild kicked off by AssetsPanel/start_play/the
        // toolbar button has finished. Must run on this thread, before any
        // panel below touches _script_sys this frame.
        viewport->poll_script_rebuild(st);

        draw_panel_safely("Hierarchy", [&] { hierarchy.draw(st); });
        draw_panel_safely("Inspector", [&] { inspector.draw(st); });
        viewport->set_global_lighting(shadow2d_settings.cfg.enabled,
                                      shadow2d_settings.cfg.ambient_intensity,
                                      shadow2d_settings.cfg.ambient_color,
                                      shadow2d_settings.cfg.max_lights_per_draw,
                                      shadow2d_settings.cfg.shadow_strength);
        draw_panel_safely("Viewport", [&] { viewport->draw(st, dt); });
        if (frame_debugger.take_capture_request()) {
            const auto snapshot = viewport->frame_debug_snapshot();
            frame_debugger.capture_runtime(snapshot.draw_calls, snapshot.regular_quads,
                                           snapshot.instanced_quads, snapshot.instanced_batches,
                                           snapshot.considered, snapshot.visible, snapshot.culled);
        }
        draw_panel_safely("Assets", [&] { assets->draw(st); });
        draw_panel_safely("Console", [&] { console.draw(st); });
        draw_panel_safely("Animator", [&] { animator_panel->draw(st); });
        draw_panel_safely("Animator Layers", [&] { animator_panel->draw_layers(st); });
        draw_panel_safely("Lobby", [&] { lobby_panel.draw(st); });

        // ── Unity-gap features ────────────────────────────────────────────────
        proj_settings.draw(st);
        sprite_slicer.draw(st);
        object_picker.draw(st);
        scene_stats.draw(st);
        color_palette.draw(st);
        template_library.draw(st);
        if (st.request_visual_script_open) {
            const std::string requested = st.requested_visual_script_asset;
            const int requested_entity = st.requested_visual_script_entity_id;
            st.request_visual_script_open = false;
            st.requested_visual_script_asset.clear();
            st.requested_visual_script_entity_id = -1;
            const std::filesystem::path raw(requested);
            const std::filesystem::path graph_path = raw.is_absolute() ? raw : (std::filesystem::path(st.asset_dir) / raw);
            if (const Entity* entity = st.find_entity(requested_entity))
                event_graph.open_entity_asset(requested_entity, entity->value("name", "Entity"), graph_path);
            else
                st.log_warn("Visual Script owner no longer exists; graph was not opened.");
        }
        event_graph.draw(st);
        if (st.request_tile_palette_open) {
            const std::string requested = st.requested_tile_palette_asset;
            st.request_tile_palette_open = false;
            st.requested_tile_palette_asset.clear();
            tile_palette.open(st, requested);
        }
        tile_palette.draw(st);
        lighting_panel.draw(st);
        audio_mixer.draw(st);
        input_manager.draw(st);
        asset_importer.draw(st);
        // ── New major feature panels ──────────────────────────────────────────
        if (st.request_prefab_stage_open) {
            const std::string requested = st.requested_prefab_stage_asset;
            st.request_prefab_stage_open = false;
            st.requested_prefab_stage_asset.clear();
            prefab_stage.open(requested, st);
        }
        prefab_manager.draw(st);
        prefab_stage.draw(st);
        navmesh_panel.draw(st);
        path_follower_panel.draw(st);
        timeline_panel.draw(st);
        scriptable_obj_panel.draw(st);
        physics_debugger.draw_ui(st);
        memory_profiler.draw(st);
        package_manager.draw(st);
        // ── Nova Advanced Panels ────────────────────────────────────────────
        if (st.request_sprite_editor_open) {
            const std::string requested = st.requested_sprite_editor_asset;
            st.request_sprite_editor_open = false;
            st.requested_sprite_editor_asset.clear();
            sprite_editor.open(requested);
        }
        sprite_editor.draw(st);
        if (st.request_shader_graph_open) {
            const std::string requested = st.requested_shader_graph_asset;
            st.request_shader_graph_open = false;
            st.requested_shader_graph_asset.clear();
            shader_graph.open_asset(requested, st);
        }
        shader_graph.draw(st);
        gradient_editor.draw();
        frame_debugger.draw(st);
        undo_history.draw(st);
        ui_builder.draw(st);
        build_report.draw(st);
        git_vcs.draw(st);
        gizmo_overlay.draw(st);
        shadow2d_settings.draw(st);
        effector_debugger.draw(st);
        batch_renamer.draw(st);
        component_search.draw(st);
        hotkeys_panel.draw(st);
        // ── Auto hot-reload: process any completed background builds ───────
        // Keep the running game on one stable set of script DLLs. A watched
        // source timestamp may change because of OneDrive, not a deliberate
        // save, so queue refresh work until Play has stopped.
        AutoHotReload::instance().set_play_mode_active(st.playing);
        if (AutoHotReload::instance().process_pending_swaps(viewport->script_system(), !st.playing)) {
            const std::string project = project_name_from_scene_path(st.scene_path);
            mark_scripts_registered(project);
            script_staleness_tracker(project).clear_pending();
        }

        // Profiler: push this frame's real dt + the per-stage timings
        // ViewportPanel::draw() just measured (st.frame_*_ms) before drawing
        // — push() must run after viewport->draw(st, dt) above so the
        // numbers it reads aren't left over from last frame.
        profiler_panel.push(st, dt);
        profiler_panel.draw(st);

        // Network and matchmaking must tick every frame regardless of play mode.
        // Previously guarded by !st.playing, which silently dropped all packets
        // (including keep-alives and StartMatch signals) while the game was running.
        //
        // IMPORTANT: When playing, Network::Update + ConsumeEvents is handled
        // entirely inside ViewportPanel::draw() (panels.hpp), which calls
        // Replication::HandleNetworkEvent, NetPredict::HandleNetworkEvent, and
        // EventBus in addition to Matchmaking. If we also call ConsumeEvents()
        // here while playing, we drain the queue before ViewportPanel sees it —
        // net_despawn (and net_health, net_spawn, etc.) are silently dropped on
        // the client, so killed enemies never disappear on the 2nd player's screen.
        // Only run this outer loop when NOT playing (lobby / pre-match phase).
        if (!st.playing && (Matchmaking::IsHosting() || Matchmaking::IsConnected())) {
            Network::Update(0.f);
            for (auto& ev : Network::ConsumeEvents()) {
                Matchmaking::HandleNetworkEvent(ev);
            }
        }

        // If a match just started (host pressed Start Match, or auto_start
        // fired once everyone was ready) but the user hasn't pressed the
        // editor's own ▶ Play button yet, do it for them. Without this,
        // Matchmaking::_deliver_pending_scene() (called below and every
        // frame from Matchmaking::Update) checks SceneManager::HasLoadSceneHandler(),
        // which is only installed by start_play() — so the queued scene
        // load from lobby_start just sits there forever, silently, and the
        // match never actually begins in this editor instance.
        if (!st.playing && (Matchmaking::InMatch() || Matchmaking::HasPendingSceneRequest())) {
            viewport->start_play(st);
        }

        // Only run Matchmaking::Update when a session is active.
        // Idle calls still poll discovery sockets and run timeout checks.
        if (Matchmaking::IsHosting() || Matchmaking::IsConnected() ||
            Matchmaking::InMatch() || Matchmaking::QuickMatchActive()) {
            Matchmaking::Update(dt);
        }

        // A real close guard rather than a toast: Save writes atomically and
        // keeps the editor open if it fails; Discard is explicit; Cancel
        // returns to the exact workspace without changing its scene.
        ImGui::SetNextWindowSize(ImVec2(480.f, 0.f), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Save changes before closing?", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
            ImGui::TextUnformatted("This scene has unsaved changes.");
            ImGui::Spacing();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 440.f);
            ImGui::TextDisabled("Save your work before closing GameEngine2D Pro?");
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            if (ImGui::Button("Save and Close", ImVec2(140.f, 32.f))) {
                if (SceneIO::save(st)) {
                    ImGui::CloseCurrentPopup();
                    running = false;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Discard", ImVec2(105.f, 32.f))) {
                ImGui::CloseCurrentPopup();
                running = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(105.f, 32.f)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // ── Blocking "Compiling Scripts" modal ──────────────────────────────────
        // Unity-style: while a script rebuild is running (kicked off either by
        // pressing Play, the toolbar's "Rebuild Scripts" button, or the
        // auto-trigger in AssetsPanel::draw when a new/changed script settles),
        // cover the whole window with a modal popup so the user can't edit the
        // scene, drag entities, or press Play again against a half-rebuilt
        // registry while the compiler is still running underneath on its
        // background thread (see rebuild_scripts_module in panels.hpp). The
        // panels above still draw normally first, so the UI underneath looks
        // frozen-in-place rather than blanked out, matching how Unity dims
        // (rather than hides) the editor during compilation.
        //
        // On success the actual module swap happens in
        // viewport.poll_script_rebuild(st) above, on this same frame, before
        // this block runs — so by the time we reach here `success` already
        // reflects the now-completed reload, and this modal just needs to
        // close itself rather than show anything.
        {
            // Multiple projects can now have independent rebuilds in
            // flight (see script_rebuild_state(project) in panels.hpp) —
            // find whichever one is actively building right now, if any,
            // to drive this modal. If none are currently building, fall
            // back to whichever one most recently finished (so a failure
            // message has a chance to actually be shown rather than the
            // modal just never opening because `building` flipped to
            // false again before this code ever ran).
            std::string active_project;
            bool building = false;
            const bool auto_building = AutoHotReload::instance().is_building();
            if (auto_building) {
                active_project = AutoHotReload::instance().active_project();
            } else {
                for (auto& [project, state_ptr] : _all_script_rebuild_states()) {
                    if (state_ptr->in_progress.load()) { active_project = project; building = true; break; }
                }
            }
            static std::string last_seen_project;
            if (building || auto_building) last_seen_project = active_project;
            if (active_project.empty()) active_project = last_seen_project;

            static bool was_building = false;
            const bool any_script_building = building || auto_building;
            static bool modal_was_auto_reload = false;
            if (any_script_building && !was_building) {
                modal_was_auto_reload = auto_building;
                ImGui::OpenPopup("##compiling_scripts_modal");
            }
            was_building = any_script_building;

            if (!active_project.empty()) {
                auto& rb = script_rebuild_state(active_project);

                ImVec2 center = ImGui::GetMainViewport()->GetCenter();
                ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                ImGuiWindowFlags mflags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar
                                        | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
                if (ImGui::BeginPopupModal("##compiling_scripts_modal", nullptr, mflags)) {
                    if (!any_script_building && modal_was_auto_reload) {
                        // The fast one-file pipeline has already handed its
                        // result to process_pending_swaps() above. Its
                        // diagnostics are retained in Console; unlike the
                        // legacy state it has no modal-owned failure object.
                        modal_was_auto_reload = false;
                        ImGui::CloseCurrentPopup();
                    } else if (auto_building) {
                        ImGui::Text("Compiling Scripts (%s)", active_project.c_str());
                        ImGui::Separator();
                        ImGui::Spacing();
                        static float auto_spin_t = 0.f;
                        auto_spin_t += dt;
                        const int dots = (static_cast<int>(auto_spin_t * 3.f)) % 4;
                        const std::string anim = "Working" + std::string(dots, '.');
                        ImGui::TextDisabled("%s", anim.c_str());
                        ImGui::PushTextWrapPos(420.f);
                        ImGui::TextWrapped("%s", AutoHotReload::instance().build_status().c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::Spacing();
                        ImGui::TextDisabled("The editor is locked while the script is rebuilt and reloaded.");
                    } else if (!building && rb.success.load()) {
                        // Reload already completed (poll_script_rebuild ran
                        // earlier this same frame) — nothing to show, just close.
                        ImGui::CloseCurrentPopup();
                    } else if (!building) {
                        ImGui::TextColored(ImVec4(0.95f,0.4f,0.35f,1.f), "Script rebuild failed (%s)", active_project.c_str());
                        ImGui::Separator();
                        ImGui::PushTextWrapPos(420.f);
                        ImGui::TextWrapped("%s", rb.get_message().c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::Spacing();
                        if (ImGui::Button("OK", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
                    } else {
                        ImGui::Text("Compiling Scripts (%s)", active_project.c_str());
                        ImGui::Separator();
                        ImGui::Spacing();
                        // Simple indeterminate spinner — no fixed progress
                        // fraction is available from cmake's own output, so an
                        // animated dot row communicates "still working" without
                        // implying a false percentage.
                        static float spin_t = 0.f;
                        spin_t += dt;
                        int dots = ((int)(spin_t * 3.f)) % 4;
                        std::string anim = "Working" + std::string(dots, '.');
                        ImGui::TextDisabled("%s", anim.c_str());
                        ImGui::PushTextWrapPos(420.f);
                        ImGui::TextWrapped("%s", rb.get_message().c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::Spacing();
                        ImGui::TextDisabled("The editor is locked until the new/changed script finishes building.");
                    }
                    ImGui::EndPopup();
                }
            }
        }

        // ── Blocking standalone-build modal ─────────────────────────────────
        // Exporting must never leave the editor in a half-editable state.  The
        // build itself runs on a worker only so this modal can keep rendering;
        // its popup blocks all editor interaction until CMake/MSBuild has
        // completed, matching the script-compilation contract above.
        {
            auto& standalone = standalone_build_state();
            const bool building = standalone.in_progress.load();
            const uint64_t generation = standalone.generation.load(std::memory_order_relaxed);
            static uint64_t shown_standalone_generation = 0;
            // A validation/configuration failure can complete before the next
            // frame. Keying popup creation to the request generation makes
            // that failure visible instead of accidentally skipping its modal.
            if (generation != 0 && generation != shown_standalone_generation) {
                ImGui::OpenPopup("##building_standalone_modal");
                shown_standalone_generation = generation;
            }

            ImVec2 center = ImGui::GetMainViewport()->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGuiWindowFlags mflags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar
                                    | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
            if (ImGui::BeginPopupModal("##building_standalone_modal", nullptr, mflags)) {
                if (!building && standalone.success.load()) {
                    // The success notification was written to Console; close
                    // the temporary input-blocking modal automatically.
                    ImGui::CloseCurrentPopup();
                } else if (!building) {
                    ImGui::TextColored(ImVec4(0.95f,0.4f,0.35f,1.f), "Standalone build failed (%s)",
                                       standalone.get_project().c_str());
                    ImGui::Separator();
                    ImGui::PushTextWrapPos(460.f);
                    ImGui::TextWrapped("%s", standalone.get_message().c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::Spacing();
                    if (ImGui::Button("Open Build Report", ImVec2(150, 0))) build_report.open();
                    ImGui::SameLine();
                    if (ImGui::Button("OK", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
                } else {
                    ImGui::Text("Building Standalone (%s)", standalone.get_project().c_str());
                    ImGui::Separator();
                    static float standalone_spin_time = 0.f;
                    standalone_spin_time += dt;
                    const int dots = ((int)(standalone_spin_time * 3.f)) % 4;
                    const std::string working = "Working" + std::string(dots, '.');
                    ImGui::TextDisabled("%s", working.c_str());
                    ImGui::PushTextWrapPos(460.f);
                    ImGui::TextWrapped("%s", standalone.get_message().c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::Spacing();
                    ImGui::TextDisabled("The editor is locked until the standalone build finishes.");
                    ImGui::TextDisabled("The completed game will be saved in the engine's export folder.");
                }
                ImGui::EndPopup();
            }
        }

        ImGui::Render();

        // ── Present the editor's own UI to the swapchain ────────────────────────
        // ViewportPanel.draw() above already rendered the *scene* into its own
        // offscreen vkr::RenderTarget this frame (see panels.hpp) — that image
        // is just another ImGui::Image() texture by this point, so a single
        // begin_frame()/end_frame() pair here, with ImGui's draw data recorded
        // into the same swapchain render pass, is all that's needed (mirrors
        // SDL_RenderClear + ImGui_ImplSDLRenderer2_RenderDrawData + SDL_RenderPresent).
        // Clear colour must match WindowBg exactly — any mismatch shows as
        // dark corners wherever the dockspace PassthruCentralNode is transparent
        // (areas not covered by a docked panel). WindowBg is sRGB 70 → linear.
        const float bg = powf((70.f/255.f + 0.055f) / 1.055f, 2.4f);
        VkCommandBuffer cmd = vk_backend.begin_frame(bg, bg, bg, 1.f);
        if (cmd != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        }
        vk_backend.end_frame(cmd);

        // Render detached tool windows after the primary swapchain submit.
        // Without this pair, enabling multi-viewport creates a window shell
        // but leaves it unpainted/unresponsive on the Vulkan backend.
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }
    }

    // Stop every producer while the Console callback and its `EditorState`
    // target are still valid. This closes the last teardown race between a
    // background script build and the UI/Debug statics below.
    AutoHotReload::instance().shutdown();
    shutdown_script_rebuild_workers();
    Debug::set_log_callback({});

    save_prefs(st);

    // Destroy GPU-resource-owning panels (thumbnail textures, viewport's
    // offscreen RenderTarget + its ImGui descriptor set) explicitly, before
    // the descriptor pool that owns their descriptor sets and the ImGui
    // Vulkan backend that provides RemoveTexture() are torn down below. See
    // the comment where these were declared as unique_ptr for why this
    // can't just be left to normal scope-exit destruction order.
    vk_backend.wait_idle();
    viewport.reset();
    assets.reset();
    animator_panel.reset();
    tile_palette.shutdown();
    sprite_editor.shutdown();
    prefab_stage.shutdown();

    vkDestroyDescriptorPool(vk_backend.ctx().device, imgui_desc_pool, nullptr);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

// Thin wrapper around the real entry point. Every Vulkan failure in this
// codebase (see vk_check() in vk_context.hpp) throws std::runtime_error with
// a real message — but if nothing catches it, the runtime calls
// std::terminate() and on Windows/Release builds that frequently prints
// *nothing* to stdout/stderr, which looks exactly like "window opens, goes
// black, closes a couple seconds later, no errors". This wrapper is what
// actually surfaces that message instead of losing it.
// See the matching Hub entry point: SDL2main calls SDL_main on Windows, so
// this macro-expanded entry point must have C linkage.
extern "C" int main(int argc, char* argv[]) {
    try {
        return editor_main_impl(argc, argv);
    } catch (const std::exception& e) {
        crashreport::write_note(std::string("Unhandled C++ exception: ") + e.what());
        std::cerr << "[Editor] Fatal: " << e.what() << "\n";
        return 1;
    } catch (...) {
        crashreport::write_note("Unhandled non-standard C++ exception");
        std::cerr << "[Editor] Fatal: unknown exception (non-std::exception type)\n";
        return 1;
    }
}
