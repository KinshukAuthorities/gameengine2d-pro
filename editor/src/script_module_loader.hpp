#pragma once
// Shared flag checked by panels.hpp to skip its own auto-rebuild
// when AutoHotReload is already watching the filesystem. Defined
// here so it's visible to both panels.hpp and auto_hot_reload.hpp
// without a circular include dependency.
inline bool& ahr_active_flag() {
    static bool flag = false;
    return flag;
}

// script_module_loader.hpp — loads/reloads a project's game_scripts module
// without restarting the editor process.
//
// This replaces the old approach (scripts compiled directly into editor.exe,
// requiring a full relink + relaunch of the editor on every script change)
// with the same pattern Unreal Engine uses for C++ hot reload: gameplay code
// lives in a separate shared library, loaded at runtime, and reloading means
// swapping which library is loaded — not restarting the host process.
//
// Each PROJECT gets its own independently loaded module (game_scripts_<ns>),
// tracked here by project name. Rebuilding/reloading game4's module has no
// effect on game3's already-loaded one — neither at the CMake/build level
// (see editor/scripts_module/CMakeLists.txt) nor here at load time. This
// matters once a project has enough scripts that recompiling everything on
// every edit, anywhere, becomes the slow part of iterating.
//
// What this does NOT do (and why): true zero-restart C++ hot-swapping of a
// class's exact in-memory layout is not generally safe (changing a class's
// member variables, base classes, or vtable shape while old instances of
// that exact class still exist in memory is undefined behavior in both
// Unreal's Hot Reload and this implementation). The safe contract here,
// matching Unreal's: every live ScriptBase instance created from the
// currently-loaded module is destroyed BEFORE that module is unloaded, then
// recreated (awake() runs again) against the freshly loaded module. State
// that needs to survive a reload belongs in the Entity/scene data (which is
// untouched by any of this), not in a script's C++ member variables.
#include "../../engine_cpp/script_system.hpp"
#include "../../engine_cpp/net/network.hpp"
#include "../../engine_cpp/net/matchmaking.hpp"

#include <filesystem>
#include <string>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <regex>
#include <cstdlib>
#include <cstdint>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <dlfcn.h>
#endif

namespace fs = std::filesystem;

// C++ compilers create and constantly rewrite .obj/.pch/.pdb files.  Keeping
// those volatile intermediates inside a synced game-engine checkout lets
// OneDrive briefly lock them, which turns an ordinary script save into C1083
// "Permission denied".  Keep the cache in the user's local app-data area;
// source files and final loaded DLLs are still selected per project, and the
// root hash prevents two engine installations with the same game folder name
// from sharing an intermediate tree.
inline fs::path script_module_build_dir(const fs::path& engine_root,
                                        const std::string& project_namespace) {
#if defined(_WIN32)
    if (const char* local_app_data = std::getenv("LOCALAPPDATA");
        local_app_data && *local_app_data) {
        // FNV-1a is deliberately deterministic across launches/toolchains;
        // std::hash is not required to have that persistence property.
        std::uint64_t engine_hash = 1469598103934665603ull;
        for (unsigned char c : fs::absolute(engine_root).generic_string()) {
            engine_hash ^= c;
            engine_hash *= 1099511628211ull;
        }
        const std::string engine_key = std::to_string(engine_hash);
        return fs::path(local_app_data) / "GameEngine2DPro" / "script-cache" /
               engine_key / project_namespace;
    }
#endif
    // Non-Windows and unusual restricted environments retain the existing,
    // project-isolated layout as a safe fallback.
    return engine_root / "build" / "scripts_module_fast" / project_namespace;
}

// Signature of the exported function every built game_scripts_<project>
// module must provide (see editor/scripts_module/CMakeLists.txt's generated
// source). Takes pointers to every piece of process-wide state this
// header-only API would otherwise instantiate separately inside the DLL:
// the HOST's ScriptRegistry::instance(), scriptfields::InstanceRegistry::
// instance() (EXPOSE_FIELD), Screen's state (current viewport/window size),
// Input's state (current frame's input snapshot), and SceneManager's state
// (the installed LoadScene handler). The module redirects its own copies of
// all of them to whichever ones it's handed, rather than assuming it can reach
// the host's globals/statics on its own — DLLs do not share the host exe's
// function-local statics or header-only inline globals (see each of their
// own comments in script_system.hpp / unity2d_script_api.hpp for why this
// matters).
using RegisterAllScriptsFn = void(*)(ScriptRegistry*, scriptfields::InstanceRegistry*, Screen::State*, Input::State*, SceneManager::State*, Network::State*, Matchmaking::State*, EventBus::State*, Debug::State*, const char*);

