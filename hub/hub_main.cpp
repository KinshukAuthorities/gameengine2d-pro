#include "project_manifest.hpp"
#include "hub_renderer.hpp"
#include "vk_render/vk_texture.hpp"
#include "third_party/stb_image.h"

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_vulkan.h>
#include <SDL2/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shellapi.h>
#  include <shobjidl.h>
#endif

namespace fs = std::filesystem;
using namespace gamehub;

namespace {

constexpr const char* kHubTitle = "GameEngine Hub";

static std::string path_text(const fs::path& path) { return path.generic_string(); }

static std::string folder_id_from_name(std::string value) {
    std::string id;
    id.reserve(value.size());
    for (const unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '_' || ch == '-') id.push_back(static_cast<char>(ch));
        else if (id.empty() || id.back() != '_') id.push_back('_');
    }
    while (!id.empty() && (id.back() == '_' || id.back() == '-')) id.pop_back();
    if (id.empty() || !std::isalpha(static_cast<unsigned char>(id.front()))) id = "Game_" + id;
    if (id.size() > 64) id.resize(64);
    while (!id.empty() && (id.back() == '_' || id.back() == '-')) id.pop_back();
    return id.empty() ? "Game" : id;
}

static std::optional<fs::path> browse_for_project_folder() {
#if defined(_WIN32)
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool should_uninitialize = SUCCEEDED(initialized);
    IFileOpenDialog* dialog = nullptr;
    fs::path chosen;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(L"Choose a project folder to copy into games");
        if (SUCCEEDED(dialog->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR selected = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &selected))) {
                    chosen = fs::path(selected);
                    CoTaskMemFree(selected);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (should_uninitialize) CoUninitialize();
    return chosen.empty() ? std::nullopt : std::optional<fs::path>(chosen);
#else
    return std::nullopt;
#endif
}

template <size_t N>
static void copy_to_buffer(std::array<char, N>& dst, const std::string& value) {
    std::snprintf(dst.data(), dst.size(), "%s", value.c_str());
}

static bool is_scene_in_project(const fs::path& root, const std::string& scene) {
    return is_safe_relative_path(fs::path(scene)) && fs::is_regular_file(root / fs::path(scene));
}

static fs::path find_engine_root() {
    fs::path start;
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH]{};
    const DWORD count = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    start = count ? fs::path(buffer).parent_path() : fs::current_path();
#else
    start = fs::current_path();
#endif
    std::error_code ec;
    for (fs::path here = start; !here.empty(); here = here.parent_path()) {
        if (fs::is_regular_file(here / "CMakeLists.txt", ec) &&
            fs::is_directory(here / "editor", ec) && fs::is_directory(here / "engine_cpp", ec))
            return here;
        const fs::path parent = here.parent_path();
        if (parent == here) break;
    }
    return fs::current_path();
}

static void hub_theme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 14.0f;
    style.FrameRounding = 9.0f;
    style.PopupRounding = 12.0f;
    style.GrabRounding = 7.0f;
    style.TabRounding = 8.0f;
    style.WindowPadding = ImVec2(20, 18);
    style.FramePadding = ImVec2(11, 8);
    style.ItemSpacing = ImVec2(10, 10);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.ScrollbarSize = 12.0f;
    ImVec4* colors = style.Colors;
    // Keep the illustration as a barely-there texture, not a blue wash.
    // The product surface itself is near-black charcoal with warm paper and
    // amber accents, so it remains readable on every monitor.
    colors[ImGuiCol_WindowBg] = ImVec4(0.020f, 0.022f, 0.018f, 0.78f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.052f, 0.052f, 0.044f, 0.92f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.068f, 0.063f, 0.050f, 0.99f);
    colors[ImGuiCol_Border] = ImVec4(0.34f, 0.30f, 0.22f, 0.52f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.f, 0.f, 0.f, 0.f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.105f, 0.096f, 0.075f, 0.96f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.155f, 0.139f, 0.103f, 0.99f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.205f, 0.178f, 0.125f, 1.0f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.055f, 0.052f, 0.043f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.62f, 0.30f, 0.105f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.78f, 0.42f, 0.15f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.45f, 0.20f, 0.060f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.15f, 0.135f, 0.10f, 0.92f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.23f, 0.198f, 0.136f, 0.98f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.29f, 0.235f, 0.145f, 1.0f);
    colors[ImGuiCol_Separator] = ImVec4(0.34f, 0.30f, 0.22f, 0.76f);
    colors[ImGuiCol_Text] = ImVec4(0.94f, 0.91f, 0.84f, 1.0f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.62f, 0.57f, 0.47f, 1.0f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.96f, 0.63f, 0.22f, 1.0f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.90f, 0.53f, 0.16f, 1.0f);
}

static void imgui_vk_check(VkResult result) {
    if (result != VK_SUCCESS) std::cerr << "[Hub] Vulkan/ImGui error: " << static_cast<int>(result) << "\n";
}

static void initialise_imgui_vulkan(HubRenderer& renderer, VkDescriptorPool pool) {
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_0;
    info.Instance = renderer.context().instance;
    info.PhysicalDevice = renderer.context().physical_device;
    info.Device = renderer.context().device;
    info.QueueFamily = *renderer.context().queue_families.graphics;
    info.Queue = renderer.context().graphics_queue;
    info.DescriptorPool = pool;
    info.MinImageCount = std::max<uint32_t>(2, renderer.swapchain().image_count());
    info.ImageCount = renderer.swapchain().image_count();
    info.PipelineInfoMain.RenderPass = renderer.swapchain().render_pass;
    info.PipelineInfoMain.Subpass = 0;
    info.UseDynamicRendering = false;
    info.CheckVkResultFn = imgui_vk_check;
    ImGui_ImplVulkan_Init(&info);
}

