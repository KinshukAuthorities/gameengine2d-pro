#pragma once

#include "editor_state.hpp"
#include "../../engine_cpp/net/matchmaking.hpp"

#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

// Windows native file-open dialog (IFileDialog / COM)
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>
#pragma comment(lib, "Ole32.lib")

// Returns the chosen absolute path, or empty string if cancelled.
inline std::string open_prefab_file_dialog() {
    std::string result;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
        return result;

    IFileOpenDialog* pfd = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                   IID_IFileOpenDialog, reinterpret_cast<void**>(&pfd)))) {
        COMDLG_FILTERSPEC filter{ L"Prefab Files", L"*.prefab" };
        pfd->SetFileTypes(1, &filter);
        pfd->SetDefaultExtension(L"prefab");
        pfd->SetTitle(L"Select Player Prefab");

        if (SUCCEEDED(pfd->Show(nullptr))) {
            IShellItem* psi = nullptr;
            if (SUCCEEDED(pfd->GetResult(&psi))) {
                PWSTR pszPath = nullptr;
                if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
                    int len = WideCharToMultiByte(CP_UTF8, 0, pszPath, -1,
                                                  nullptr, 0, nullptr, nullptr);
                    if (len > 0) {
                        result.resize(len - 1);
                        WideCharToMultiByte(CP_UTF8, 0, pszPath, -1,
                                            result.data(), len, nullptr, nullptr);
                    }
                    CoTaskMemFree(pszPath);
                }
                psi->Release();
            }
        }
        pfd->Release();
    }
    CoUninitialize();
    // Normalise to forward-slashes so std::filesystem handles it cleanly
    for (char& c : result) if (c == '\\') c = '/';
    return result;
}
#else
inline std::string open_prefab_file_dialog() { return {}; }
#endif

struct LobbyMatchmakingPanel {
    bool open        = false;
    bool initialized = false;

    std::vector<std::string> available_maps;
    int         selected_map = 0;
    std::string scanned_root;
    double      last_scan_time = -999.0; // ImGui time of last scan_maps call

    // Kept so editor_main.cpp's existing menu/host-now hooks still compile.
    bool        host_mode        = true;
    int         port             = 7777;
    bool        public_lobby     = true;
    bool        password_required = false;
    bool        auto_start       = true;
    bool        lan_discovery    = true;
    std::string lobby_name       = "Quick Match";
    std::string player_name      = "Player";
    std::string mode_name        = "Casual";
    std::string map_name;
    std::string region           = "LAN";
    std::string password;
    std::string build_version    = "1.0";
    bool        auto_readied     = false;

    // Full absolute path to the player prefab, forwarded to matchmaking state
    // every time push_defaults() is called.  Stored as an absolute path so
    // resolve_prefab_path() finds it on strategy #1 (try-as-is) regardless of
    // CWD — the filename-only approach only works if the prefab lives directly
    // in asset_dir, which is not guaranteed.
    std::string player_prefab_path = "AbyssPlayer.prefab";

    // ── helpers ──────────────────────────────────────────────────────────────

    static std::filesystem::path scene_project_root(const std::string& scene_path) {
        namespace fs = std::filesystem;
        fs::path p(scene_path);
        std::string gen = p.generic_string();

        auto extract_root = [&](const std::string& marker) -> fs::path {
            auto pos = gen.find(marker);
            if (pos == std::string::npos) return {};
            std::string rest = gen.substr(pos + marker.size());
            auto slash = rest.find('/');
            if (slash == std::string::npos)
                return fs::path(gen.substr(0, pos + marker.size() + rest.size()));
            return fs::path(gen.substr(0, pos + marker.size() + slash));
        };

        if (auto root = extract_root("games/"); !root.empty()) return root;
        if (auto root = extract_root("export/"); !root.empty()) return root;
        if (p.has_parent_path()) return p.parent_path();
        return fs::current_path();
    }

    static std::string scene_relative_to_project_root(const std::filesystem::path& root,
                                                       const std::string& path) {
        namespace fs = std::filesystem;
        if (path.empty()) return {};
        fs::path p(path);
        if (p.is_absolute()) {
            std::error_code ec;
            fs::path rel = fs::relative(p, root, ec);
            if (!ec) {
                std::string s = rel.lexically_normal().generic_string();
                if (!s.empty() && s.rfind("..", 0) != 0) return s;
            }
            return p.lexically_normal().generic_string();
        }
        return p.lexically_normal().generic_string();
    }

