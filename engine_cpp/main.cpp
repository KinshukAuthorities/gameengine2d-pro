/*
 * main.cpp — Entry point for the pure C++ standalone game engine.
 *
 * Include generated_game_scripts.hpp here so all discovered script
 * translation units are pulled in before run_game() is called.
 */

#include "core.hpp"
#include "script_system.hpp"            // ScriptRegistry for debug logging
#include "generated_game_scripts.hpp"   // generated per-project script registrations
#include "crash_reporter.hpp"

#include <iostream>
#include <string>
#include <filesystem>
#include <typeinfo>

#include <vector>

static std::filesystem::path resolve_scene_path(const std::string& raw_scene, const char* argv0) {
    namespace fs = std::filesystem;
    fs::path scene(raw_scene);

    auto exists_file = [](const fs::path& p) -> bool {
        std::error_code ec;
        return !p.empty() && fs::exists(p, ec) && fs::is_regular_file(p, ec);
    };

    auto strip_leading_rel = [](fs::path p) {
        while (!p.empty()) {
            auto s = p.generic_string();
            if (s.rfind("./", 0) == 0) {
                p = fs::path(s.substr(2));
                continue;
            }
            if (s.rfind("../", 0) == 0) {
                p = fs::path(s.substr(3));
                continue;
            }
            break;
        }
        return p;
    };

    if (exists_file(scene)) return fs::absolute(scene);

    std::vector<fs::path> bases;
    bases.push_back(fs::current_path());

    fs::path exe_dir = fs::path(argv0).parent_path();
    fs::path cur = exe_dir;
    for (int i = 0; i < 8 && !cur.empty(); ++i) {
        bases.push_back(cur);
        if (cur.has_parent_path()) cur = cur.parent_path();
        else break;
    }

    std::vector<fs::path> rels;
    rels.push_back(scene);
    auto stripped = strip_leading_rel(scene);
    if (stripped != scene) rels.push_back(stripped);
    if (scene.is_relative()) rels.push_back(scene.lexically_normal());

    for (const auto& base : bases) {
        for (const auto& rel : rels) {
            fs::path candidate = base / rel;
            if (exists_file(candidate)) return fs::absolute(candidate);
            std::error_code ec;
            auto norm = candidate.lexically_normal();
            if (exists_file(norm)) return fs::absolute(norm);
        }
    }

    return scene;
}

int main(int argc, char* argv[]) {
    try {
        std::filesystem::path exe_dir = std::filesystem::path(argv[0]).parent_path();
        if (!exe_dir.empty()) std::filesystem::current_path(exe_dir);
    } catch (...) {}

    crashreport::install("standalone", std::filesystem::current_path());

    // Standalone exports always place their designated boot scene next to the
    // executable as scene.json. Never fall back to a source-checkout sample.
    std::string scene = (argc > 1) ? argv[1] : "scene.json";
    scene = resolve_scene_path(scene, argv[0]).string();
    if (!std::filesystem::is_regular_file(scene)) {
        std::cerr << "[Engine] Fatal: Cannot locate startup scene: " << scene << "\n";
        std::cerr << "[Engine] Expected scene.json next to the game executable. "
                     "Rebuild the export from Project Settings.\n";
        return 1;
    }

    // REGISTER_SCRIPT deliberately queues registrations so hot-reload DLLs
    // can hand them to the editor host after it binds shared state.  A
    // standalone has no loader to do that handoff, so it must drain its own
    // queue before the first scene is loaded.  Without this call every script
    // looked unregistered: UIButton clicks, movement, shooting and menu
    // controllers were all inert despite being compiled into the .exe.
    scriptregistry_detail::drain_into(ScriptRegistry::instance());
    ScriptRegistry::instance().set_active_project_from_scene_path(scene);

    std::cout << "[Engine] Starting: " << scene << "\n";
    std::cout << "[Engine] Registered scripts:\n";
    for (auto& name : ScriptRegistry::instance().all_names())
        std::cout << "         + " << name << "\n";

    std::cerr << "[DEBUG] main: about to call run_game() with scene='" << scene << "'\n";
    try {
        run_game(scene, 1280, 720, false);
    } catch (const nlohmann::json::exception& je) {
        crashreport::write_note(std::string("Unhandled JSON exception: ") + je.what());
        std::cerr << "[Engine] Fatal: " << je.what() << "\n";
        std::cerr << "[DEBUG] main: caught nlohmann::json::exception, id=" << je.id << "\n";
        std::cerr << "[DEBUG] main: this is a JSON type/parse/out-of-range error - "
                     "see [DEBUG] lines above for the last JSON operation attempted\n";
        return 1;
    } catch (const std::exception& ex) {
        crashreport::write_note(std::string("Unhandled C++ exception: ") + ex.what());
        std::cerr << "[Engine] Fatal: " << ex.what() << "\n";
        std::cerr << "[DEBUG] main: caught std::exception of type "
                   << typeid(ex).name() << "\n";
        return 1;
    } catch (...) {
        crashreport::write_note("Unhandled non-standard C++ exception");
        std::cerr << "[Engine] Fatal: unknown exception\n";
        return 1;
    }
    return 0;
}
