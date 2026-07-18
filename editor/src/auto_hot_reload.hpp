#pragma once

#include "../../engine_cpp/file_watcher.hpp"
#include "script_module_loader.hpp"
#include "toolchain_discovery.hpp"
#include "../../engine_cpp/script_system.hpp"

#include <filesystem>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <unordered_map>
#include <cstdlib>
#include <regex>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <cctype>
#include <chrono>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace _ahr_fs = std::filesystem;

// Fast per-script hot-reload.
//
// Watches the active project's scripts/ directory via FileWatcher.
// When a .cpp file is saved, rebuilds ONLY that one script's DLL
// instead of every script in the project. This makes the iteration
// cycle ~1–2 seconds instead of 10–30+.
//
// For EXISTING scripts (target already registered in CMake): builds
// the existing target directly with no cmake reconfigure — msbuild
// compiles exactly one wrapper+physics.cpp.
//
// For NEW scripts (target not yet created): runs cmake reconfigure
// first (picks up the GLOB), then builds the single new target.
//
// The rebuild runs on a background thread via std::system(). When
// it finishes, process_pending_swaps() (called from the main loop)
// destroys old script instances and loads the newly built DLL.
class AutoHotReload {
public:
    static AutoHotReload& instance() {
        static AutoHotReload inst;
        return inst;
    }

    static bool is_active() { return ahr_active_flag(); }

    ~AutoHotReload() { shutdown(); }

    void shutdown() {
        ahr_active_flag() = false;
        _shutting_down = true;
        _watcher.stop();
        // A worker captures `this` and writes rebuild status. Detaching it
        // let it race editor/Console teardown and caused shutdown crashes.
        // The build is finite, so establish a real lifetime boundary.
        if (_build_thread.joinable()) _build_thread.join();
        _building = false;
    }

    void start_watching(const std::string& project,
                        const _ahr_fs::path& scripts_dir,
                        const _ahr_fs::path& /*build_dir_ignored*/,
                        const _ahr_fs::path& project_root) {
        _project      = project;
        _scripts_dir  = _ahr_fs::absolute(scripts_dir);
        _project_root = _ahr_fs::absolute(project_root);
        _ns           = _cmake_namespace(project);
        // Build output must belong to exactly one project and live outside a
        // synced checkout.  `script_module_build_dir` keeps MSVC's volatile
        // .obj/.pch/.pdb files in LOCALAPPDATA on Windows, avoiding OneDrive
        // file handles without changing script source or project locations.
        _build_dir    = script_module_build_dir(_project_root, _ns);
        _msbuild_path = _find_msbuild();
        _shutting_down = false;

        if (_msbuild_path.empty()) {
            Debug::log_warning("[AutoHotReload] MSBuild not found at VS install paths — "
                               "will fall back to cmake --build (slower, full cascade)");
        } else {
            Debug::log("[AutoHotReload] Using MSBuild: " + _msbuild_path);
        }

        ahr_active_flag() = true;
        _watcher.watch(_scripts_dir, true, ".cpp");
        _watcher.set_callback([this](const std::vector<FileWatchEvent>& events) {
            _on_file_changed(events);
        });
        _watcher.start();

        Debug::log("[AutoHotReload] Watching " + _scripts_dir.string() +
                   "  (build: " + _build_dir.string() + ")");
    }

    bool is_building() const { return _building.load(); }

    // Fast per-script builds use their own worker, separate from the legacy
    // project-wide rebuild state. Expose a thread-safe snapshot so the editor
    // can present the same blocking compilation modal for both paths.
    std::string build_status() const {
        std::lock_guard<std::mutex> lock(_status_mutex);
        return _status;
    }

    std::string active_project() const {
        std::lock_guard<std::mutex> lock(_status_mutex);
        return _status_project;
    }

