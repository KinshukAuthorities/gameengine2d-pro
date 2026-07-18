#pragma once
/*
 * crash_reporter.hpp -- last-resort native crash diagnostics.
 *
 * Script callbacks are guarded individually by ScriptSystem, but a driver,
 * third-party DLL, or a fault outside a callback can still terminate a
 * release build before its console is visible.  On Windows we leave a small
 * text report and a MiniDump in crash_reports/ so the next debugging pass has
 * an actual fault address and module list instead of a silent window close.
 * The handler is deliberately best-effort: reporting must never make the
 * original fault worse, and non-Windows builds keep a no-op implementation.
 */

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <atomic>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <dbghelp.h>
#endif

namespace crashreport {

struct State {
    std::filesystem::path directory;
    std::string application;
    // Only string literals are stored here.  That keeps stage updates safe
    // even when a fatal exception interrupts a frame mid-allocation.
    std::atomic<const char*> stage{"startup"};
};

inline State& state() {
    static State s;
    return s;
}

inline void set_stage(const char* stage) noexcept {
    state().stage.store(stage ? stage : "unknown", std::memory_order_relaxed);
}

inline const char* last_stage() noexcept {
    return state().stage.load(std::memory_order_relaxed);
}

inline void write_note(const std::string& message) noexcept {
    try {
        State& s = state();
        if (s.directory.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(s.directory, ec);
        std::ofstream out(s.directory / (s.application.empty() ? "crash.txt" : s.application + "_last_error.txt"),
                          std::ios::out | std::ios::trunc);
        if (out) {
            out << "Last engine stage: " << last_stage() << "\n";
            out << message << "\n";
        }
    } catch (...) {
        // A corrupted process may not be able to allocate or write.  The
        // original exception/fault must still take its normal path.
    }
}

#if defined(_WIN32)
inline std::wstring timestamp_suffix() noexcept {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t suffix[48] = {};
    swprintf_s(suffix, L"%04u%02u%02u_%02u%02u%02u_%03u",
               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
               now.wSecond, now.wMilliseconds);
    return suffix;
}

inline LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* exception_info) {
    // This is a last-ditch handler after the process has faulted. Keep it
    // small, avoid UI dialogs, and do not throw from it.
    State& s = state();
    try {
        if (!s.directory.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(s.directory, ec);
            const std::wstring stamp = timestamp_suffix();
            const std::wstring base = std::wstring(s.application.begin(), s.application.end()) + L"_" + stamp;
            const std::filesystem::path dump_path = s.directory / (base + L".dmp");
            const std::filesystem::path text_path = s.directory / (base + L".txt");

            HANDLE file = CreateFileW(dump_path.c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION info{};
                info.ThreadId = GetCurrentThreadId();
                info.ExceptionPointers = exception_info;
                info.ClientPointers = FALSE;
                MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                  MiniDumpNormal, exception_info ? &info : nullptr,
                                  nullptr, nullptr);
                CloseHandle(file);
            }

            std::ofstream out(text_path, std::ios::out | std::ios::trunc);
            if (out) {
                out << "Application: " << s.application << "\n";
                out << "Process id: " << GetCurrentProcessId() << "\n";
                out << "Last engine stage: " << last_stage() << "\n";
                if (exception_info && exception_info->ExceptionRecord) {
                    out << "Exception code: 0x" << std::hex
                        << exception_info->ExceptionRecord->ExceptionCode << "\n";
                    out << "Fault address: " << exception_info->ExceptionRecord->ExceptionAddress << "\n";
                }
                out << "A minidump with the same timestamp was requested next to this file.\n";
            }
        }
    } catch (...) {
        // Best effort only -- continue Windows' normal fatal-fault path.
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

inline void install(const std::string& application, const std::filesystem::path& root) noexcept {
    try {
        state().application = application.empty() ? "gameengine" : application;
        state().directory = root / "crash_reports";
        set_stage("startup");
        std::error_code ec;
        std::filesystem::create_directories(state().directory, ec);
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        SetUnhandledExceptionFilter(unhandled_exception_filter);
    } catch (...) {}
}
#else
inline void install(const std::string&, const std::filesystem::path&) noexcept {}
#endif

} // namespace crashreport