// A single full-window image gives the Hub an intentional product surface
// instead of the flat developer-tool appearance of a default ImGui window.
// It is independent of project thumbnails and only uploads once at startup.
class HubBackdrop {
public:
    void init(HubRenderer& renderer, const fs::path& image_path) {
        int width = 0, height = 0, channels = 0;
        stbi_uc* pixels = stbi_load(image_path.string().c_str(), &width, &height, &channels, 4);
        if (!pixels || width <= 0 || height <= 0) {
            if (pixels) stbi_image_free(pixels);
            return;
        }
        _uploader = std::make_unique<vkr::TextureUploader>(renderer.context());
        _texture = _uploader->upload(pixels, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                     vkr::FilterMode::Bilinear);
        stbi_image_free(pixels);
        if (!_texture.valid()) return;
        _descriptor = ImGui_ImplVulkan_AddTexture(_texture.sampler, _texture.image.view,
                                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    void shutdown() {
        if (_descriptor) { ImGui_ImplVulkan_RemoveTexture(_descriptor); _descriptor = VK_NULL_HANDLE; }
        if (_uploader && _texture.valid()) _uploader->destroy(_texture);
        _uploader.reset();
    }

    void draw(ImDrawList* draw, const ImVec2& position, const ImVec2& size) const {
        const ImVec2 end{position.x + size.x, position.y + size.y};
        if (_descriptor) {
            // The shipped illustration is deliberately kept quiet behind the
            // workspace.  It gives the product a recognisable home screen
            // without ever competing with project names or controls.
            draw->AddImage((ImTextureID)(intptr_t)_descriptor, position, end,
                           ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), IM_COL32(172, 150, 82, 176));
        } else {
            // Offline fallback: layered gradients, nebula glow, a perspective
            // grid and portal arcs. It is intentionally artwork rather than a
            // flat colour, so the Hub remains polished even when an install
            // has not packaged the optional raster backdrop yet.
            draw->AddRectFilledMultiColor(position, end,
                IM_COL32(18, 25, 20, 255), IM_COL32(31, 42, 28, 255),
                IM_COL32(48, 31, 23, 255), IM_COL32(15, 18, 15, 255));
            const ImVec2 moss{position.x + size.x * .78f, position.y + size.y * .22f};
            const ImVec2 ember{position.x + size.x * .16f, position.y + size.y * .92f};
            for (int ring = 22; ring >= 1; --ring) {
                const float t = static_cast<float>(ring) / 22.f;
                draw->AddCircleFilled(moss, size.y * (.08f + .42f * t), IM_COL32(102, 137, 80, static_cast<int>(3 + 8 * (1.f - t))));
                draw->AddCircleFilled(ember, size.y * (.04f + .34f * t), IM_COL32(181, 102, 47, static_cast<int>(3 + 7 * (1.f - t))));
            }
            const ImVec2 horizon{position.x + size.x * .70f, position.y + size.y * .70f};
            for (int i = -9; i <= 9; ++i) {
                const float x = position.x + size.x * .5f + i * size.x * .10f;
                draw->AddLine(horizon, {x, end.y}, IM_COL32(161, 133, 80, 42), 1.f);
            }
            for (int i = 1; i <= 8; ++i) {
                const float t = static_cast<float>(i) / 8.f;
                const float y = horizon.y + (end.y - horizon.y) * t * t;
                draw->AddLine({position.x, y}, {end.x, y}, IM_COL32(157, 132, 84, 36), 1.f);
            }
            for (int ring = 0; ring < 4; ++ring) {
                const float radius = 54.f + ring * 18.f;
                draw->PathArcTo({position.x + size.x * .84f, position.y + size.y * .34f}, radius,
                                -2.35f, .92f, 42);
                draw->PathStroke(IM_COL32(196, 144, 70, 105 - ring * 18), false, 2.f);
            }
            for (int i = 0; i < 42; ++i) {
                const float px = position.x + std::fmod(43.f * i + 137.f, std::max(1.f, size.x));
                const float py = position.y + std::fmod(83.f * i + 61.f, std::max(1.f, size.y));
                const float radius = 1.f + (i % 3) * .45f;
                draw->AddCircleFilled({px, py}, radius, IM_COL32(219, 202, 155, 32 + (i % 4) * 13));
            }
        }
        // A layered vignette preserves the illustration at the outer edges
        // while keeping every workspace surface readable.
        draw->AddRectFilledMultiColor(position, end,
            IM_COL32(12, 14, 11, 205), IM_COL32(16, 19, 14, 158),
            IM_COL32(19, 17, 12, 165), IM_COL32(10, 12, 9, 210));
        draw->AddRect(position, end, IM_COL32(192, 153, 83, 42), 0.f, 0, 1.f);
    }

private:
    std::unique_ptr<vkr::TextureUploader> _uploader;
    vkr::Texture _texture;
    VkDescriptorSet _descriptor = VK_NULL_HANDLE;
};

enum class Page { Projects, NewProject };
enum class SortMode { Recent, Name, LastOpened };

static bool hub_nav_button(const char* label, bool selected) {
    if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.46f, .22f, .075f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.61f, .31f, .11f, 1.f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.062f, .057f, .045f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.16f, .14f, .10f, 1.f));
    }
    const bool clicked = ImGui::Button(label, ImVec2(-1, 39.f));
    ImGui::PopStyleColor(selected ? 2 : 2);
    return clicked;
}

static void draw_hub_mark() {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 ember = IM_COL32(191, 106, 39, 255);
    draw->AddRectFilled(pos, ImVec2(pos.x + 38.f, pos.y + 38.f), ember, 10.f);
    draw->AddLine(ImVec2(pos.x + 11.f, pos.y + 10.f), ImVec2(pos.x + 11.f, pos.y + 28.f), IM_COL32(255, 244, 222, 255), 3.f);
    draw->AddLine(ImVec2(pos.x + 11.f, pos.y + 10.f), ImVec2(pos.x + 27.f, pos.y + 10.f), IM_COL32(255, 244, 222, 255), 3.f);
    draw->AddLine(ImVec2(pos.x + 27.f, pos.y + 10.f), ImVec2(pos.x + 27.f, pos.y + 28.f), IM_COL32(255, 244, 222, 255), 3.f);
    draw->AddLine(ImVec2(pos.x + 11.f, pos.y + 28.f), ImVec2(pos.x + 27.f, pos.y + 28.f), IM_COL32(255, 244, 222, 255), 3.f);
    ImGui::Dummy(ImVec2(38.f, 38.f));
}

struct EditorBuildTask {
    std::atomic<bool> running{false};
    std::atomic<int> exit_code{-1};
    std::thread worker;
    fs::path log_path;

    ~EditorBuildTask() { if (worker.joinable()) worker.join(); }

    void start() {
        if (running.exchange(true)) return;
        if (worker.joinable()) worker.join();
        exit_code = -1;
        worker = std::thread([this] {
            // Hub's working directory is always the engine root. Parentheses
            // ensure configuration output is captured with the build output.
            // Some processes launched from 32-bit shells inherit an ambiguous
            // VS architecture environment. Explicitly selecting AMD64 keeps
            // MSBuild on Hostx64\\x64, avoiding the HostX86 compiler stall.
#if defined(_WIN32)
            const char* command =
                "set \"PROCESSOR_ARCHITECTURE=AMD64\" && set \"PROCESSOR_ARCHITEW6432=AMD64\" && "
                "(cmake -S . -B build && cmake --build build --target editor --config Release --parallel 1 -- /p:PreferredToolArchitecture=x64) "
                "> \"build\\hub_editor_build.log\" 2>&1";
#else
            const char* command =
                "(cmake -S . -B build && cmake --build build --target editor --config Release --parallel 1) "
                "> \"build/hub_editor_build.log\" 2>&1";
#endif
            const int result = std::system(command);
            exit_code = result;
            running = false;
        });
    }

    std::string read_log() const {
        std::ifstream in(log_path);
        if (!in) return "No build output has been captured yet.";
        std::ostringstream text;
        text << in.rdbuf();
        std::string output = text.str();
        constexpr size_t kMaxLog = 24000;
        if (output.size() > kMaxLog) output = "... earlier output omitted ...\n" + output.substr(output.size() - kMaxLog);
        return output;
    }
};

class HubApp {
public:
    HubApp()
        : root(find_engine_root()), exe_dir(executable_directory()), prefs(load_preferences(root)) {
        fs::current_path(root);
        build.log_path = root / "build" / "hub_editor_build.log";
        rescan();
    }

    fs::path root;
    fs::path exe_dir;
    HubPreferences prefs;
    std::vector<ProjectInfo> projects;
    EditorBuildTask build;
    Page page = Page::Projects;
    SortMode sort = SortMode::Recent;
    bool show_hidden = false;
    bool cards = true;
    bool request_project_search_focus = false;
    // Project-card menus are child popups. A modal opened directly from one
    // would inherit that child popup level and then fail to appear once the
    // menu closes, so defer it to the Hub root window instead.
    bool request_rename_modal = false;
    bool request_duplicate_modal = false;
    std::array<char, 160> search{};
    std::string notice;
    bool notice_error = false;

    void rescan() {
        std::error_code ec;
        fs::create_directories(root / "games", ec);
        projects = discover_projects(root, prefs);
    }

    void set_notice(std::string message, bool error = false) {
        notice = std::move(message);
        notice_error = error;
    }