    // The Assets panel knows exactly which source it just created. Asking the
    // watcher to rebuild that path directly avoids the legacy project-wide
    // meta target. Existing targets compile immediately; a new source pays one
    // configure to create its project file, then builds only its own DLL.
    void request_rebuild(const _ahr_fs::path& source) {
        if (_shutting_down.load() || _scripts_dir.empty()) return;
        std::error_code ec;
        const _ahr_fs::path absolute = _ahr_fs::absolute(source, ec);
        if (ec || absolute.extension() != ".cpp") return;
        const _ahr_fs::path relative = absolute.lexically_relative(_scripts_dir);
        if (relative.empty() || *relative.begin() == "..") return;

        // A creation also raises a watcher Created/Modified pair shortly
        // afterwards. Suppress that duplicate notification briefly, without
        // swallowing a real later save.
        {
            std::lock_guard<std::mutex> lk(_requested_mutex);
            _ignore_events_until[absolute.generic_string()] =
                std::chrono::steady_clock::now() + std::chrono::seconds(2);
        }
        if (_play_mode_active.load() || _building.load())
            _queue_change(absolute, FileWatchEvent::Created);
        else
            _start_build(absolute, FileWatchEvent::Created);
    }

    // Play mode must run against a stable set of DLLs. File notifications can
    // come from OneDrive as well as a deliberate source save, so defer both
    // build starts and module swaps until the editor leaves Play mode.
    void set_play_mode_active(bool active) { _play_mode_active = active; }

    // Must be called from the main thread every frame (after all
    // panels draw but before Present). Picks up completed background
    // builds and performs the safe module swap.
    //
    // `script_sys` is the active ScriptSystem (owned by ViewportPanel).
    // Its instances are destroyed before the old DLL is unloaded so no
    // vtable pointers dangle. In edit mode (no play session) the system
    // is empty and this is a safe no-op for its part.
    bool process_pending_swaps(ScriptSystem& script_sys, bool allow_reload = true) {
        if (!allow_reload) return false;
        _start_queued_change_if_ready();
        std::string script_key;
        std::vector<std::string> factory_keys;
        _ahr_fs::path dll_path;
        bool remove_script = false;
        std::string removal_key;
        {
            std::lock_guard<std::mutex> lk(_swap_mutex);
            if (!_pending_swap.load() && !_pending_removal) return false;
            script_key = _pending_script_key;
            factory_keys = _pending_factory_keys;
            dll_path   = _pending_dll_path;
            _pending_swap = false;
            _pending_script_key.clear();
            _pending_factory_keys.clear();
            _pending_dll_path.clear();
            remove_script = _pending_removal;
            removal_key = _pending_removal_key;
            _pending_removal = false;
            _pending_removal_key.clear();
        }

        if (remove_script && !removal_key.empty()) {
            script_sys.reset_all_instances();
            if (ScriptModuleLoader::instance().unload_script(_project, removal_key)) {
                Debug::log("[AutoHotReload] Removed script module: " + removal_key);
            }
        }

        if (dll_path.empty() || script_key.empty()) return remove_script;
        if (!_ahr_fs::exists(dll_path)) {
            Debug::log_error("[AutoHotReload] DLL not found: " + dll_path.string());
            return remove_script;
        }
        // Destroy all live script instances before unloading the old DLL
        // — otherwise vtable pointers dangle on FreeLibrary.
        // Safe to call even when no instances exist (edit mode).
        script_sys.reset_all_instances();

        std::string error;
        if (ScriptModuleLoader::instance().load(
                _project, dll_path, &error, script_key, std::move(factory_keys))) {
            Debug::log("[AutoHotReload] Reloaded: " + dll_path.filename().string());
            return true;
        } else {
            Debug::log_error("[AutoHotReload] Load failed for " +
                             dll_path.filename().string() + ": " + error);
            return remove_script;
        }
    }

private:
    struct QueuedChange {
        _ahr_fs::path path;
        FileWatchEvent::Type type = FileWatchEvent::Modified;
    };

