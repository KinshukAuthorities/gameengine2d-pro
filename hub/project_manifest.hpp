#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

// Shared project metadata and filesystem rules used by both hub.exe and the
// editor. A project is always a direct child of <engine-root>/games.
namespace gamehub {
namespace fs = std::filesystem;
using json = nlohmann::json;

inline constexpr int kProjectFormatVersion = 1;
inline constexpr const char* kManifestFilename = "project.json";
inline constexpr const char* kLockFilename = ".gameengine.lock";

inline std::string now_utc_string() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char text[32]{};
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return text;
}

inline bool is_valid_project_id(const std::string& value) {
    if (value.empty() || value.size() > 64) return false;
    if (!std::isalpha(static_cast<unsigned char>(value.front()))) return false;
    for (unsigned char c : value) {
        if (!std::isalnum(c) && c != '_' && c != '-') return false;
    }
    return true;
}

inline bool is_safe_relative_path(const fs::path& value) {
    if (value.empty() || value.is_absolute() || value.has_root_name() || value.has_root_directory()) return false;
    for (const auto& part : value) if (part == "..") return false;
    return true;
}

inline std::string fallback_scene(const fs::path& project_root) {
    std::error_code ec;
    const fs::path conventional = project_root / "scene.json";
    if (fs::is_regular_file(conventional, ec)) return "scene.json";

    std::vector<fs::path> scenes;
    for (fs::directory_iterator it(project_root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (it->path().extension() != ".json") continue;
        if (it->path().filename() == kManifestFilename) continue;
        scenes.push_back(it->path().filename());
    }
    std::sort(scenes.begin(), scenes.end());
    return scenes.empty() ? std::string{} : scenes.front().generic_string();
}

inline std::vector<std::string> project_scenes(const fs::path& project_root) {
    std::vector<std::string> scenes;
    std::error_code ec;
    for (fs::directory_iterator it(project_root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".json") continue;
        if (it->path().filename() == kManifestFilename) continue;
        scenes.push_back(it->path().filename().generic_string());
    }
    std::sort(scenes.begin(), scenes.end());
    return scenes;
}

struct ProjectManifest {
    int format_version = kProjectFormatVersion;
    std::string project_id;
    std::string display_name;
    std::string template_id = "blank-2d";
    std::string default_scene;
    std::string description;
    std::string thumbnail;
    std::string created_at;
    std::string last_opened_at;
};

inline void to_json(json& j, const ProjectManifest& m) {
    j = json{
        {"format_version", m.format_version}, {"project_id", m.project_id},
        {"display_name", m.display_name}, {"template_id", m.template_id},
        {"default_scene", m.default_scene}, {"description", m.description},
        {"thumbnail", m.thumbnail}, {"created_at", m.created_at},
        {"last_opened_at", m.last_opened_at}
    };
}

inline void from_json(const json& j, ProjectManifest& m) {
    m.format_version = j.value("format_version", kProjectFormatVersion);
    m.project_id = j.value("project_id", "");
    m.display_name = j.value("display_name", "");
    m.template_id = j.value("template_id", "blank-2d");
    m.default_scene = j.value("default_scene", "");
    m.description = j.value("description", "");
    m.thumbnail = j.value("thumbnail", "");
    m.created_at = j.value("created_at", "");
    m.last_opened_at = j.value("last_opened_at", "");
}

inline bool write_json_atomic(const fs::path& destination, const json& value,
                              std::string* error = nullptr) {
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    if (ec) {
        if (error) *error = "Could not create folder: " + ec.message();
        return false;
    }
    const fs::path temp = destination.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        if (!out) {
            if (error) *error = "Could not write " + temp.string();
            return false;
        }
        out << value.dump(2) << '\n';
        out.flush();
        if (!out) {
            if (error) *error = "Could not finish writing " + temp.string();
            return false;
        }
    }
    // Replacing an existing file must be a single rename operation: deleting
    // the old manifest first creates a crash window where the project has no
    // metadata. MoveFileEx is the Windows equivalent of POSIX replace-rename.
#if defined(_WIN32)
    if (!MoveFileExW(temp.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
#else
    fs::rename(temp, destination, ec);
#endif
    if (ec) {
        fs::remove(temp, ec);
        if (error) *error = "Could not save " + destination.string() + ": " + ec.message();
        return false;
    }
    return true;
}

inline bool save_manifest(const fs::path& project_root, const ProjectManifest& manifest,
                          std::string* error = nullptr) {
    if (!is_valid_project_id(manifest.project_id)) {
        if (error) *error = "Project folder name is invalid.";
        return false;
    }
    if (manifest.default_scene.empty() || !is_safe_relative_path(manifest.default_scene)) {
        if (error) *error = "Default scene must be a path inside the project.";
        return false;
    }
    return write_json_atomic(project_root / kManifestFilename, json(manifest), error);
}

inline ProjectManifest legacy_manifest(const fs::path& project_root) {
    ProjectManifest m;
    m.project_id = project_root.filename().string();
    m.display_name = m.project_id;
    m.template_id = "legacy";
    m.default_scene = fallback_scene(project_root);
    return m;
}

inline std::optional<ProjectManifest> load_manifest(const fs::path& project_root,
                                                     bool allow_legacy = true,
                                                     std::string* error = nullptr) {
    const fs::path path = project_root / kManifestFilename;
    std::ifstream in(path);
    if (!in) return allow_legacy ? std::optional<ProjectManifest>(legacy_manifest(project_root)) : std::nullopt;
    try {
        json data;
        in >> data;
        ProjectManifest m = data.get<ProjectManifest>();
        if (m.project_id.empty()) m.project_id = project_root.filename().string();
        if (m.display_name.empty()) m.display_name = m.project_id;
        if (m.default_scene.empty() || !is_safe_relative_path(m.default_scene))
            m.default_scene = fallback_scene(project_root);
        if (!is_valid_project_id(m.project_id) || m.default_scene.empty()) {
            if (error) *error = "Manifest is missing a valid project ID or default scene.";
            return std::nullopt;
        }
        return m;
    } catch (const std::exception& ex) {
        if (error) *error = std::string("Could not read manifest: ") + ex.what();
        return std::nullopt;
    }
}

struct HubPreferences {
    std::vector<std::string> favorites;
    std::vector<std::string> hidden_projects;
    std::vector<std::string> recent_projects;
};

inline void to_json(json& j, const HubPreferences& prefs) {
    j = json{{"favorites", prefs.favorites}, {"hidden_projects", prefs.hidden_projects},
             {"recent_projects", prefs.recent_projects}};
}

inline void from_json(const json& j, HubPreferences& prefs) {
    prefs.favorites = j.value("favorites", std::vector<std::string>{});
    prefs.hidden_projects = j.value("hidden_projects", std::vector<std::string>{});
    prefs.recent_projects = j.value("recent_projects", std::vector<std::string>{});
}

inline HubPreferences load_preferences(const fs::path& engine_root) {
    std::ifstream in(engine_root / "hub_prefs.json");
    if (!in) return {};
    try { json data; in >> data; return data.get<HubPreferences>(); }
    catch (...) { return {}; }
}

inline bool save_preferences(const fs::path& engine_root, const HubPreferences& prefs,
                             std::string* error = nullptr) {
    return write_json_atomic(engine_root / "hub_prefs.json", json(prefs), error);
}

inline bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

inline void set_membership(std::vector<std::string>& values, const std::string& value, bool enabled) {
    auto it = std::find(values.begin(), values.end(), value);
    if (enabled && it == values.end()) values.push_back(value);
    if (!enabled && it != values.end()) values.erase(it);
}

inline void mark_recent(HubPreferences& prefs, const std::string& project_id) {
    auto it = std::find(prefs.recent_projects.begin(), prefs.recent_projects.end(), project_id);
    if (it != prefs.recent_projects.end()) prefs.recent_projects.erase(it);
    prefs.recent_projects.insert(prefs.recent_projects.begin(), project_id);
    if (prefs.recent_projects.size() > 32) prefs.recent_projects.resize(32);
}

inline void rename_preference_id(HubPreferences& prefs, const std::string& old_id,
                                 const std::string& new_id) {
    for (auto* list : {&prefs.favorites, &prefs.hidden_projects, &prefs.recent_projects})
        for (auto& value : *list) if (value == old_id) value = new_id;
}

enum class LockState { Unlocked, Locked, Stale };

inline LockState project_lock_state(const fs::path& project_root) {
    const fs::path lock = project_root / kLockFilename;
    std::error_code ec;
    if (!fs::exists(lock, ec)) return LockState::Unlocked;
    std::ifstream in(lock);
    // A concurrently-created lock can be briefly unreadable while the editor
    // writes its PID. Protect it rather than removing it on that observation.
    if (!in) return LockState::Locked;
    try {
        json data; in >> data;
        const unsigned long pid = data.value("pid", 0UL);
        if (pid == 0) return LockState::Stale;
#if defined(_WIN32)
        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) {
            // Access denied can mean an editor owned by another user or an
            // elevated process. Only an invalid PID is definitely stale.
            return GetLastError() == ERROR_INVALID_PARAMETER ? LockState::Stale : LockState::Locked;
        }
        DWORD exit_code = 0;
        const bool active = GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
        CloseHandle(process);
        return active ? LockState::Locked : LockState::Stale;
#else
        return LockState::Locked;
#endif
    } catch (...) {
        // A malformed/partially-written lock is handled conservatively.
        return LockState::Locked;
    }
}

inline bool clear_stale_lock(const fs::path& project_root) {
    if (project_lock_state(project_root) != LockState::Stale) return false;
    std::error_code ec;
    return fs::remove(project_root / kLockFilename, ec);
}

class ProjectLock {
public:
    ProjectLock() = default;
    ProjectLock(const ProjectLock&) = delete;
    ProjectLock& operator=(const ProjectLock&) = delete;
    ~ProjectLock() { release(); }

    // `mode` documents who owns the project while keeping one exclusive lock
    // file for Hub/editor safety.  `editor` is the normal lifetime lock;
    // `build` is available to a standalone exporter launched without an
    // editor.  A per-acquisition token makes release ownership-safe.
    bool acquire(const fs::path& project_root, std::string* error = nullptr,
                 const std::string& mode = "editor") {
        release();
        if (project_lock_state(project_root) == LockState::Stale) clear_stale_lock(project_root);
        const fs::path lock = project_root / kLockFilename;
        const auto nonce = std::chrono::high_resolution_clock::now().time_since_epoch().count();
#if defined(_WIN32)
        _owner_token = std::to_string(static_cast<unsigned long>(GetCurrentProcessId())) + "-" +
                       std::to_string(nonce);
#else
        _owner_token = std::to_string(nonce);
#endif
#if defined(_WIN32)
        HANDLE file = CreateFileW(lock.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_HIDDEN, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            if (error) *error = "This project is already open in another editor.";
            return false;
        }
        const json data{{"pid", static_cast<unsigned long>(GetCurrentProcessId())},
                        {"opened_at", now_utc_string()}, {"mode", mode},
                        {"owner_token", _owner_token}};
        const std::string text = data.dump(2) + "\n";
        DWORD written = 0;
        const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                        written == text.size();
        CloseHandle(file);
        if (!ok) {
            std::error_code ec; fs::remove(lock, ec);
            if (error) *error = "Could not create the project lock.";
            return false;
        }
#else
        std::ofstream out(lock, std::ios::app);
        if (!out) { if (error) *error = "Could not create the project lock."; return false; }
        out << json{{"pid", 1}, {"opened_at", now_utc_string()}, {"mode", mode},
                    {"owner_token", _owner_token}}.dump(2);
#endif
        _path = lock;
        return true;
    }

    void release() {
        if (_path.empty()) return;
        // Never blindly remove a filename that may have been replaced after
        // a crash/recovery.  Only the owner token written by this instance is
        // allowed to release the live lock.
        std::error_code ec;
        bool ours = false;
        try {
            std::ifstream in(_path);
            json data;
            if (in) {
                in >> data;
                ours = data.value("owner_token", std::string()) == _owner_token;
            }
        } catch (...) {
            ours = false;
        }
        if (ours) fs::remove(_path, ec);
        _path.clear();
        _owner_token.clear();
    }

private:
    fs::path _path;
    std::string _owner_token;
};

struct ProjectInfo {
    fs::path root;
    ProjectManifest manifest;
    bool legacy = false;
    bool favorite = false;
    bool hidden = false;
    LockState lock_state = LockState::Unlocked;
};

inline std::vector<ProjectInfo> discover_projects(const fs::path& engine_root,
                                                  const HubPreferences& prefs) {
    std::vector<ProjectInfo> projects;
    std::error_code ec;
    const fs::path games = engine_root / "games";
    for (fs::directory_iterator it(games, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const auto manifest = load_manifest(it->path());
        if (!manifest) continue;
        ProjectInfo info;
        info.root = it->path();
        info.manifest = *manifest;
        info.legacy = !fs::exists(info.root / kManifestFilename, ec);
        info.favorite = contains(prefs.favorites, info.manifest.project_id);
        info.hidden = contains(prefs.hidden_projects, info.manifest.project_id);
        info.lock_state = project_lock_state(info.root);
        projects.push_back(std::move(info));
    }
    std::sort(projects.begin(), projects.end(), [](const ProjectInfo& a, const ProjectInfo& b) {
        return a.manifest.display_name < b.manifest.display_name;
    });
    return projects;
}

inline bool copy_project_tree(const fs::path& from, const fs::path& to, std::string* error = nullptr) {
    std::error_code ec;
    if (!fs::is_directory(from, ec) || fs::exists(to, ec)) {
        if (error) *error = "Source project is unavailable or the destination already exists.";
        return false;
    }
    fs::create_directories(to, ec);
    if (ec) { if (error) *error = "Could not create destination: " + ec.message(); return false; }
    const std::set<std::string> skipped_dirs{"build", ".git", ".vs", "__pycache__"};
    for (fs::recursive_directory_iterator it(from, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        const fs::path source = it->path();
        if (it->is_directory(ec) && skipped_dirs.count(source.filename().string())) {
            it.disable_recursion_pending();
            continue;
        }
        if (source.filename() == kLockFilename) continue;
        const fs::path destination = to / fs::relative(source, from, ec);
        if (ec) break;
        if (it->is_directory(ec)) fs::create_directories(destination, ec);
        else if (it->is_regular_file(ec)) fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec);
        if (ec) break;
    }
    if (ec && error) *error = "Could not copy project: " + ec.message();
    return !ec;
}

} // namespace gamehub