    fs::path editor_path() const {
        // The installed layout keeps the signed/release editor next to hub.exe.
        // A source checkout still uses CMake's conventional build output.  Check
        // the installed location first so Hub never reports a healthy installed
        // Editor as missing merely because there is no build/ directory.
        const fs::path installed = root / "editor.exe";
        if (fs::is_regular_file(installed)) return installed;
        const fs::path release = root / "build" / "editor" / "Release" / "editor.exe";
        if (fs::is_regular_file(release)) return release;
        const fs::path fallback = root / "build" / "editor" / "editor.exe";
        return fs::is_regular_file(fallback) ? fallback : release;
    }

    bool is_packaged_release() const {
        std::error_code ec;
        return fs::is_regular_file(root / "editor.exe", ec);
    }

    enum class EditorState { Ready, Missing, Outdated };

    EditorState editor_state() const {
        const auto now = std::chrono::steady_clock::now();
        if (now - editor_state_checked < std::chrono::seconds(2)) return cached_editor_state;
        editor_state_checked = now;
        const fs::path binary = editor_path();
        std::error_code ec;
        if (!fs::is_regular_file(binary, ec)) return cached_editor_state = EditorState::Missing;
        // Package builds are intentionally self-contained.  Their source
        // timestamps are not a signal that the shipped binary is stale: the
        // executable is the tested release artifact and an installed user does
        // not receive the complete editor build toolchain.
        if (is_packaged_release()) return cached_editor_state = EditorState::Ready;
        const auto exe_time = fs::last_write_time(binary, ec);
        if (ec) return cached_editor_state = EditorState::Missing;
        const auto source_time = newest_engine_source_time();
        return cached_editor_state = source_time > exe_time ? EditorState::Outdated : EditorState::Ready;
    }

    bool save_prefs() {
        std::string error;
        if (!gamehub::save_preferences(root, prefs, &error)) { set_notice(error, true); return false; }
        return true;
    }

    void open_project(ProjectInfo& project) {
        if (project.lock_state == LockState::Locked) {
            set_notice("This project is already open. Close the other editor before opening it again.", true);
            return;
        }
        if (project.lock_state == LockState::Stale) clear_stale_lock(project.root);
        if (!is_scene_in_project(project.root, project.manifest.default_scene)) {
            set_notice("The selected project's default scene is missing. Choose a valid scene in Project Settings.", true);
            return;
        }
        if (editor_state() == EditorState::Missing) {
            set_notice(is_packaged_release()
                ? "The installed editor is unavailable. Repair or reinstall GameEngine2D Pro."
                : "editor.exe is unavailable. Use Build / Refresh Editor in the sidebar first.", true);
            return;
        }
        mark_recent(prefs, project.manifest.project_id);
        if (!project.legacy) {
            project.manifest.last_opened_at = now_utc_string();
            std::string error;
            if (!save_manifest(project.root, project.manifest, &error)) { set_notice(error, true); return; }
        }
        if (!save_prefs()) return;
#if defined(_WIN32)
        const std::string arguments = "--project \"" + project.manifest.project_id + "\" --scene \"" +
                                      project.manifest.default_scene + "\"";
        const auto result = ShellExecuteA(nullptr, "open", editor_path().string().c_str(), arguments.c_str(),
                                         root.string().c_str(), SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32) {
            set_notice("Windows could not launch editor.exe.", true);
            return;
        }
        set_notice("Opening " + project.manifest.display_name + " in the editor.");
#else
        set_notice("Project launching is currently provided on Windows builds.", true);
#endif
        rescan();
    }

    void reveal(const fs::path& path) {
#if defined(_WIN32)
        ShellExecuteA(nullptr, "open", path.string().c_str(), nullptr, root.string().c_str(), SW_SHOWNORMAL);
#else
        (void)path;
#endif
    }

    bool create_blank(const std::string& id, const std::string& title, const std::string& description) {
        const fs::path destination = root / "games" / id;
        std::error_code ec;
        fs::create_directories(destination / "assets", ec);
        fs::create_directories(destination / "scripts", ec);
        if (ec) { set_notice("Could not create project folders: " + ec.message(), true); return false; }
        json scene = json{{"entities", json::array()}, {"sorting_layers", json::array({"Background", "Default", "Foreground", "UI"})}};
        std::string error;
        if (!write_json_atomic(destination / "scene.json", scene, &error)) { set_notice(error, true); return false; }
        ProjectManifest manifest;
        manifest.project_id = id;
        manifest.display_name = title.empty() ? id : title;
        manifest.template_id = "blank-2d";
        manifest.default_scene = "scene.json";
        manifest.description = description;
        manifest.created_at = now_utc_string();
        if (!save_manifest(destination, manifest, &error)) { set_notice(error, true); return false; }
        set_notice("Created " + manifest.display_name + " under games/" + id + ".");
        return true;
    }

    bool create_from_template(const std::string& id, const std::string& title, const std::string& description,
                              const std::string& template_id) {
        // Release packaging places templates next to hub.exe.  Keep the old
        // hub/templates location as a migration fallback for development
        // staging folders created by earlier releases.
        fs::path source = exe_dir / "templates" / template_id;
        if (!fs::is_directory(source)) source = exe_dir / "hub" / "templates" / template_id;
        if (!fs::is_directory(source)) {
            set_notice("The " + template_id + " template was not packaged with hub.exe. Rebuild the Hub target.", true);
            return false;
        }
        return copy_into_project(source, id, title, description, template_id, "Created");
    }

    bool copy_into_project(const fs::path& source, const std::string& id, const std::string& title,
                           const std::string& description, const std::string& template_id,
                           const std::string& action) {
        if (!validate_new_id(id)) return false;
        const std::string scene = fallback_scene(source);
        if (scene.empty()) { set_notice("The source has no root JSON scene to open.", true); return false; }
        const fs::path destination = root / "games" / id;
        std::string error;
        if (!copy_project_tree(source, destination, &error)) { set_notice(error, true); return false; }
        ProjectManifest manifest;
        if (const auto loaded = load_manifest(destination); loaded) manifest = *loaded;
        manifest.project_id = id;
        manifest.display_name = title.empty() ? id : title;
        manifest.template_id = template_id;
        manifest.default_scene = is_scene_in_project(destination, manifest.default_scene) ? manifest.default_scene : fallback_scene(destination);
        manifest.description = description.empty() ? manifest.description : description;
        manifest.created_at = now_utc_string();
        manifest.last_opened_at.clear();
        if (!save_manifest(destination, manifest, &error)) { set_notice(error, true); return false; }

        // The shipped Abyss template includes prebuilt native script modules.
        // Clone those into a namespace matching the new folder so the first
        // editor launch can register its gameplay immediately. The module ABI
        // receives the destination project id at load time, so this remains
        // correct for every Hub-created project without a startup recompile.
        if (template_id == "abyss-of-hollows") {
            const std::string source_namespace = "abyss_of_hollows";
            std::string destination_namespace = id;
            for (char& c : destination_namespace)
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
            const fs::path modules = root / "build" / "scripts_module_fast" /
                                     source_namespace / "Release";
            const fs::path module_destination = root / "build" / "scripts_module_fast" /
                                                destination_namespace / "Release";
            std::error_code module_ec;
            int copied_modules = 0;
            if (fs::is_directory(modules, module_ec)) {
                fs::create_directories(module_destination, module_ec);
                const std::string prefix = source_namespace + "_";
                for (fs::directory_iterator it(modules, module_ec), end;
                     !module_ec && it != end; it.increment(module_ec)) {
                    if (!it->is_regular_file(module_ec) || it->path().extension() != ".dll") continue;
                    const std::string name = it->path().filename().string();
                    if (name.rfind(prefix, 0) != 0) continue;
                    const fs::path target = module_destination /
                        (destination_namespace + name.substr(source_namespace.size()));
                    fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, module_ec);
                    if (!module_ec) ++copied_modules;
                }
            }
            if (module_ec || copied_modules == 0) {
                set_notice("Project files were created, but bundled Abyss script modules could not be staged. Repair the installation.", true);
                return false;
            }
        }
        set_notice(action + " project as games/" + id + ".");
        return true;
    }

    bool validate_new_id(const std::string& id) {
        if (!is_valid_project_id(id)) {
            set_notice("Project folders must start with a letter and use only letters, numbers, _ or - (max 64).", true);
            return false;
        }
        std::error_code ec;
        if (fs::exists(root / "games" / id, ec)) {
            set_notice("games/" + id + " already exists. Choose a different project name.", true);
            return false;
        }
        return true;
    }

    bool rename_project(ProjectInfo& project, const std::string& next_id) {
        if (project.lock_state == LockState::Locked) { set_notice("Close the editor before renaming this project.", true); return false; }
        if (next_id == project.manifest.project_id) return true;
        if (!validate_new_id(next_id)) return false;
        const fs::path destination = root / "games" / next_id;
        std::error_code ec;
        fs::rename(project.root, destination, ec);
        if (ec) { set_notice("Could not rename project folder: " + ec.message(), true); return false; }
        ProjectManifest manifest = project.manifest;
        manifest.project_id = next_id;
        manifest.display_name = next_id; // Folder rename intentionally changes the project title too.
        std::string error;
        if (!save_manifest(destination, manifest, &error)) {
            // Files remain safe under their new name; surface the save failure rather than attempting a risky rollback.
            set_notice("Folder renamed, but manifest update failed: " + error, true);
            return false;
        }
        rename_preference_id(prefs, project.manifest.project_id, next_id);
        save_prefs();
        set_notice("Renamed project to " + next_id + ".");
        return true;
    }

    void hide_project(const ProjectInfo& project, bool hide) {
        set_membership(prefs.hidden_projects, project.manifest.project_id, hide);
        if (save_prefs()) { rescan(); set_notice(hide ? "Removed from the Hub list. Files were not deleted." : "Project restored to the Hub list."); }
    }

    void set_favorite(const ProjectInfo& project, bool favorite) {
        set_membership(prefs.favorites, project.manifest.project_id, favorite);
        if (save_prefs()) rescan();
    }