    std::string                     _project;
    _ahr_fs::path                   _scripts_dir;
    _ahr_fs::path                   _project_root;
    _ahr_fs::path                   _build_dir;
    std::string                     _ns;
    std::string                     _msbuild_path;
    // 150 ms keeps saves feeling immediate without spinning on a OneDrive
    // project folder (the old 500 ms poll was a visible delay by itself).
    FileWatcher                     _watcher{std::chrono::milliseconds(150)};

    std::atomic<bool>               _building{false};
    std::atomic<bool>               _pending_swap{false};
    std::atomic<bool>               _play_mode_active{false};
    std::atomic<bool>               _shutting_down{false};
    mutable std::mutex               _status_mutex;
    std::string                      _status;
    std::string                      _status_project;
    std::mutex                      _swap_mutex;
    std::string                     _pending_script_key;
    std::vector<std::string>        _pending_factory_keys;
    _ahr_fs::path                   _pending_dll_path;
    bool                            _pending_removal = false;
    std::string                     _pending_removal_key;
    std::mutex                      _queued_change_mutex;
    std::vector<QueuedChange>       _queued_changes;
    std::mutex                      _requested_mutex;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
                                    _ignore_events_until;
    std::thread                     _build_thread;

    void _set_status(std::string message) {
        std::lock_guard<std::mutex> lock(_status_mutex);
        _status = std::move(message);
        _status_project = _project;
    }

    // Serialise configure/build work across editor.exe processes that have
    // the same project open. Windows releases an abandoned mutex when a
    // process dies, so this cannot leave behind a stale build lock.
    class BuildLease {
    public:
        explicit BuildLease(const std::string& name) {
#if defined(_WIN32)
            _handle = ::CreateMutexA(nullptr, FALSE, name.c_str());
#else
            (void)name;
#endif
        }

        ~BuildLease() {
#if defined(_WIN32)
            if (_owns && _handle) ::ReleaseMutex(_handle);
            if (_handle) ::CloseHandle(_handle);
#endif
        }

        bool acquire(unsigned timeout_ms = 60000) {
#if defined(_WIN32)
            if (!_handle) return false;
            const DWORD result = ::WaitForSingleObject(_handle, timeout_ms);
            _owns = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
            return _owns;
#else
            (void)timeout_ms;
            return true;
#endif
        }

        BuildLease(const BuildLease&) = delete;
        BuildLease& operator=(const BuildLease&) = delete;

    private:
#if defined(_WIN32)
        HANDLE _handle = nullptr;
        bool _owns = false;
#endif
    };

    // Mirror panels.hpp _cmake_namespace_for_project exactly
    static std::string _cmake_namespace(const std::string& project) {
        std::string ns;
        ns.reserve(project.size());
        for (char c : project)
            ns.push_back((std::isalnum((unsigned char)c) || c == '_') ? c : '_');
        if (!ns.empty() && std::isdigit((unsigned char)ns[0]))
            ns = "project_" + ns;
        return ns;
    }

    static std::string _quote(const _ahr_fs::path& p) {
        return std::string("\"") + p.string() + "\"";
    }

    static int _run_with_lock_retry(const std::string& command,
                                    const std::string& target,
                                    const char* phase,
                                    int attempts = 3) {
        int result = 1;
        for (int attempt = 1; attempt <= attempts; ++attempt) {
            result = std::system(command.c_str());
            if (result == 0) return 0;
            if (attempt < attempts) {
                Debug::log_warning("[AutoHotReload] " + std::string(phase) +
                                   " hit a transient compiler file lock for \"" +
                                   target + "\"; retrying (" +
                                   std::to_string(attempt + 1) + "/" +
                                   std::to_string(attempts) + ").");
                std::this_thread::sleep_for(std::chrono::milliseconds(900));
            }
        }
        return result;
    }