class ScriptModuleLoader {
public:
    static ScriptModuleLoader& instance() {
        static ScriptModuleLoader inst;
        return inst;
    }

    // True once `project`'s module has been loaded and registered at least
    // once this process.
    bool is_loaded(const std::string& project) const {
        auto it = _modules.find(project);
        return it != _modules.end() && !it->second.empty();
    }

    fs::path loaded_path(const std::string& project) const {
        // For backward compat: return the first loaded module's path
        auto it = _modules.find(project);
        if (it == _modules.end() || it->second.empty()) return fs::path();
        return it->second.begin()->second.path;
    }

    // Unload ALL modules for a project (both per-script and monolithic).
    // Calls FreeLibrary/dlclose on every handle and removes the DLL files.
    // The caller must have already destroyed every live ScriptBase instance
    // (ScriptSystem::reset_all_instances()) before calling this.
    void unload_project(const std::string& project) {
        auto it = _modules.find(project);
        if (it == _modules.end()) return;
        // All project factories point into the modules being unloaded.
        ScriptRegistry::instance().clear_project(project);
        for (auto& [key, mod] : it->second) {
            if (mod.handle) {
#if defined(_WIN32)
                FreeLibrary(static_cast<HMODULE>(mod.handle));
#else
                dlclose(mod.handle);
#endif
                mod.handle = nullptr;
                // best-effort cleanup of the module file
                std::error_code ec;
                for (int attempt = 0; attempt < 20; ++attempt) {
                    if (!fs::exists(mod.path, ec)) break;
                    if (fs::remove(mod.path, ec)) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));
                }
            }
        }
        _modules.erase(it);
    }

    // `script_key` is the stable module key, normally
    // "<project>::<source-file-stem>". It deliberately is not a class name:
    // one file may define several classes and those classes may be renamed.
    bool unload_script(const std::string& project, const std::string& script_key) {
        auto project_it = _modules.find(project);
        if (project_it == _modules.end()) return false;
        auto module_it = project_it->second.find(script_key);
        if (module_it == project_it->second.end()) return false;

        LoadedModule& mod = module_it->second;
        for (const auto& factory_key : mod.factory_keys)
            ScriptRegistry::instance().unreg(factory_key);
        if (mod.handle) {
#if defined(_WIN32)
            FreeLibrary(static_cast<HMODULE>(mod.handle));
#else
            dlclose(mod.handle);
#endif
            mod.handle = nullptr;
            _remove_module_file(mod.path);
        }
        project_it->second.erase(module_it);
        if (project_it->second.empty()) _modules.erase(project_it);
        return true;
    }

    // Loads `module_path` as the module for `project`, calls its
    // RegisterAllScripts(host_registry), and — if a previous module was
    // already loaded for this SAME project+script combo — unloads that one
    // afterward. Order matters: load-and-register the NEW module first, so
    // there is never a moment where the script is unregistered at all.
    //
    // `script_key` is optional: if non-empty (e.g. "novaSlash::AbyssPlayer"),
    // the module is tracked per-script. If empty, `project` is used as the
    // key (monolithic module containing many scripts).
    //
    // IMPORTANT: the caller must have already destroyed every live ScriptBase
    // instance that came from the PREVIOUS module for this key before calling
    // this — see ScriptSystem::reset_all_instances(). Any ScriptBase* still
    // alive from the old module becomes a dangling vtable call the instant
    // that FreeLibrary/dlclose runs.
    bool load(const std::string& project, const fs::path& module_path,
              std::string* out_error, const std::string& script_key = {},
              std::vector<std::string> factory_keys = {}) {
        std::string key = script_key.empty() ? project : script_key;
        auto& mods = _modules[project];
        auto& slot = mods[key]; // value-initializes {nullptr, {}} if new
        void* old_handle = slot.handle;
        fs::path old_path = slot.path;
        std::vector<std::string> old_factory_keys = slot.factory_keys;

#if defined(_WIN32)
        HMODULE h = LoadLibraryA(module_path.string().c_str());
        if (!h) {
            if (out_error) *out_error = "LoadLibrary failed for " + module_path.string() +
                " (error code " + std::to_string(GetLastError()) + ")";
            return false;
        }
        auto fn = reinterpret_cast<RegisterAllScriptsFn>(GetProcAddress(h, "RegisterAllScripts"));
        if (!fn) {
            if (out_error) *out_error = "RegisterAllScripts symbol not found in " + module_path.string();
            FreeLibrary(h);
            return false;
        }
#else
        void* h = dlopen(module_path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) {
            if (out_error) *out_error = std::string("dlopen failed for ") + module_path.string() +
                ": " + (dlerror() ? dlerror() : "unknown error");
            return false;
        }
        auto fn = reinterpret_cast<RegisterAllScriptsFn>(dlsym(h, "RegisterAllScripts"));
        if (!fn) {
            if (out_error) *out_error = "RegisterAllScripts symbol not found in " + module_path.string();
            dlclose(h);
            return false;
        }
#endif
        // For per-script DLLs (script_key is set), DON'T clear the entire
        // project — each script DLL registers one unique class that won't
        // conflict with other scripts in the same project. For monolithic
        // modules (script_key empty), clear the project as before.
        if (script_key.empty()) {
            ScriptRegistry::instance().clear_project(project);
        } else {
            // Only clear the specific script's registration
            std::string expected_key = project + "::";
            // Find and remove any factory starting with "<project>::" and
            // matching this script name (we don't know the exact key — the
            // REGISTER_SCRIPT macro normalizes the name). We'll just clear
            // project and re-register all other scripts from other DLLs...
            // Actually, for per-script DLLs, each DLL only registers one
            // class with a project-prefixed key. Since no two DLLs register
            // the same key, there's no conflict. Don't clear anything here.
        }

        fn(&ScriptRegistry::instance(), &scriptfields::InstanceRegistry::instance(),
           &Screen::_state(), &Input::_state(), &SceneManager::_state(), &Network::_state(), &Matchmaking::_state(),
           &EventBus::_state(), &Debug::_state(), project.c_str());

        // Entries no longer emitted by this source must not retain pointers
        // into the old DLL after it is unloaded below.
        if (!script_key.empty() && !factory_keys.empty()) {
            for (const auto& old_factory_key : old_factory_keys) {
                if (std::find(factory_keys.begin(), factory_keys.end(), old_factory_key) == factory_keys.end())
                    ScriptRegistry::instance().unreg(old_factory_key);
            }
        }

        slot.handle = h;
        slot.path = module_path;
        if (!script_key.empty() && !factory_keys.empty())
            slot.factory_keys = std::move(factory_keys);

        if (old_handle) {
#if defined(_WIN32)
            FreeLibrary(static_cast<HMODULE>(old_handle));
#else
            dlclose(old_handle);
#endif
            // Best-effort cleanup of the old module file itself.
            std::error_code ec;
            for (int attempt = 0; attempt < 20; ++attempt) {
                if (!fs::exists(old_path, ec)) break;
                if (fs::remove(old_path, ec)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
        }
        return true;
    }

    // Load all per-script DLLs for a project from a set of candidate
    // directories.  Each source stem is selected globally by newest modified
    // time before anything is loaded.  This matters because the fast watcher
    // writes just-edited DLLs into LOCALAPPDATA while the rest of the project
    // may still live in the workspace build: loading one directory at a time
    // used to make one local NewScript hide every other valid game module.
    // Each DLL is expected to be named `<ns>_<scriptname>.dll` (or .so/.dylib).
    // Returns the count successfully loaded.
    int load_all_for_project(const std::string& project, const std::vector<fs::path>& dirs,
                             const fs::path& scripts_dir = {},
                             std::string* out_error = nullptr) {
        std::string ns = _project_to_ns(project);
        std::string prefix = ns + "_";
        struct Candidate {
            fs::path path;
            fs::file_time_type modified{};
        };
        std::unordered_map<std::string, Candidate> newest_by_source;

        // AutoHotReload deliberately gives a live DLL a unique
        // `<target>_hot_<timestamp>` filename so Windows can keep the old
        // module loaded until its instances are safely destroyed. At startup
        // those artifacts must be mapped back to their source stem; treating
        // `NewScript_hot_123.cpp` as the source used to skip every hot DLL,
        // leaving a reopened editor with an empty ScriptRegistry.
        for (const auto& dir : dirs) {
            std::error_code ec;
            if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) continue;
            for (auto& entry : fs::directory_iterator(dir, ec)) {
                if (ec || !entry.is_regular_file(ec)) continue;
                auto ext = entry.path().extension().string();
#if defined(_WIN32)
                if (ext != ".dll") continue;
#else
                if (ext != ".so" && ext != ".dylib") continue;
#endif
                std::string stem = entry.path().stem().string();
#if !defined(_WIN32)
                if (stem.rfind("lib", 0) == 0) stem = stem.substr(3);
#endif
                if (stem.rfind(prefix, 0) != 0) continue;
                std::string script_name = stem.substr(prefix.size());
                const std::size_t hot_suffix = script_name.rfind("_hot_");
                if (hot_suffix != std::string::npos) script_name.resize(hot_suffix);
                if (script_name.empty()) continue;

                std::error_code time_error;
                const auto modified = fs::last_write_time(entry.path(), time_error);
                if (time_error) continue;
                auto found = newest_by_source.find(script_name);
                if (found == newest_by_source.end() || modified > found->second.modified)
                    newest_by_source[script_name] = {entry.path(), modified};
            }
        }

        int count = 0;
        for (const auto& [script_name, candidate] : newest_by_source) {

            std::vector<std::string> factory_keys;
            if (!scripts_dir.empty()) {
                // Build directories outlive source files. Source membership
                // wins, so a deleted script can never be loaded from a stale
                // DLL during a later editor launch.
                factory_keys = _factory_keys_from_source(
                    project, scripts_dir / (script_name + ".cpp"));
                if (factory_keys.empty()) continue;
            }

            std::string key = project + "::" + script_name;
            std::string err;
            if (load(project, candidate.path, &err, key, std::move(factory_keys))) {
                ++count;
            } else if (out_error) {
                *out_error += err + "\n";
            }
        }
        return count;
    }

    // One-directory convenience form used by a live hot-reload swap.
    int load_all_for_project(const std::string& project, const fs::path& dir,
                             const fs::path& scripts_dir = {},
                             std::string* out_error = nullptr) {
        return load_all_for_project(project, std::vector<fs::path>{dir}, scripts_dir, out_error);
    }

private:
    struct LoadedModule {
        void* handle = nullptr;
        fs::path path;
        std::vector<std::string> factory_keys;
    };
    // _modules[project][script_key] -> LoadedModule
    // script_key = "" for monolithic, "project::ClassName" for per-script
    std::unordered_map<std::string, std::unordered_map<std::string, LoadedModule>> _modules;

    static void _remove_module_file(const fs::path& path) {
        std::error_code ec;
        for (int attempt = 0; attempt < 20; ++attempt) {
            if (!fs::exists(path, ec)) break;
            if (fs::remove(path, ec)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    }

    static std::vector<std::string> _factory_keys_from_source(
        const std::string& project, const fs::path& source) {
        std::ifstream in(source);
        if (!in) return {};
        std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        static const std::regex class_re(
            R"(class[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]*:[ \t]*public[ \t]+(?:ScriptBase|MonoBehaviour|Behaviour2D|Behaviour))");
        std::vector<std::string> keys;
        for (std::sregex_iterator it(text.begin(), text.end(), class_re), end;
             it != end; ++it) {
            keys.push_back(project + "::" + (*it)[1].str());
        }
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        return keys;
    }

    static std::string _project_to_ns(const std::string& project) {
        std::string ns = project;
        for (auto& c : ns) if (!std::isalnum((unsigned char)c) && c != '_') c = '_';
        return ns;
    }
};