private:
    mutable EditorState cached_editor_state = EditorState::Missing;
    mutable std::chrono::steady_clock::time_point editor_state_checked{};

    static fs::path executable_directory() {
#if defined(_WIN32)
        wchar_t buffer[MAX_PATH]{};
        const DWORD count = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        if (count) return fs::path(buffer).parent_path();
#endif
        return fs::current_path();
    }

    fs::file_time_type newest_engine_source_time() const {
        std::error_code ec;
        fs::file_time_type newest = fs::last_write_time(root / "CMakeLists.txt", ec);
        for (const fs::path relative : {fs::path("editor"), fs::path("engine_cpp")}) {
            for (fs::recursive_directory_iterator it(root / relative, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec)) {
                if (it->is_directory(ec) && (it->path().filename() == "build" || it->path().filename() == "third_party")) {
                    it.disable_recursion_pending();
                    continue;
                }
                if (!it->is_regular_file(ec)) continue;
                const auto when = it->last_write_time(ec);
                if (!ec && when > newest) newest = when;
            }
            ec.clear();
        }
        return newest;
    }
};

struct NewProjectForm {
    std::array<char, 80> id{};
    std::array<char, 160> title{};
    std::array<char, 512> description{};
    int template_index = 0;
};

struct ProjectSettingsForm {
    std::string project_id;
    std::array<char, 160> title{};
    std::array<char, 512> description{};
    std::array<char, 260> thumbnail{};
    int scene_index = 0;
    bool favorite = false;
};

struct ProjectActionForm {
    std::string project_id;
    std::array<char, 80> value{};
};

static ProjectInfo* find_project(HubApp& app, const std::string& id) {
    for (auto& project : app.projects) if (project.manifest.project_id == id) return &project;
    return nullptr;
}

static std::string unique_import_id(const HubApp& app, const std::string& folder_name) {
    const std::string base = folder_id_from_name(folder_name);
    std::error_code ec;
    if (!fs::exists(app.root / "games" / base, ec)) return base;
    for (int copy = 2; copy < 10000; ++copy) {
        const std::string suffix = "-" + std::to_string(copy);
        std::string candidate = base.substr(0, std::min<std::size_t>(base.size(), 64 - suffix.size())) + suffix;
        ec.clear();
        if (!fs::exists(app.root / "games" / candidate, ec)) return candidate;
    }
    return base;
}

static bool matches_filter(const ProjectInfo& project, const std::string& search) {
    if (search.empty()) return true;
    auto lower = [](std::string text) {
        for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    };
    const std::string needle = lower(search);
    return lower(project.manifest.display_name).find(needle) != std::string::npos ||
           lower(project.manifest.project_id).find(needle) != std::string::npos ||
           lower(project.manifest.description).find(needle) != std::string::npos;
}

static int recent_rank(const HubPreferences& prefs, const std::string& id) {
    const auto it = std::find(prefs.recent_projects.begin(), prefs.recent_projects.end(), id);
    return it == prefs.recent_projects.end() ? 99999 : static_cast<int>(it - prefs.recent_projects.begin());
}

static void project_status_line(const ProjectInfo& project) {
    if (project.lock_state == LockState::Locked) ImGui::TextColored(ImVec4(1.f, .67f, .25f, 1.f), "Open in editor - protected");
    else if (project.lock_state == LockState::Stale) ImGui::TextColored(ImVec4(1.f, .79f, .31f, 1.f), "Stale lock detected");
    else if (project.legacy) ImGui::TextColored(ImVec4(.86f, .68f, .34f, 1.f), "Legacy project - manifest will be created when saved");
    else ImGui::TextDisabled("Default scene: %s", project.manifest.default_scene.c_str());
}

static ImU32 project_accent(const std::string& id) {
    static constexpr ImU32 palette[] = {
        IM_COL32(138, 108, 55, 255), IM_COL32(111, 78, 76, 255),
        IM_COL32(82, 116, 76, 255), IM_COL32(177, 88, 45, 255),
        IM_COL32(126, 83, 113, 255)
    };
    unsigned int hash = 2166136261u;
    for (const unsigned char ch : id) hash = (hash ^ ch) * 16777619u;
    return palette[hash % (sizeof(palette) / sizeof(palette[0]))];
}

static void begin_project_settings(ProjectInfo& project, ProjectSettingsForm& settings) {
    settings.project_id = project.manifest.project_id;
    copy_to_buffer(settings.title, project.manifest.display_name);
    copy_to_buffer(settings.description, project.manifest.description);
    copy_to_buffer(settings.thumbnail, project.manifest.thumbnail);
    settings.favorite = project.favorite;
    const auto scenes = project_scenes(project.root);
    settings.scene_index = 0;
    for (size_t i = 0; i < scenes.size(); ++i)
        if (scenes[i] == project.manifest.default_scene) settings.scene_index = static_cast<int>(i);
    ImGui::OpenPopup("Project Settings");
}

