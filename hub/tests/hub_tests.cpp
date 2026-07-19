#include "project_manifest.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace gamehub;

namespace {

int failures = 0;

void expect(bool value, const char* description) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << description << "\n";
    }
}

void write_scene(const fs::path& path) {
    std::ofstream out(path);
    out << R"({"entities":[],"sorting_layers":["Default"]})";
}

bool is_child_of(const fs::path& child, const fs::path& parent) {
    const fs::path relative = fs::absolute(child).lexically_relative(fs::absolute(parent));
    return !relative.empty() && relative.generic_string().rfind("..", 0) != 0;
}

} // namespace

int main() {
    const fs::path sandbox = fs::temp_directory_path() /
#if defined(_WIN32)
        ("gameengine_hub_tests_" + std::to_string(GetCurrentProcessId()));
#else
        "gameengine_hub_tests";
#endif
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox / "games", ec);
    expect(!ec, "create test engine root");

    // Name validation and scene fallback for manifest-free legacy projects.
    expect(is_valid_project_id("Alpha_2"), "accept safe project identifier");
    expect(!is_valid_project_id("../escape"), "reject path traversal identifier");
    expect(!is_valid_project_id("2startsWithNumber"), "reject identifier starting with a number");
    const fs::path legacy = sandbox / "games" / "legacy";
    fs::create_directories(legacy, ec);
    write_scene(legacy / "only_scene.json");
    expect(fallback_scene(legacy) == "only_scene.json", "legacy fallback finds single root scene");
    write_scene(legacy / "scene.json");
    expect(fallback_scene(legacy) == "scene.json", "scene.json wins legacy fallback");

    // Atomic manifest saves and manifest-free discovery.
    ProjectManifest legacy_manifest_data = legacy_manifest(legacy);
    expect(legacy_manifest_data.default_scene == "scene.json", "legacy project remains readable without manifest");
    ProjectManifest manifest;
    manifest.project_id = "legacy";
    manifest.display_name = "Legacy saved";
    manifest.default_scene = "scene.json";
    manifest.created_at = now_utc_string();
    std::string error;
    expect(save_manifest(legacy, manifest, &error), "write project manifest atomically");
    expect(fs::is_regular_file(legacy / "project.json"), "manifest exists after save");
    expect(!fs::exists(legacy / "project.json.tmp"), "atomic save leaves no temporary manifest");
    const auto reread = load_manifest(legacy, false, &error);
    expect(reread && reread->display_name == "Legacy saved", "parse saved manifest");

    // Integration-style import, duplicate, and rename flow. Every output is
    // explicitly rooted in games/, never an external linked location.
    const fs::path external = sandbox / "external_source";
    fs::create_directories(external / "assets", ec);
    write_scene(external / "scene.json");
    const fs::path imported = sandbox / "games" / "imported";
    expect(copy_project_tree(external, imported, &error), "copy external project into games");
    ProjectManifest imported_manifest;
    imported_manifest.project_id = "imported";
    imported_manifest.display_name = "Imported";
    imported_manifest.default_scene = fallback_scene(imported);
    expect(save_manifest(imported, imported_manifest, &error), "save imported manifest");
    const fs::path duplicate = sandbox / "games" / "imported-copy";
    expect(copy_project_tree(imported, duplicate, &error), "duplicate project under games");
    imported_manifest.project_id = "imported-copy";
    imported_manifest.display_name = "Imported copy";
    expect(save_manifest(duplicate, imported_manifest, &error), "save duplicate manifest");
    const fs::path renamed = sandbox / "games" / "renamed";
    fs::rename(duplicate, renamed, ec);
    expect(!ec, "rename duplicated project folder");
    imported_manifest.project_id = "renamed";
    imported_manifest.display_name = "renamed";
    expect(save_manifest(renamed, imported_manifest, &error), "save renamed manifest");
    expect(is_child_of(imported, sandbox / "games") && is_child_of(renamed, sandbox / "games"),
           "import/duplicate/rename outputs stay inside games");

    // Preferences provide the persistent favorite/hidden/recent ordering.
    HubPreferences prefs;
    set_membership(prefs.favorites, "renamed", true);
    set_membership(prefs.hidden_projects, "legacy", true);
    mark_recent(prefs, "imported");
    mark_recent(prefs, "renamed");
    expect(contains(prefs.favorites, "renamed") && contains(prefs.hidden_projects, "legacy"),
           "persist project membership preferences");
    expect(!prefs.recent_projects.empty() && prefs.recent_projects.front() == "renamed",
           "recent-project order puts most recently opened first");
    expect(save_preferences(sandbox, prefs, &error), "save Hub preferences atomically");
    expect(load_preferences(sandbox).recent_projects.front() == "renamed", "read saved Hub preferences");

    // A live editor lock prevents mutation, while an invalid/dead PID is
    // recognized as stale and can be removed safely.
    {
        ProjectLock lock;
        expect(lock.acquire(imported, &error), "acquire project lock");
        expect(project_lock_state(imported) == LockState::Locked, "identify live project lock");
        nlohmann::json lock_data;
        std::ifstream lock_input(imported / kLockFilename);
        lock_input >> lock_data;
        expect(lock_data.value("mode", std::string()) == "editor" &&
               !lock_data.value("owner_token", std::string()).empty(),
               "project lock records mode and ownership token");
    }
    expect(project_lock_state(imported) == LockState::Unlocked, "release project lock on scope exit");
    {
        std::ofstream lock_file(imported / kLockFilename);
        lock_file << R"({"pid":4294967295,"opened_at":"test"})";
    }
    expect(project_lock_state(imported) == LockState::Stale, "identify stale project lock");
    expect(clear_stale_lock(imported), "clear stale project lock safely");

    // Verify the current workspace's canonical Abyss showcase remains
    // discoverable.  Nova Slash is intentionally no longer shipped or kept
    // as a live project in the release workspace.
    const fs::path workspace = fs::current_path();
    const auto current_projects = discover_projects(workspace, {});
    bool abyss_found = false;
    for (const ProjectInfo& project : current_projects) {
        abyss_found |= project.manifest.project_id == "abyss-of-hollows";
    }
    expect(abyss_found,
           "discover the canonical Abyss of Hollows showcase project");

    fs::remove_all(sandbox, ec);
    if (failures == 0) std::cout << "Hub tests passed.\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