    // Some launchers provide a 32-bit-looking environment even when the
    // editor and MSBuild are 64-bit. Visual Studio then selects HostX86\x64
    // cl.exe; on this machine that process can stall before parsing physics.
    // Calling VsDevCmd is stronger than setting PROCESSOR_ARCHITECTURE alone:
    // it selects HostX64, supplies INCLUDE/LIB and keeps CMake/MSBuild on one
    // consistent toolchain.  Fall back to the variables for custom installs.
    static std::string _msvc_x64_environment_prefix() {
        return gameengine::toolchain::host_x64_environment_prefix();
    }

    // Finds Visual Studio or standalone Build Tools through Windows registry,
    // environment and PATH discovery. The engine does not assume an edition,
    // a version, or a particular installation drive.
    static std::string _find_msbuild() {
        return gameengine::toolchain::msbuild().string();
    }

    // Extract class names from a script source file — mirrors
    // extract_script_class_names() in panels.hpp.
    static std::vector<std::string> _extract_classes(const _ahr_fs::path& cpp) {
        std::ifstream in(cpp);
        if (!in) return {};
        std::string src((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        std::vector<std::string> names;
        static const std::regex re(
            R"(class[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]*:[ \t]*public[ \t]+(?:ScriptBase|MonoBehaviour|Behaviour2D|Behaviour))");
        for (std::sregex_iterator it(src.begin(), src.end(), re), end;
             it != end; ++it) {
            if (it->size() > 1)
                names.push_back((*it)[1].str());
        }
        return names;
    }

    // The wrapper owns REGISTER_SCRIPT(...) lines. If a class is renamed,
    // added, or removed, compiling the old wrapper would either register the
    // wrong class or fail. Compare its registrations to the saved source and
    // reconfigure only for that structural change, not for normal code edits.
    static bool _wrapper_matches_classes(const _ahr_fs::path& wrapper,
                                         std::vector<std::string> classes) {
        std::ifstream in(wrapper);
        if (!in) return false;
        std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        static const std::regex registered_re(
            R"(REGISTER_SCRIPT\(([A-Za-z_][A-Za-z0-9_]*)\);)");
        std::vector<std::string> registered;
        for (std::sregex_iterator it(text.begin(), text.end(), registered_re), end;
             it != end; ++it) {
            registered.push_back((*it)[1].str());
        }
        std::sort(classes.begin(), classes.end());
        classes.erase(std::unique(classes.begin(), classes.end()), classes.end());
        std::sort(registered.begin(), registered.end());
        registered.erase(std::unique(registered.begin(), registered.end()), registered.end());
        return registered == classes;
    }

    void _on_file_changed(const std::vector<FileWatchEvent>& events) {
        if (_shutting_down.load()) return;
        for (auto& ev : events) {
            if (ev.type == FileWatchEvent::Modified ||
                ev.type == FileWatchEvent::Created ||
                ev.type == FileWatchEvent::Deleted) {
                if (ev.path.extension() != ".cpp") continue;
                if (ev.type != FileWatchEvent::Deleted && _consume_requested_event(ev.path)) continue;
                if (_play_mode_active.load() || _building.load()) {
                    // Saves that happen while compiling used to be silently
                    // lost. Keep the latest event and build it next. During
                    // Play this intentionally makes the watcher passive so a
                    // sync-provider timestamp change cannot interrupt play.
                    _queue_change(ev.path, ev.type);
                } else {
                    _start_build(ev.path, ev.type);
                }
            }
        }
    }

    bool _consume_requested_event(const _ahr_fs::path& path) {
        const auto now = std::chrono::steady_clock::now();
        const std::string key = _ahr_fs::absolute(path).generic_string();
        std::lock_guard<std::mutex> lk(_requested_mutex);
        for (auto it = _ignore_events_until.begin(); it != _ignore_events_until.end();) {
            if (it->second <= now) it = _ignore_events_until.erase(it);
            else ++it;
        }
        const auto found = _ignore_events_until.find(key);
        if (found == _ignore_events_until.end()) return false;
        _ignore_events_until.erase(found);
        return true;
    }

    void _start_queued_change_if_ready() {
        if (_shutting_down.load() || _play_mode_active.load() || _building.load()) return;
        _ahr_fs::path path;
        FileWatchEvent::Type type = FileWatchEvent::Modified;
        {
            std::lock_guard<std::mutex> lk(_queued_change_mutex);
            if (_queued_changes.empty()) return;
            path = _queued_changes.front().path;
            type = _queued_changes.front().type;
            _queued_changes.erase(_queued_changes.begin());
        }
        _start_build(path, type);
    }

    void _queue_change(const _ahr_fs::path& path, FileWatchEvent::Type type) {
        std::lock_guard<std::mutex> lk(_queued_change_mutex);
        auto queued = std::find_if(_queued_changes.begin(), _queued_changes.end(),
            [&](const QueuedChange& change) { return change.path == path; });
        if (queued == _queued_changes.end()) _queued_changes.push_back({path, type});
        else queued->type = type; // latest state wins (notably delete after modify)
    }

    void _start_build(const _ahr_fs::path& changed_file, FileWatchEvent::Type event_type) {
        if (_shutting_down.load()) return;
        if (event_type == FileWatchEvent::Deleted) {
            _start_removal(_ns + "_" + changed_file.stem().string(),
                           _project + "::" + changed_file.stem().string());
            return;
        }
        // Both the watcher thread and the main-thread queued-change pump can
        // reach this method. A plain `if (!_building)` allowed both to start
        // and assign the same std::thread concurrently. Claim the one build
        // slot atomically; preserve the losing change for the next pass.
        bool expected = false;
        if (!_building.compare_exchange_strong(expected, true)) {
            _queue_change(changed_file, event_type);
            return;
        }
        _set_status("Preparing " + changed_file.filename().string());
        std::string script_stem = changed_file.stem().string();
        std::string target      = _ns + "_" + script_stem;
        _ahr_fs::path vcxproj   = _build_dir / (target + ".vcxproj");
        _ahr_fs::path wrapper   = _build_dir / (target + "_wrapper.cpp");
        std::string module_key  = _project + "::" + script_stem;

        // Derive script key from the .cpp's actual class name(s)
        auto classes = _extract_classes(changed_file);
        if (classes.empty()) {
            // A file save is not a delete. Editors and sync providers can
            // expose a temporarily empty/partial source while replacing it;
            // only a FileWatchEvent::Deleted may unload this module.
            Debug::log_warning("[AutoHotReload] Ignoring source without a ScriptBase class: " +
                               changed_file.filename().string());
            _set_status("No ScriptBase class found in " + changed_file.filename().string());
            _building = false;
            return;
            Debug::log_warning("[AutoHotReload] No ScriptBase class found in " +
                               changed_file.filename().string() + " — skipping");
            return;
        }
        // Use the first class name as the key. If multiple classes exist
        // in one file (unusual) only the first is hot-reloaded.
        std::vector<std::string> factory_keys;
        factory_keys.reserve(classes.size());
        for (const auto& class_name : classes)
            factory_keys.push_back(_project + "::" + class_name);
        bool needs_configure = !_ahr_fs::exists(vcxproj) ||
                               !_wrapper_matches_classes(wrapper, classes);
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        std::string output_name = target + "_hot_" + std::to_string(stamp);
        _ahr_fs::path dll_out = _build_dir / "Release" / (output_name + ".dll");

        if (_build_thread.joinable()) _build_thread.join(); // reap previous build
        _build_thread = std::thread(
            [this, target, module_key, factory_keys = std::move(factory_keys),
             vcxproj, dll_out, output_name, needs_configure]() mutable {
                _do_build(target, module_key, std::move(factory_keys), vcxproj,
                          dll_out, output_name, needs_configure);
            });
    }

    void _start_removal(const std::string& target, const std::string& module_key) {
        if (_shutting_down.load()) return;
        bool expected = false;
        if (!_building.compare_exchange_strong(expected, true)) {
            _queue_change(_scripts_dir / (target.substr(_ns.size() + 1) + ".cpp"), FileWatchEvent::Deleted);
            return;
        }
        if (_build_thread.joinable()) _build_thread.join();
        _build_thread = std::thread([this, target, module_key]() {
            _do_remove(target, module_key);
        });
    }

    void _do_remove(const std::string& target, const std::string& module_key) {
        BuildLease lease("Local\\GameEngine2D_AutoHotReload_" + _ns);
        if (!lease.acquire()) {
            Debug::log_warning("[AutoHotReload] Timed out waiting for the project build lock while removing \"" + target + "\".");
            _queue_change(_scripts_dir / (target.substr(_ns.size() + 1) + ".cpp"), FileWatchEvent::Deleted);
            _building = false;
            return;
        }
        _ahr_fs::path scripts_src = _project_root / "editor" / "scripts_module";
        std::string cmd = _msvc_x64_environment_prefix() + "cmake -S " + _quote(scripts_src)
                        + " -B " + _quote(_build_dir)
                        + " -DSCRIPTS_TARGET_PROJECT=" + _project
                        + " 2>&1";
        if (_run_with_lock_retry(cmd, target, "CMake reconfigure", 2) != 0) {
            Debug::log_warning("[AutoHotReload] cmake reconfigure failed while removing \"" +
                               target + "\"");
            _building = false;
            return;
        }
        {
            std::lock_guard<std::mutex> lk(_swap_mutex);
            _pending_removal = true;
            _pending_removal_key = module_key;
        }
        _building = false;
    }

    void _do_build(const std::string& target,
                   const std::string& module_key,
                   std::vector<std::string> factory_keys,
                   const _ahr_fs::path& vcxproj,
                   const _ahr_fs::path& dll_out,
                   const std::string& output_name,
                   bool needs_configure) {
        BuildLease lease("Local\\GameEngine2D_AutoHotReload_" + _ns);
        if (!lease.acquire()) {
            Debug::log_warning("[AutoHotReload] Timed out waiting for the project build lock for \"" + target + "\". The latest save remains queued.");
            _queue_change(_scripts_dir / (target.substr(_ns.size() + 1) + ".cpp"), FileWatchEvent::Modified);
            _building = false;
            return;
        }

        // ── Phase 1: reconfigure if this is a brand-new script ────────
        // New scripts have no .vcxproj yet — run cmake configure to
        // generate one (picks up the GLOB and creates the target).
        //
        // Even though reconfigure regenerates EVERY .vcxproj with fresh
        // timestamps, Phase 2 uses msbuild DIRECTLY (not cmake --build)
        // on the single .vcxproj, so only that one project gets built.
        if (needs_configure) {
            _set_status("Registering new script target: " + target);
            _ahr_fs::path scripts_src = _project_root / "editor" / "scripts_module";
            std::string cmd = _msvc_x64_environment_prefix() + "cmake -S " + _quote(scripts_src)
                            + " -B " + _quote(_build_dir)
                            + " -DSCRIPTS_TARGET_PROJECT=" + _project
                            + " 2>&1";
            int rc = _run_with_lock_retry(cmd, target, "CMake configure", 2);
            if (rc != 0) {
                Debug::log_warning(
                    "[AutoHotReload] cmake reconfigure failed for new script \"" +
                    target + "\" (exit " + std::to_string(rc) + ")");
                _building = false;
                return;
            }
        }

        // ── Phase 1.5: prime the shared physics lib ────────────────────
        // Every script links "<ns>_physics_shared" (see
        // scripts_module/CMakeLists.txt). Build it once if the output
        // artifact is missing — subsequent msbuild calls on a single
        // .vcxproj resolve ProjectReferences and build dependencies
        // automatically, but priming avoids the cascading rebuild from
        // the cmake --build fallback path.
        _ahr_fs::path physics_vcxproj = _build_dir / (_ns + "_physics_shared.vcxproj");
        _ahr_fs::path physics_lib     = _build_dir / "Release" / (_ns + "_physics_shared.lib");
        if (_ahr_fs::exists(physics_vcxproj) && !_ahr_fs::exists(physics_lib)) {
            _set_status("Preparing shared physics for " + target);
            std::string msbuild_exe = _msbuild_path.empty() ? "msbuild" : _msbuild_path;
            std::string cmd = _msvc_x64_environment_prefix() + " " + _quote(msbuild_exe) + " " + _quote(physics_vcxproj) +
                               " /p:Configuration=Release /p:PreferredToolArchitecture=x64 2>&1";
            if (_run_with_lock_retry(cmd, target, "shared physics build") != 0) {
                Debug::log_warning("[AutoHotReload] Failed to prime shared physics lib for \"" +
                                    target + "\"");
                _building = false;
                return;
            }
        }

        // ── Phase 2: build the single per-script DLL ──────────────────
        // Use msbuild DIRECTLY on the .vcxproj instead of cmake --build.
        // cmake --build embeds a cmake re-run check that regenerates
        // ALL project files + triggers full rebuild. msbuild on a single
        // .vcxproj skips cmake entirely and builds ONLY that target.
        //
        // NO `/m` flag — parallel compilation of a single .vcxproj causes
        // C1041 PDB lock when the compiler spawn shares vc143.pdb across
        // cl.exe instances for the same target directory. Sequential is
        // fast enough (1-2s per script).
        _set_status("Compiling " + target);
        std::string msbuild_exe = _msbuild_path.empty() ? "msbuild" : _msbuild_path;
        // NB: prefix with space to prevent cmd.exe /c from stripping outer
        // quotes — without it, a command starting with '"' triggers
        // cmd.exe's special-quoting heuristic that removes the first and
        // last '"', splitting path components with spaces ("C:\Program
        // Files\..." → 'C:\Program' is not recognized).
        std::string cmd = _msvc_x64_environment_prefix() + " " + _quote(msbuild_exe) + " " + _quote(vcxproj) +
                          " /p:Configuration=Release /p:PreferredToolArchitecture=x64 /p:TargetName=" + output_name + " 2>&1";
        int rc = _run_with_lock_retry(cmd, target, "script build");
        if (rc != 0) {
            Debug::log_warning(
                "[AutoHotReload] msbuild failed for \"" + target +
                "\" (exit " + std::to_string(rc) +
                "). Trying cmake --build as fallback...");
            // Fallback: cmake --build (triggers reconfigure cascade but
            // still produces the DLL). Serial (no `/m`) to avoid PDB lock.
            std::string fallback = _msvc_x64_environment_prefix() + "cmake --build " + _quote(_build_dir)
                                 + " --target " + target
                                 + " --config Release -- /p:PreferredToolArchitecture=x64 /p:TargetName=" + output_name + " 2>&1";
            rc = _run_with_lock_retry(fallback, target, "fallback script build", 2);
            if (rc != 0) {
                Debug::log_warning(
                    "[AutoHotReload] cmake --build also failed for \"" + target +
                    "\" (exit " + std::to_string(rc) + ")");
                _building = false;
                return;
            }
        }

        // ── Phase 3: queue the swap for the main thread ───────────────
        {
            std::lock_guard<std::mutex> lk(_swap_mutex);
            _pending_script_key = module_key;
            _pending_factory_keys = std::move(factory_keys);
            _pending_dll_path   = dll_out;
            _pending_swap       = true;
        }
        _set_status("Reloading " + target);
        _building = false;
    }
};