static void show_project_card(HubApp& app, ProjectInfo& project, ProjectSettingsForm& settings,
                              ProjectActionForm& rename, ProjectActionForm& duplicate) {
    const std::string id = project.manifest.project_id;
    ImGui::PushID(id.c_str());
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 15.f);
    ImGui::BeginChild("card", ImVec2(0, 290), true, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    const ImVec2 card_pos = ImGui::GetWindowPos();
    const ImVec2 card_size = ImGui::GetWindowSize();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 accent = project_accent(id);
    const ImVec2 art_end{card_pos.x + card_size.x, card_pos.y + 98.f};
    draw->AddRectFilledMultiColor(card_pos, art_end,
        IM_COL32(27, 27, 21, 255), accent, IM_COL32(38, 30, 22, 255), IM_COL32(18, 19, 15, 255));
    draw->AddRectFilled(ImVec2(card_pos.x, card_pos.y + 82.f), art_end, IM_COL32(11, 12, 9, 96));
    const ImVec2 orb{card_pos.x + card_size.x * .80f, card_pos.y + 28.f};
    draw->AddCircleFilled(orb, 42.f, IM_COL32(255, 255, 255, 15));
    draw->AddCircle(orb, 28.f, IM_COL32(247, 232, 193, 110), 28, 1.5f);
    draw->AddCircle(orb, 18.f, accent, 28, 2.0f);
    draw->AddLine(ImVec2(card_pos.x + 17.f, card_pos.y + 76.f),
                  ImVec2(card_pos.x + card_size.x - 17.f, card_pos.y + 76.f),
                  IM_COL32(239, 221, 175, 60), 1.f);
    ImGui::Dummy(ImVec2(0.f, 86.f));
    ImGui::TextColored(ImVec4(.88f, .74f, .46f, 1.f), "%s", project.favorite ? "FAVORITE PROJECT" : "PROJECT");
    ImGui::SameLine();
    const char* state = project.lock_state == LockState::Locked ? "OPEN" : project.legacy ? "LEGACY" : "READY";
    const ImVec4 state_color = project.lock_state == LockState::Locked ? ImVec4(1.f, .68f, .28f, 1.f) :
                              project.legacy ? ImVec4(.86f, .68f, .34f, 1.f) : ImVec4(.48f, .78f, .48f, 1.f);
    ImGui::TextColored(state_color, "%s", state);
    ImGui::TextUnformatted(project.manifest.display_name.c_str());
    ImGui::TextDisabled("games/%s  |  %s", id.c_str(), project.manifest.default_scene.c_str());
    project_status_line(project);
    if (!project.manifest.description.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextDisabled("%s", project.manifest.description.c_str());
        ImGui::PopTextWrapPos();
    }
    const bool locked = project.lock_state == LockState::Locked;
    const float footer_y = ImGui::GetWindowContentRegionMax().y - 39.f;
    if (ImGui::GetCursorPosY() < footer_y) ImGui::SetCursorPosY(footer_y);
    const float open_width = std::max(102.f, (ImGui::GetContentRegionAvail().x - 8.f) * .69f);
    if (ImGui::Button("Open project", ImVec2(open_width, 31.f))) app.open_project(project);
    ImGui::SameLine();
    if (ImGui::Button("Manage", ImVec2(-1, 31.f))) ImGui::OpenPopup("actions");
    if (ImGui::BeginPopup("actions")) {
        if (ImGui::MenuItem(project.favorite ? "Remove favorite" : "Favorite")) app.set_favorite(project, !project.favorite);
        if (ImGui::MenuItem("Project settings", nullptr, false, !locked)) {
            begin_project_settings(project, settings);
        }
        if (ImGui::MenuItem("Rename", nullptr, false, !locked)) {
            rename.project_id = id; copy_to_buffer(rename.value, id); app.request_rename_modal = true;
        }
        if (ImGui::MenuItem("Duplicate", nullptr, false, !locked)) {
            duplicate.project_id = id; copy_to_buffer(duplicate.value, id + "-copy"); app.request_duplicate_modal = true;
        }
        if (ImGui::MenuItem("Reveal in Explorer")) app.reveal(project.root);
        if (project.lock_state == LockState::Stale && ImGui::MenuItem("Clear stale lock")) {
            if (clear_stale_lock(project.root)) { app.set_notice("Cleared stale project lock."); app.rescan(); }
        }
        if (ImGui::MenuItem(project.hidden ? "Restore to Hub list" : "Remove from Hub list")) app.hide_project(project, !project.hidden);
        ImGui::EndPopup();
    }
    ImGui::EndChild();
    ImGui::PopID();
}

