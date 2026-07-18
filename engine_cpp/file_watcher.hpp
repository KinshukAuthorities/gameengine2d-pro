#pragma once
/*
 * file_watcher.hpp — Cross-platform directory file watcher.
 *
 * Polls a set of directories for file modifications (create/modify/delete)
 * on a background thread and invokes callbacks. Uses file timestamps.
 *
 * Unity's "Auto Refresh" is notoriously slow because it blocks the editor
 * while reimporting assets. This watcher runs on a background thread with
 * a configurable debounce interval, so the editor stays responsive.
 */
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <algorithm>

namespace fs = std::filesystem;

struct FileWatchEvent {
    enum Type { Created, Modified, Deleted };
    Type type;
    fs::path path;
};

class FileWatcher {
public:
    using Callback = std::function<void(const std::vector<FileWatchEvent>&)>;

    explicit FileWatcher(std::chrono::milliseconds poll_interval = std::chrono::milliseconds(500))
        : _poll_interval(poll_interval), _running(false) {}

    ~FileWatcher() { stop(); }

    void watch(const fs::path& directory, bool recursive = true,
               const std::string& extension_filter = "") {
        std::lock_guard<std::mutex> lock(_mutex);
        WatchTarget t;
        t.dir = directory;
        t.recursive = recursive;
        t.extension_filter = extension_filter;
        // Snapshot current state
        _snapshot(t);
        _targets.push_back(std::move(t));
    }

    void set_callback(Callback cb) {
        std::lock_guard<std::mutex> lock(_mutex);
        _callback = std::move(cb);
    }

    void start() {
        if (_running.exchange(true)) return;
        _thread = std::thread([this] { _poll_loop(); });
    }

    void stop() {
        if (!_running.exchange(false)) return;
        if (_thread.joinable()) {
            // A callback may intentionally stop its own watch (for example
            // a one-shot import/reload watch). Joining the current thread
            // throws/terminates; detach is safe in that narrow case because
            // `_running` is already false and _poll_loop returns immediately
            // after the callback completes.
            if (_thread.get_id() == std::this_thread::get_id()) _thread.detach();
            else _thread.join();
        }
    }

    bool is_running() const { return _running.load(); }

    // Force an immediate poll (useful before critical operations)
    std::vector<FileWatchEvent> poll_once() {
        std::vector<FileWatchEvent> all_events;
        std::lock_guard<std::mutex> lock(_mutex);
        for (auto& t : _targets) {
            auto events = _scan_changes(t);
            all_events.insert(all_events.end(), events.begin(), events.end());
        }
        return all_events;
    }

private:
    struct WatchTarget {
        fs::path dir;
        bool recursive = true;
        std::string extension_filter;
        std::unordered_map<std::string, fs::file_time_type> file_times;
    };

    std::vector<WatchTarget> _targets;
    Callback _callback;
    std::thread _thread;
    std::atomic<bool> _running;
    std::chrono::milliseconds _poll_interval;
    mutable std::mutex _mutex;

    void _snapshot(WatchTarget& t) {
        t.file_times.clear();
        std::error_code ec;
        if (!fs::exists(t.dir, ec)) return;
        if (t.recursive) {
            for (auto& entry : fs::recursive_directory_iterator(t.dir, fs::directory_options::skip_permission_denied)) {
                if (!entry.is_regular_file(ec)) continue;
                auto path_str = entry.path().string();
                if (!t.extension_filter.empty() && entry.path().extension() != t.extension_filter) continue;
                t.file_times[path_str] = entry.last_write_time(ec);
            }
        } else {
            for (auto& entry : fs::directory_iterator(t.dir, fs::directory_options::skip_permission_denied)) {
                if (!entry.is_regular_file(ec)) continue;
                auto path_str = entry.path().string();
                if (!t.extension_filter.empty() && entry.path().extension() != t.extension_filter) continue;
                t.file_times[path_str] = entry.last_write_time(ec);
            }
        }
    }

    std::vector<FileWatchEvent> _scan_changes(WatchTarget& t) {
        std::vector<FileWatchEvent> events;
        std::error_code ec;

        // Build current state
        std::unordered_map<std::string, fs::file_time_type> current;
        if (fs::exists(t.dir, ec)) {
            if (t.recursive) {
                for (auto& entry : fs::recursive_directory_iterator(t.dir, fs::directory_options::skip_permission_denied)) {
                    if (!entry.is_regular_file(ec)) continue;
                    auto path_str = entry.path().string();
                    if (!t.extension_filter.empty() && entry.path().extension() != t.extension_filter) continue;
                    current[path_str] = entry.last_write_time(ec);
                }
            } else {
                for (auto& entry : fs::directory_iterator(t.dir, fs::directory_options::skip_permission_denied)) {
                    if (!entry.is_regular_file(ec)) continue;
                    auto path_str = entry.path().string();
                    if (!t.extension_filter.empty() && entry.path().extension() != t.extension_filter) continue;
                    current[path_str] = entry.last_write_time(ec);
                }
            }
        }

        // Detect new + modified
        for (auto& [path, time] : current) {
            auto it = t.file_times.find(path);
            if (it == t.file_times.end()) {
                events.push_back({FileWatchEvent::Created, fs::path(path)});
            } else if (it->second != time) {
                events.push_back({FileWatchEvent::Modified, fs::path(path)});
            }
        }

        // Detect deleted
        for (auto& [path, time] : t.file_times) {
            if (!current.count(path)) {
                events.push_back({FileWatchEvent::Deleted, fs::path(path)});
            }
        }

        t.file_times = std::move(current);
        return events;
    }

    void _poll_loop() {
        while (_running.load()) {
            std::this_thread::sleep_for(_poll_interval);

            std::vector<FileWatchEvent> all_events;
            Callback callback;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                for (auto& t : _targets) {
                    auto events = _scan_changes(t);
                    all_events.insert(all_events.end(), events.begin(), events.end());
                }
                // Copy while protected, then execute after releasing the
                // watcher lock. A callback may begin a rebuild or stop the
                // watcher; invoking it under `_mutex` can deadlock that work.
                callback = _callback;
            }

            if (!all_events.empty() && callback) callback(all_events);
        }
    }
};
