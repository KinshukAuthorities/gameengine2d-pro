#pragma once

// Windows developer tools are user-selectable software.  Do not assume a
// particular Visual Studio edition, drive, or installation directory: both
// the regular IDE and Build Tools register their install roots with Windows.
// These helpers deliberately prefer the registry and environment, then use
// PATH.  The small conventional-location fallback is only for broken legacy
// installers that failed to register themselves.

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace gameengine::toolchain {
namespace fs = std::filesystem;

inline void append_unique(std::vector<fs::path>& paths, const fs::path& path) {
    if (path.empty()) return;
    const auto it = std::find_if(paths.begin(), paths.end(), [&](const fs::path& other) {
#if defined(_WIN32)
        return _wcsicmp(other.wstring().c_str(), path.wstring().c_str()) == 0;
#else
        return other == path;
#endif
    });
    if (it == paths.end()) paths.push_back(path);
}

inline fs::path environment_path(const char* variable) {
    if (const char* value = std::getenv(variable); value && *value) return fs::path(value);
    return {};
}

#if defined(_WIN32)
inline std::wstring registry_string(HKEY hive, const wchar_t* sub_key, const wchar_t* value_name,
                                    REGSAM view = KEY_WOW64_64KEY) {
    const DWORD view_flags = (view == KEY_WOW64_32KEY) ? RRF_SUBKEY_WOW6432KEY : RRF_SUBKEY_WOW6464KEY;
    DWORD type = 0;
    DWORD byte_count = 0;
    const LSTATUS size_status = RegGetValueW(hive, sub_key, value_name,
        RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | view_flags, &type, nullptr, &byte_count);
    if (size_status != ERROR_SUCCESS || byte_count < sizeof(wchar_t)) return {};

    std::wstring value(byte_count / sizeof(wchar_t), L'\0');
    if (RegGetValueW(hive, sub_key, value_name,
        RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | view_flags, &type, value.data(), &byte_count) != ERROR_SUCCESS) {
        return {};
    }
    value.resize(wcsnlen(value.c_str(), value.size()));
    if (type == REG_EXPAND_SZ) {
        const DWORD expanded_size = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
        if (expanded_size > 1) {
            std::wstring expanded(expanded_size - 1, L'\0');
            ExpandEnvironmentStringsW(value.c_str(), expanded.data(), expanded_size);
            return expanded;
        }
    }
    return value;
}

inline void append_visual_studio_registry_roots(std::vector<fs::path>& roots,
                                                HKEY hive, REGSAM view) {
    // Setup\\VS\\Products entries are present for both full Visual Studio and
    // the standalone Build Tools installer.  Values are paths, not a version
    // assumption, so future VS releases continue to work.
    constexpr wchar_t products_key[] = L"SOFTWARE\\Microsoft\\VisualStudio\\Setup\\VS\\Products";
    HKEY products = nullptr;
    if (RegOpenKeyExW(hive, products_key, 0, KEY_READ | view, &products) != ERROR_SUCCESS) return;
    for (DWORD index = 0;; ++index) {
        wchar_t name[512]{};
        DWORD name_size = static_cast<DWORD>(std::size(name));
        if (RegEnumKeyExW(products, index, name, &name_size, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        const std::wstring sub_key = std::wstring(products_key) + L"\\" + name;
        const std::wstring install_path = registry_string(hive, sub_key.c_str(), L"InstallationPath", view);
        if (!install_path.empty()) append_unique(roots, fs::path(install_path));
    }
    RegCloseKey(products);
}

inline fs::path app_path(const wchar_t* executable) {
    const std::wstring key = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\") + executable;
    for (HKEY hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
            const std::wstring registered = registry_string(hive, key.c_str(), nullptr, view);
            if (!registered.empty() && fs::exists(registered)) return fs::path(registered);
        }
    }
    return {};
}

inline fs::path path_executable(const wchar_t* executable) {
    DWORD length = SearchPathW(nullptr, executable, nullptr, 0, nullptr, nullptr);
    if (length == 0) return {};
    std::wstring value(length, L'\0');
    if (SearchPathW(nullptr, executable, nullptr, length, value.data(), nullptr) == 0) return {};
    value.resize(wcsnlen(value.c_str(), value.size()));
    return fs::path(value);
}
#endif

inline std::vector<fs::path> visual_studio_roots() {
    std::vector<fs::path> roots;
    append_unique(roots, environment_path("VSINSTALLDIR"));
    append_unique(roots, environment_path("VisualStudioInstallDir"));
#if defined(_WIN32)
    for (HKEY hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        append_visual_studio_registry_roots(roots, hive, KEY_WOW64_64KEY);
        append_visual_studio_registry_roots(roots, hive, KEY_WOW64_32KEY);
    }
#endif

    // Compatibility fallback for old local installations that did not write
    // the VS Setup registry keys.  The base directory comes from Windows, not
    // a hard-coded user path, and it is queried only after registry discovery.
    const fs::path program_files = environment_path("ProgramFiles");
    const fs::path program_files_x86 = environment_path("ProgramFiles(x86)");
    for (const auto& base : {program_files, program_files_x86}) {
        if (base.empty()) continue;
        for (const wchar_t* year : {L"2022", L"2019", L"2017"}) {
            for (const wchar_t* edition : {L"BuildTools", L"Community", L"Professional", L"Enterprise"}) {
                const fs::path candidate = base / L"Microsoft Visual Studio" / year / edition;
                if (fs::exists(candidate)) append_unique(roots, candidate);
            }
        }
    }
    return roots;
}

inline fs::path vsdevcmd() {
    for (const auto& root : visual_studio_roots()) {
        const fs::path candidate = root / "Common7" / "Tools" / "VsDevCmd.bat";
        if (fs::exists(candidate)) return candidate;
    }
    return {};
}

inline fs::path msbuild() {
#if defined(_WIN32)
    if (const fs::path from_path = path_executable(L"MSBuild.exe"); !from_path.empty()) return from_path;
#endif
    for (const auto& root : visual_studio_roots()) {
        for (const auto& relative : {fs::path("MSBuild/Current/Bin/amd64/MSBuild.exe"),
                                     fs::path("MSBuild/Current/Bin/MSBuild.exe")}) {
            const fs::path candidate = root / relative;
            if (fs::exists(candidate)) return candidate;
        }
    }
    return {};
}

inline fs::path sdl2_root_dir(const fs::path& engine_root = {}) {
    for (const char* var : {"SDL2_ROOT", "SDL2_DIR"}) {
        if (const fs::path from_env = environment_path(var);
            !from_env.empty() && fs::exists(from_env / "include" / "SDL2" / "SDL.h"))
            return from_env;
    }
    if (!engine_root.empty()) {
        const fs::path local = engine_root / "third_party" / "sdl2";
        if (fs::exists(local / "include" / "SDL2" / "SDL.h"))
            return local;
    }
    return {};
}

inline std::string sdl2_cmake_arg(const fs::path& engine_root = {}) {
    const fs::path sdl2 = sdl2_root_dir(engine_root);
    if (!sdl2.empty())
        return " -DSDL2_ROOT_DIR=\"" + sdl2.string() + "\"";
    return {};
}

inline std::string host_x64_environment_prefix() {
#if defined(_WIN32)
    if (const fs::path command = vsdevcmd(); !command.empty()) {
        return "call \"" + command.string() + "\" -arch=x64 -host_arch=x64 >nul && ";
    }
    return "set \"PROCESSOR_ARCHITECTURE=AMD64\" && set \"PROCESSOR_ARCHITEW6432=AMD64\" && ";
#else
    return {};
#endif
}

inline fs::path vscode() {
#if defined(_WIN32)
    if (const fs::path from_path = path_executable(L"code.exe"); !from_path.empty()) return from_path;
    if (const fs::path from_app_path = app_path(L"Code.exe"); !from_app_path.empty()) return from_app_path;
#endif
    std::vector<fs::path> candidates;
    const fs::path local_app_data = environment_path("LOCALAPPDATA");
    const fs::path program_files = environment_path("ProgramFiles");
    const fs::path program_files_x86 = environment_path("ProgramFiles(x86)");
    if (!local_app_data.empty()) append_unique(candidates, local_app_data / "Programs" / "Microsoft VS Code" / "Code.exe");
    if (!program_files.empty()) append_unique(candidates, program_files / "Microsoft VS Code" / "Code.exe");
    if (!program_files_x86.empty()) append_unique(candidates, program_files_x86 / "Microsoft VS Code" / "Code.exe");
    for (const auto& candidate : candidates) if (fs::exists(candidate)) return candidate;
    return {};
}

} // namespace gameengine::toolchain