static void show_projects_page(HubApp& app, ProjectSettingsForm& settings,
                               ProjectActionForm& rename, ProjectActionForm& duplicate) {
    int visible_project_count = 0;
    int favorite_count = 0;
    int locked_count = 0;
    for (const auto& project : app.projects) if (app.show_hidden || !project.hidden) ++visible_project_count;
    for (const auto& project : app.projects) {
        if (project.hidden && !app.show_hidden) continue;
        if (project.favorite) ++favorite_count;
        if (project.lock_state == LockState::Locked) ++locked_count;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 16.f);
    ImGui::BeginChild("projects_overview", ImVec2(0.f, 142.f), true, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    const ImVec2 hero_pos = ImGui::GetWindowPos();
    const ImVec2 hero_size = ImGui::GetWindowSize();
    const ImVec2 hero_end{hero_pos.x + hero_size.x, hero_pos.y + hero_size.y};
    ImDrawList* hero_draw = ImGui::GetWindowDrawList();
    hero_draw->AddRectFilledMultiColor(hero_pos, hero_end,
        IM_COL32(36, 39, 28, 238), IM_COL32(69, 63, 35, 232),
        IM_COL32(66, 42, 27, 232), IM_COL32(27, 30, 22, 240));
    const ImVec2 hero_orb{hero_pos.x + hero_size.x * .84f, hero_pos.y + hero_size.y * .45f};
    for (int ring = 4; ring >= 1; --ring)
        hero_draw->AddCircle(hero_orb, 26.f + ring * 18.f, IM_COL32(236, 194, 108, 22 + ring * 13), 40, 1.5f);
    hero_draw->AddLine(ImVec2(hero_pos.x + 20.f, hero_end.y - 24.f), ImVec2(hero_end.x - 20.f, hero_end.y - 24.f),
                       IM_COL32(233, 205, 140, 55), 1.f);
    ImGui::SetCursorPos(ImVec2(22.f, 16.f));
    ImGui::TextDisabled("GAMEENGINE 2D  /  PROJECT WORKSPACE");
    ImGui::SetCursorPos(ImVec2(22.f, 39.f));
    ImGui::Text("Make your next world.");
    ImGui::TextDisabled("%d project%s stored in games/", visible_project_count, visible_project_count == 1 ? "" : "s");
    ImGui::TextDisabled("%d favorite%s  ·  %d currently open", favorite_count, favorite_count == 1 ? "" : "s", locked_count);
    ImGui::TextDisabled("Open a project, create a new game, or bring an existing project into this engine.");
    const float new_button_x = std::max(22.f, hero_size.x - 154.f);
    ImGui::SetCursorPos(ImVec2(new_button_x, 52.f));
    if (ImGui::Button("New project", ImVec2(128.f, 36.f))) app.page = Page::NewProject;
    ImGui::EndChild();
    if (!app.notice.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, app.notice_error ? ImVec4(1.f, .48f, .48f, 1.f) : ImVec4(.43f, .88f, .58f, 1.f));
        ImGui::TextWrapped("%s", app.notice.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    ImGui::SetNextItemWidth(320);
    if (app.request_project_search_focus) {
        ImGui::SetKeyboardFocusHere();
        app.request_project_search_focus = false;
    }
    ImGui::InputTextWithHint("##search", "Search by project name or folder", app.search.data(), app.search.size());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Search projects (Ctrl+F)");
    ImGui::SameLine();
    const char* sort_items[] = {"Recent", "Name", "Last opened"};
    int sort_index = static_cast<int>(app.sort);
    ImGui::SetNextItemWidth(130);
    if (ImGui::Combo("##sort", &sort_index, sort_items, IM_ARRAYSIZE(sort_items))) app.sort = static_cast<SortMode>(sort_index);
    ImGui::SameLine(); ImGui::Checkbox("Show hidden", &app.show_hidden);
    ImGui::SameLine(); ImGui::Checkbox("Card view", &app.cards);
    ImGui::SameLine();
    if (ImGui::Button("Rescan", ImVec2(80.f, 0.f))) { app.rescan(); app.set_notice("Project folders rescanned."); }

    std::vector<ProjectInfo*> visible;
    for (auto& project : app.projects) if ((app.show_hidden || !project.hidden) && matches_filter(project, app.search.data())) visible.push_back(&project);
    std::sort(visible.begin(), visible.end(), [&](const ProjectInfo* left, const ProjectInfo* right) {
        if (left->favorite != right->favorite) return left->favorite > right->favorite;
        if (app.sort == SortMode::Name) return left->manifest.display_name < right->manifest.display_name;
        if (app.sort == SortMode::LastOpened) return left->manifest.last_opened_at > right->manifest.last_opened_at;
        return recent_rank(app.prefs, left->manifest.project_id) < recent_rank(app.prefs, right->manifest.project_id);
    });
    ImGui::Spacing();
    if (visible.empty()) {
        ImGui::BeginChild("empty_projects", ImVec2(0.f, 120.f), true);
        ImGui::Text("No matching projects");
        ImGui::TextDisabled("Create a project, or rescan after adding one under games/.");
        if (ImGui::Button("Create your first project")) app.page = Page::NewProject;
        ImGui::EndChild();
    }

    if (app.cards) {
        const float card_width = 350.f;
        const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / card_width));
        if (ImGui::BeginTable("project_cards", columns, ImGuiTableFlags_SizingStretchSame)) {
            for (size_t i = 0; i < visible.size(); ++i) {
                ImGui::TableNextColumn();
                show_project_card(app, *visible[i], settings, rename, duplicate);
            }
            ImGui::EndTable();
        }
    } else if (ImGui::BeginTable("project_list", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Project"); ImGui::TableSetupColumn("Folder"); ImGui::TableSetupColumn("Default scene");
        ImGui::TableSetupColumn("State"); ImGui::TableSetupColumn("Action"); ImGui::TableHeadersRow();
        for (ProjectInfo* project : visible) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(project->manifest.display_name.c_str());
            ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("%s", project->manifest.project_id.c_str());
            ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("%s", project->manifest.default_scene.c_str());
            ImGui::TableSetColumnIndex(3); project_status_line(*project);
            ImGui::TableSetColumnIndex(4); ImGui::PushID(project->manifest.project_id.c_str());
            if (ImGui::SmallButton("Open")) app.open_project(*project);
            ImGui::SameLine(); if (ImGui::SmallButton("Reveal")) app.reveal(project->root);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    std::vector<std::string> remembered_ids;
    for (const auto* ids : {&app.prefs.hidden_projects, &app.prefs.favorites, &app.prefs.recent_projects})
        for (const std::string& id : *ids) if (!contains(remembered_ids, id)) remembered_ids.push_back(id);
    bool first_missing = true;
    for (const std::string& id : remembered_ids) {
        if (find_project(app, id)) continue;
        if (first_missing) { ImGui::Spacing(); ImGui::TextDisabled("Missing project entries"); first_missing = false; }
        ImGui::BulletText("%s is not currently found under games/", id.c_str());
        ImGui::SameLine(); ImGui::PushID(("forget" + id).c_str());
        if (ImGui::SmallButton("Forget")) {
            set_membership(app.prefs.hidden_projects, id, false);
            set_membership(app.prefs.favorites, id, false);
            set_membership(app.prefs.recent_projects, id, false);
            app.save_prefs();
        }
        ImGui::PopID();
    }
}

static void show_new_project_page(HubApp& app, NewProjectForm& form) {
    const float available = ImGui::GetContentRegionAvail().x;
    const float form_width = std::min(620.f, available * .62f);
    ImGui::BeginChild("new_project_form", ImVec2(form_width, 0.f), true);
    ImGui::TextDisabled("CREATE / PROJECT");
    ImGui::Text("Start with a solid foundation.");
    ImGui::TextDisabled("Projects are always created safely inside this engine's games/ folder.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Project folder");
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextWithHint("##project_folder", "for example: moonlit-ruins", form.id.data(), form.id.size());
    ImGui::TextDisabled("Letters first; letters, numbers, _ and - after that.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Display name");
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextWithHint("##display_name", "Name shown in the Hub", form.title.data(), form.title.size());
    ImGui::Spacing();
    ImGui::TextUnformatted("Description");
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextMultiline("##description", form.description.data(), form.description.size(), ImVec2(-1.f, 92.f));
    ImGui::Spacing();
    ImGui::TextUnformatted("Template");
    // Blank 2D is the built-in empty-project option. Abyss of Hollows is the
    // sole shipped game template; do not expose retired sample projects here.
    const char* templates[] = {"Blank 2D", "Abyss of Hollows sample"};
    ImGui::SetNextItemWidth(-1.f);
    ImGui::Combo("##template", &form.template_index, templates, IM_ARRAYSIZE(templates));
    const std::string id = form.id.data();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("DESTINATION");
    ImGui::TextWrapped("%s", path_text(app.root / "games" / (id.empty() ? "<project-name>" : id)).c_str());
    ImGui::Spacing();
    if (ImGui::Button("Create project", ImVec2(148.f, 36.f))) {
        const std::string title = form.title.data();
        const std::string description = form.description.data();
        bool created = false;
        if (!app.validate_new_id(id)) {}
        else if (form.template_index == 0) created = app.create_blank(id, title, description);
        else created = app.create_from_template(id, title, description, "abyss-of-hollows");
        if (created) { app.rescan(); app.page = Page::Projects; form = {}; }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(96.f, 36.f))) app.page = Page::Projects;
    ImGui::EndChild();
    ImGui::SameLine(0.f, 16.f);
    ImGui::BeginChild("new_project_preview", ImVec2(0.f, 0.f), true, ImGuiWindowFlags_NoScrollbar);
    const ImVec2 preview_pos = ImGui::GetWindowPos();
    const ImVec2 preview_size = ImGui::GetWindowSize();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 crest{preview_pos.x + preview_size.x * .72f, preview_pos.y + 95.f};
    draw->AddCircleFilled(crest, 64.f, IM_COL32(189, 112, 46, 24));
    draw->AddCircle(crest, 45.f, IM_COL32(241, 194, 105, 105), 40, 2.f);
    draw->AddCircle(crest, 28.f, IM_COL32(117, 143, 83, 145), 40, 2.f);
    ImGui::SetCursorPos(ImVec2(24.f, 24.f));
    ImGui::TextDisabled("TEMPLATE PREVIEW");
    ImGui::SetCursorPos(ImVec2(24.f, 53.f));
    ImGui::TextUnformatted(templates[form.template_index]);
    ImGui::SetCursorPos(ImVec2(24.f, 82.f));
    const char* description = form.template_index == 0 ?
        "An empty 2D scene with Assets and Scripts folders, ready for your first prototype." :
        "A complete dark-fantasy action-adventure sample with scenes, assets, and controls.";
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + preview_size.x - 48.f);
    ImGui::TextDisabled("%s", description);
    ImGui::PopTextWrapPos();
    ImGui::SetCursorPos(ImVec2(24.f, std::max(196.f, preview_size.y - 132.f)));
    ImGui::Separator();
    ImGui::TextDisabled("HUB GUARANTEES");
    ImGui::BulletText("No external destination or linked project folders");
    ImGui::BulletText("A manifest and a valid default scene are created");
    ImGui::BulletText("Templates copy source files, never build caches");
    ImGui::EndChild();
}