    void scan_maps(EditorState& st) {
        namespace fs = std::filesystem;
        available_maps.clear();

        fs::path root = scene_project_root(st.scene_path);
        scanned_root = root.string();

        std::error_code ec;
        if (fs::exists(root, ec) && fs::is_directory(root, ec)) {
            for (auto it = fs::recursive_directory_iterator(
                     root, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec)) continue;
                if (it->path().extension() != ".json") continue;
                available_maps.push_back(
                    scene_relative_to_project_root(root, it->path().string()));
            }
        }
        std::sort(available_maps.begin(), available_maps.end());

        std::string current_rel =
            scene_relative_to_project_root(root, st.scene_path);
        if (!current_rel.empty() &&
            std::find(available_maps.begin(), available_maps.end(),
                      current_rel) == available_maps.end()) {
            available_maps.insert(available_maps.begin(), current_rel);
        }

        if (available_maps.empty()) {
            selected_map = -1;
            map_name.clear();
        } else {
            auto it = std::find(available_maps.begin(), available_maps.end(),
                                current_rel);
            selected_map = (it != available_maps.end())
                ? (int)std::distance(available_maps.begin(), it) : 0;
            map_name = available_maps[(std::size_t)selected_map];
        }
    }

    void sync_from_state() {
        auto& s = Matchmaking::_state();
        if (!s.player_name.empty()) player_name = s.player_name;
        initialized = true;
    }

    // Pushes all settings into matchmaking state.
    // Called both from play() AND any time the prefab path changes so the
    // value is always current before the match starts.
    void push_defaults(EditorState& st) {
        Matchmaking::SetProjectName(
            project_name_from_scene_path(std::filesystem::path(st.scene_path)));
        Matchmaking::SetPlayerName(player_name);
        Matchmaking::SetLobbyName(lobby_name);
        Matchmaking::SetMapName(map_name);
        Matchmaking::SetModeName(mode_name);
        Matchmaking::SetRegion(region);
        Matchmaking::SetPassword("");
        Matchmaking::SetBuildVersion(build_version);
        Matchmaking::SetPublicLobby(true);
        Matchmaking::SetAutoStart(true);
        Matchmaking::SetLanDiscovery(true);
        Matchmaking::SetMaxPlayers(8);
        Matchmaking::SetPort((std::uint16_t)port);
        Matchmaking::SetDiscoveryPort(45454);
        // KEY FIX: push the full absolute path so resolve_prefab_path finds it
        Matchmaking::SetPlayerPrefabPath(player_prefab_path);
    }

    void play(EditorState& st) {
        if (selected_map >= 0 && selected_map < (int)available_maps.size())
            map_name = available_maps[(std::size_t)selected_map];
        push_defaults(st);
        Matchmaking::QuickMatch(mode_name, map_name, 1);
    }

    // ── draw ─────────────────────────────────────────────────────────────────

    void draw(EditorState& st) {
        if (!initialized) sync_from_state();
        if (Matchmaking::IsOpen()) open = true;
        if (!open) return;

        // scan_maps does a full recursive filesystem walk — only do it once,
        // then retry at most every 2 seconds if it found nothing.
        double now = ImGui::GetTime();
        if (available_maps.empty() && (now - last_scan_time) > 2.0) {
            scan_maps(st);
            last_scan_time = now;
        }

        bool visible = true;
        ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Play", &visible,
                          ImGuiWindowFlags_NoCollapse |
                          ImGuiWindowFlags_AlwaysAutoResize)) {
            open = visible;
            ImGui::End();
            return;
        }
        open = visible;

        bool searching          = Matchmaking::QuickMatchActive();
        bool in_match           = Matchmaking::InMatch();
        bool connected_or_hosting = Matchmaking::IsConnected() || Matchmaking::IsHosting();

        if (connected_or_hosting && !auto_readied) {
            Matchmaking::SetReady(true);
            auto_readied = true;
        } else if (!connected_or_hosting) {
            auto_readied = false;
        }

        // ── Player Prefab (only editable while idle) ──────────────────────
        if (!connected_or_hosting && !searching) {
            ImGui::Spacing();
            ImGui::TextUnformatted("Player Prefab");

            // Show only the filename in the box, store full path internally
            std::string display_name =
                std::filesystem::path(player_prefab_path).filename().string();
            if (display_name.empty()) display_name = player_prefab_path;

            ImGui::PushItemWidth(-64.f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.17f, 1.f));
            // Read-only text box — editing is done via the browse button
            char disp_buf[512];
            std::snprintf(disp_buf, sizeof(disp_buf), "%s", display_name.c_str());
            ImGui::InputText("##player_prefab_display", disp_buf, sizeof(disp_buf),
                             ImGuiInputTextFlags_ReadOnly);
            ImGui::PopStyleColor();
            ImGui::PopItemWidth();

            ImGui::SameLine();
            if (ImGui::Button("Browse##prefab", ImVec2(56.f, 0))) {
                std::string chosen = open_prefab_file_dialog();
                if (!chosen.empty()) {
                    // Store the FULL absolute path — this is what gets passed
                    // to SetPlayerPrefabPath and then resolve_prefab_path.
                    // resolve_prefab_path strategy #1 (try-as-is) succeeds
                    // immediately for an absolute path, no asset_dir needed.
                    player_prefab_path = chosen;
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Browse for a .prefab file on disk");

            // Show full path as a tooltip / small hint
            if (!player_prefab_path.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.60f, 1.f));
                ImGui::TextUnformatted(player_prefab_path.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::Spacing();
        }

        // ── Map picker ────────────────────────────────────────────────────
        ImGui::Spacing();
        ImGui::TextUnformatted("Map");
        ImGui::PushItemWidth(-1);
        bool disable_picker = searching || connected_or_hosting;
        if (disable_picker) ImGui::BeginDisabled();
        std::string current_label =
            (selected_map >= 0 && selected_map < (int)available_maps.size())
                ? available_maps[(std::size_t)selected_map]
                : "(no maps found)";
        if (ImGui::BeginCombo("##map_select", current_label.c_str())) {
            for (int i = 0; i < (int)available_maps.size(); ++i) {
                bool sel = (i == selected_map);
                if (ImGui::Selectable(available_maps[(std::size_t)i].c_str(), sel))
                    selected_map = i;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (disable_picker) ImGui::EndDisabled();
        ImGui::PopItemWidth();
        if (available_maps.empty())
            ImGui::TextDisabled("No .json scenes found under %s", scanned_root.c_str());

        ImGui::Spacing();
        ImGui::Spacing();

        // ── Action buttons ────────────────────────────────────────────────
        if (!connected_or_hosting && !searching) {
            ImGui::BeginDisabled(available_maps.empty());
            if (ImGui::Button("Play", ImVec2(-1, 48))) play(st);
            ImGui::EndDisabled();
        } else if (searching) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.30f, 0.32f, 1.f));
            ImGui::Button("Finding match...", ImVec2(-1, 48));
            ImGui::PopStyleColor();
            ImGui::TextDisabled("Joining an open lobby, or hosting one if none is found.");
        } else if (in_match) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.25f, 1.f));
            ImGui::Button("In Match", ImVec2(-1, 48));
            ImGui::PopStyleColor();
        } else {
            ImGui::TextColored(ImVec4(0.80f, 0.90f, 1.00f, 1.f), "%s",
                               Matchmaking::LobbySummary().c_str());
            ImGui::Text("Players: %d", (int)Matchmaking::Members().size());
            if (Matchmaking::IsHosting()) {
                if (ImGui::Button("Start Match", ImVec2(-1, 36)))
                    Matchmaking::StartMatch();
                ImGui::TextDisabled(
                    "Match also starts automatically once everyone is ready.");
            } else {
                ImGui::TextDisabled("Waiting for the host to start the match...");
            }
            if (ImGui::Button("Cancel", ImVec2(-1, 36))) Matchmaking::Leave();
        }

        if (!Matchmaking::LastError().empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.45f, 0.35f, 1.f));
            ImGui::TextWrapped("%s", Matchmaking::LastError().c_str());
            ImGui::PopStyleColor();
        }

        ImGui::End();
    }
};