static void show_tools_page(HubApp& app) {
    ImGui::Text("Tools and status");
    ImGui::Separator();
    const HubApp::EditorState state = app.editor_state();
    const char* state_text = state == HubApp::EditorState::Ready ? "Ready" : state == HubApp::EditorState::Missing ? "Missing" : "Outdated";
    const ImVec4 state_color = state == HubApp::EditorState::Ready ? ImVec4(.43f, .88f, .58f, 1.f) : ImVec4(1.f, .70f, .28f, 1.f);
    ImGui::Text("Editor: "); ImGui::SameLine(); ImGui::TextColored(state_color, "%s", state_text);
    ImGui::TextDisabled("%s", path_text(app.editor_path()).c_str());
    if (app.is_packaged_release()) {
        ImGui::TextDisabled("This installed release is ready to launch. Editor rebuilds are intentionally disabled here.");
        ImGui::TextDisabled("Use a newer GameEngine2D Pro installer to update the Editor.");
    } else if (app.build.running) {
        ImGui::TextColored(ImVec4(.92f, .68f, .29f, 1.f), "Building editor in the background...");
        ImGui::BeginDisabled(); ImGui::Button("Build / Refresh Editor"); ImGui::EndDisabled();
    } else {
        if (ImGui::Button("Build / Refresh Editor")) { app.build.start(); app.set_notice("Editor build started. Output is shown below."); }
        if (app.build.exit_code >= 0) ImGui::TextColored(app.build.exit_code == 0 ? ImVec4(.43f, .88f, .58f, 1.f) : ImVec4(1.f, .42f, .42f, 1.f),
                                                         app.build.exit_code == 0 ? "Last build completed successfully." : "Last build failed. See diagnostics below.");
    }
    ImGui::Spacing(); ImGui::Text("Build diagnostics");
    std::string log = app.build.read_log();
    ImGui::InputTextMultiline("##build_log", log.data(), log.size() + 1, ImVec2(-1, 320), ImGuiInputTextFlags_ReadOnly);
}

static void show_project_dialogs(HubApp& app, ProjectSettingsForm& settings,
                                 ProjectActionForm& rename, ProjectActionForm& duplicate) {
    if (ImGui::BeginPopupModal("Project Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ProjectInfo* project = find_project(app, settings.project_id);
        if (!project) { ImGui::Text("Project no longer exists."); if (ImGui::Button("Close")) ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
        const bool locked = project->lock_state == LockState::Locked;
        if (locked) ImGui::TextColored(ImVec4(1.f, .68f, .26f, 1.f), "Close the editor to change project settings.");
        ImGui::BeginDisabled(locked);
        ImGui::InputText("Title", settings.title.data(), settings.title.size());
        ImGui::InputTextMultiline("Description", settings.description.data(), settings.description.size(), ImVec2(420, 85));
        ImGui::InputText("Thumbnail path", settings.thumbnail.data(), settings.thumbnail.size());
        auto scenes = project_scenes(project->root);
        if (scenes.empty()) ImGui::TextColored(ImVec4(1.f, .42f, .42f, 1.f), "No root JSON scenes found.");
        else {
            settings.scene_index = std::clamp(settings.scene_index, 0, static_cast<int>(scenes.size() - 1));
            if (ImGui::BeginCombo("Default scene", scenes[settings.scene_index].c_str())) {
                for (int i = 0; i < static_cast<int>(scenes.size()); ++i) {
                    const bool selected = settings.scene_index == i;
                    if (ImGui::Selectable(scenes[i].c_str(), selected)) settings.scene_index = i;
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
        ImGui::Checkbox("Favorite", &settings.favorite);
        if (ImGui::Button("Save")) {
            if (scenes.empty()) app.set_notice("A project needs a root JSON scene before its settings can be saved.", true);
            else if (std::string(settings.thumbnail.data()).size() > 0 &&
                     !is_safe_relative_path(fs::path(settings.thumbnail.data()))) {
                app.set_notice("Thumbnail path must be relative to the project folder.", true);
            }
            else {
                ProjectManifest manifest = project->manifest;
                manifest.project_id = project->root.filename().string();
                manifest.display_name = std::string(settings.title.data()).empty() ? manifest.project_id : settings.title.data();
                manifest.description = settings.description.data();
                manifest.thumbnail = settings.thumbnail.data();
                manifest.default_scene = scenes[settings.scene_index];
                if (manifest.created_at.empty()) manifest.created_at = now_utc_string();
                std::string error;
                if (save_manifest(project->root, manifest, &error)) {
                    set_membership(app.prefs.favorites, manifest.project_id, settings.favorite);
                    app.save_prefs(); app.rescan(); app.set_notice("Project settings saved."); ImGui::CloseCurrentPopup();
                } else app.set_notice(error, true);
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine(); if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Rename Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Rename changes the games/ folder and the project title.");
        ImGui::InputText("New folder/title", rename.value.data(), rename.value.size());
        if (ImGui::Button("Rename")) {
            if (ProjectInfo* project = find_project(app, rename.project_id)) {
                if (app.rename_project(*project, rename.value.data())) { app.rescan(); ImGui::CloseCurrentPopup(); }
            } else { app.set_notice("Project no longer exists.", true); ImGui::CloseCurrentPopup(); }
        }
        ImGui::SameLine(); if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Duplicate Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("The copy is always created under games/.");
        ImGui::InputText("New folder", duplicate.value.data(), duplicate.value.size());
        if (ImGui::Button("Duplicate")) {
            if (ProjectInfo* project = find_project(app, duplicate.project_id)) {
                if (project->lock_state == LockState::Locked) app.set_notice("Close the editor before duplicating this project.", true);
                else if (app.copy_into_project(project->root, duplicate.value.data(), duplicate.value.data(), "", "duplicate", "Duplicated")) {
                    app.rescan(); ImGui::CloseCurrentPopup();
                }
            } else { app.set_notice("Project no longer exists.", true); ImGui::CloseCurrentPopup(); }
        }
        ImGui::SameLine(); if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

static void show_import_modal(HubApp& app, ProjectActionForm& import) {
    if (ImGui::BeginPopupModal("Import Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static fs::path source;
        static std::string source_title;
        ImGui::TextWrapped("Import copies a project into games/. The Hub never links an external folder.");
        ImGui::Spacing();
        if (ImGui::Button("Browse PC...", ImVec2(150.f, 34.f))) {
            if (const auto chosen = browse_for_project_folder()) {
                source = *chosen;
                source_title = source.filename().string();
                copy_to_buffer(import.value, unique_import_id(app, source_title));
            }
        }
        if (source.empty()) {
            ImGui::TextDisabled("Choose the external project folder to copy.");
        } else {
            ImGui::TextColored(ImVec4(.91f, .76f, .47f, 1.f), "%s", source_title.c_str());
            ImGui::TextDisabled("Selected from your PC");
            ImGui::Text("Destination: games/%s", import.value.data());
        }
        if (ImGui::Button("Import copy")) {
            if (source.empty() || !fs::is_directory(source)) app.set_notice("Choose an existing external project folder with Browse PC.", true);
            else if (app.copy_into_project(source, import.value.data(), source_title, "", "import", "Imported")) {
                app.rescan(); source.clear(); source_title.clear(); ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine(); if (ImGui::Button("Cancel")) { source.clear(); source_title.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

static const char* hub_page_title(Page page) {
    switch (page) {
    case Page::Projects:   return "Projects";
    case Page::NewProject: return "New project";
    }
    return "GameEngine Hub";
}

static bool show_hub_sidebar(HubApp& app) {
    bool request_import = false;
    ImGui::BeginChild("hub_sidebar", ImVec2(240.f, 0.f), true, ImGuiWindowFlags_NoScrollbar);
    const ImVec2 sidebar_pos = ImGui::GetWindowPos();
    const ImVec2 sidebar_size = ImGui::GetWindowSize();
    ImDrawList* sidebar_draw = ImGui::GetWindowDrawList();
    sidebar_draw->AddRectFilledMultiColor(sidebar_pos, ImVec2(sidebar_pos.x + sidebar_size.x, sidebar_pos.y + 104.f),
        IM_COL32(45, 45, 31, 247), IM_COL32(83, 65, 35, 238), IM_COL32(66, 42, 29, 238), IM_COL32(31, 33, 25, 247));
    sidebar_draw->AddCircle(ImVec2(sidebar_pos.x + 194.f, sidebar_pos.y + 34.f), 31.f, IM_COL32(238, 194, 108, 75), 36, 1.5f);
    sidebar_draw->AddLine(ImVec2(sidebar_pos.x + 16.f, sidebar_pos.y + 104.f),
                         ImVec2(sidebar_pos.x + sidebar_size.x - 16.f, sidebar_pos.y + 104.f),
                         IM_COL32(231, 204, 142, 60), 1.f);
    ImGui::SetCursorPos(ImVec2(18.f, 21.f));
    draw_hub_mark();
    ImGui::SameLine(65.f);
    ImGui::BeginGroup();
    ImGui::Text("GAMEENGINE 2D");
    ImGui::TextDisabled("PROJECT HUB  /  VULKAN");
    ImGui::EndGroup();
    ImGui::SetCursorPosY(122.f);
    ImGui::TextDisabled("WORKSPACE");
    if (hub_nav_button("Projects", app.page == Page::Projects)) app.page = Page::Projects;
    if (hub_nav_button("New project", app.page == Page::NewProject)) app.page = Page::NewProject;
    ImGui::Spacing();
    ImGui::TextDisabled("PROJECT ACTIONS");
    if (ImGui::Button("Import project", ImVec2(-1, 36.f))) {
        request_import = true;
    }
    if (ImGui::Button("Open games folder", ImVec2(-1, 34.f))) app.reveal(app.root / "games");

    const float status_y = ImGui::GetWindowContentRegionMax().y - 118.f;
    if (ImGui::GetCursorPosY() < status_y) ImGui::SetCursorPosY(status_y);
    ImGui::Separator();
    const HubApp::EditorState editor_state = app.editor_state();
    const bool ready = editor_state == HubApp::EditorState::Ready;
    ImGui::TextDisabled("EDITOR BUILD");
    ImGui::TextColored(ready ? ImVec4(.43f, .88f, .58f, 1.f) : ImVec4(1.f, .70f, .28f, 1.f),
                       "%s", ready ? "Ready to launch" : editor_state == HubApp::EditorState::Missing ? "Editor missing" : "Refresh available");
    if (app.build.running) {
        ImGui::TextDisabled("Building editor in background...");
    } else if (!ready && ImGui::Button("Build / Refresh Editor", ImVec2(-1, 31.f))) {
        app.build.start();
        app.set_notice("Editor build started in the background.");
    }
    ImGui::EndChild();
    return request_import;
}

int hub_main() {
    HubApp app;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) {
        std::cerr << "[Hub] SDL_Init failed: " << SDL_GetError() << "\n";
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow(kHubTitle, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1420, 880,
                                          SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_VULKAN);
    if (!window) { std::cerr << "[Hub] " << SDL_GetError() << "\n"; SDL_Quit(); return 1; }

    try {
        HubRenderer renderer(window);
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128};
        VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 128;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &size;
        vkr::vk_check(vkCreateDescriptorPool(renderer.context().device, &pool_info, nullptr, &pool), "create Hub ImGui pool");

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.IniFilename = "hub_layout.ini";
        hub_theme();
        ImGui_ImplSDL2_InitForVulkan(window);
        initialise_imgui_vulkan(renderer, pool);
        HubBackdrop backdrop;
        fs::path backdrop_asset = app.exe_dir / "assets" / "hub_background.png";
        if (!fs::is_regular_file(backdrop_asset)) backdrop_asset = app.root / "hub" / "assets" / "hub_background.png";
        backdrop.init(renderer, backdrop_asset);

        NewProjectForm new_project;
        ProjectSettingsForm settings;
        ProjectActionForm rename;
        ProjectActionForm duplicate;
        ProjectActionForm import;
        bool running = true;
        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL2_ProcessEvent(&event);
                if (event.type == SDL_QUIT) running = false;
                if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window)) running = false;
            }
            renderer.recreate_if_possible();
            if (renderer.needs_recreate()) { SDL_Delay(16); continue; }

            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplSDL2_NewFrame();
            ImGui::NewFrame();
            // Hub-wide keyboard actions stay available without forcing users
            // to hunt through the sidebar. They deliberately do not launch a
            // project, so there is no accidental external state change.
            if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N)) {
                app.page = Page::NewProject;
            }
            if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_R)) {
                app.rescan();
                app.set_notice("Project folders rescanned.");
            }
            if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F)) {
                app.page = Page::Projects;
                app.request_project_search_focus = true;
            }
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->WorkPos);
            ImGui::SetNextWindowSize(ImGui::GetMainViewport()->WorkSize);
            ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
            ImGui::Begin("Hub", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);
            backdrop.draw(ImGui::GetWindowDrawList(), ImGui::GetWindowPos(), ImGui::GetWindowSize());
            const bool request_import = show_hub_sidebar(app);
            ImGui::SameLine(0.f, 14.f);
            ImGui::BeginChild("hub_content", ImVec2(0.f, 0.f), false, ImGuiWindowFlags_NoScrollbar);
            ImGui::Text("%s", hub_page_title(app.page));
            ImGui::Separator();
            ImGui::Spacing();
            if (app.page == Page::Projects) show_projects_page(app, settings, rename, duplicate);
            else show_new_project_page(app, new_project);
            ImGui::EndChild();
            if (request_import) { import = {}; ImGui::OpenPopup("Import Project"); }
            if (app.request_rename_modal) { ImGui::OpenPopup("Rename Project"); app.request_rename_modal = false; }
            if (app.request_duplicate_modal) { ImGui::OpenPopup("Duplicate Project"); app.request_duplicate_modal = false; }
            show_project_dialogs(app, settings, rename, duplicate);
            show_import_modal(app, import);
            ImGui::End();

            ImGui::Render();
            VkCommandBuffer command = renderer.begin_frame(.055f, .067f, .10f, 1.f);
            if (command != VK_NULL_HANDLE) ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), command);
            renderer.end_frame(command);
            if (renderer.needs_recreate()) {
                vkDeviceWaitIdle(renderer.context().device);
                ImGui_ImplVulkan_Shutdown();
                renderer.recreate_if_possible();
                if (!renderer.needs_recreate()) initialise_imgui_vulkan(renderer, pool);
            }
        }
        vkDeviceWaitIdle(renderer.context().device);
        backdrop.shutdown();
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        vkDestroyDescriptorPool(renderer.context().device, pool, nullptr);
    } catch (const std::exception& error) {
        std::cerr << "[Hub] Fatal: " << error.what() << "\n";
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

} // namespace

// SDL2main provides the Windows entry point and calls SDL_main. SDL's own
// headers intentionally macro-expand main to SDL_main, which must use C
// linkage (otherwise MSVC C++ name-mangles it and the link fails).
extern "C" int main(int /*argc*/, char** /*argv*/) {
    return hub_main();
}
