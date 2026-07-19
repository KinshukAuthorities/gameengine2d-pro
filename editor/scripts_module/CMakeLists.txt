cmake_minimum_required(VERSION 3.16)
project(GameEngine2DPro_ScriptsModule CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W3 /wd4244 /wd4267 /wd4305 /wd4996)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  # MSVC otherwise writes per-target PDBs under the sync-provider workspace.
  # OneDrive can hold those files during a normal script save, producing a
  # C1083 failure or a compiler that waits indefinitely.  Put debug records
  # in a local profile cache while retaining the normal DLL output paths.
  set(_scripts_local_debug_root "$ENV{LOCALAPPDATA}/GameEngine2DPro/script_debug")
  if("${_scripts_local_debug_root}" STREQUAL "/GameEngine2DPro/script_debug")
    set(_scripts_local_debug_root "${CMAKE_BINARY_DIR}/script_debug")
  endif()
  file(TO_CMAKE_PATH "${_scripts_local_debug_root}" _scripts_local_debug_root)
else()
  if(NOT ENGINE_DETERMINISTIC)
    add_compile_options(-O2 -Wall -Wextra -Wno-unused-parameter -ffast-math)
  else()
    add_compile_options(-O2 -Wall -Wextra -Wno-unused-parameter)
  endif()
endif()
add_compile_definitions(STANDALONE_CPP NO_SDL_MIXER)
if(ENGINE_DETERMINISTIC)
  add_compile_definitions(ENGINE_DETERMINISTIC)
endif()

# ── SDL2 (script headers transitively need SDL_Scancode etc.) ────────────────
find_package(SDL2 CONFIG QUIET)
if(NOT SDL2_FOUND)
  if(NOT SDL2_ROOT_DIR)
    set(SDL2_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/sdl2" CACHE PATH "SDL2 root (engine-local or environment supplied)")
    if(DEFINED ENV{SDL2_ROOT} AND EXISTS "$ENV{SDL2_ROOT}/include/SDL2/SDL.h")
      set(SDL2_ROOT_DIR "$ENV{SDL2_ROOT}" CACHE PATH "SDL2 root (engine-local or environment supplied)" FORCE)
    elseif(DEFINED ENV{SDL2_DIR} AND EXISTS "$ENV{SDL2_DIR}/include/SDL2/SDL.h")
      set(SDL2_ROOT_DIR "$ENV{SDL2_DIR}" CACHE PATH "SDL2 root (engine-local or environment supplied)" FORCE)
    endif()
  endif()
  find_path(SDL2_INCLUDE SDL2/SDL.h HINTS "${SDL2_ROOT_DIR}/include" REQUIRED)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    find_library(SDL2_LIB NAMES SDL2 HINTS "${SDL2_ROOT_DIR}/lib/x64" "${SDL2_ROOT_DIR}/lib" REQUIRED)
  else()
    find_library(SDL2_LIB NAMES SDL2 HINTS "${SDL2_ROOT_DIR}/lib/x86" "${SDL2_ROOT_DIR}/lib" REQUIRED)
  endif()
  message(STATUS "  scripts_module: using manual SDL2 at ${SDL2_ROOT_DIR} (SDL2_INCLUDE=${SDL2_INCLUDE})")
endif()

# ── nlohmann/json ─────────────────────────────────────────────────────────────
find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
  if(NOT NLOHMANN_DIR)
    set(NLOHMANN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "nlohmann root (engine-local)")
  endif()
  find_path(NLOHMANN_INCLUDE nlohmann/json.hpp
    HINTS "${NLOHMANN_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party" "${CMAKE_CURRENT_SOURCE_DIR}/../.." REQUIRED)
endif()

set(ACTIVE_GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../games" CACHE PATH
    "Root folder containing active game project(s)")

# Keep the generated Visual Studio graph in sync when projects are added or
# removed. The fast save path calls MSBuild on one project file directly, so
# this dependency check is not paid for on an ordinary script edit.
file(GLOB _module_game_projects CONFIGURE_DEPENDS
    RELATIVE "${ACTIVE_GAME_DIR}" "${ACTIVE_GAME_DIR}/*")
list(SORT _module_game_projects)

# Shared link-time setup
function(_scripts_module_link_common _target)
    target_include_directories(${_target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../engine_cpp
    )
    if(TARGET SDL2::SDL2)
        target_link_libraries(${_target} PRIVATE SDL2::SDL2)
    else()
        target_include_directories(${_target} PRIVATE ${SDL2_INCLUDE} ${SDL2_INCLUDE}/SDL2)
        target_link_libraries(${_target} PRIVATE ${SDL2_LIB})
    endif()
    if(nlohmann_json_FOUND)
        target_link_libraries(${_target} PRIVATE nlohmann_json::nlohmann_json)
    else()
        target_include_directories(${_target} PRIVATE ${NLOHMANN_INCLUDE})
    endif()
    if(UNIX AND NOT APPLE)
        target_link_libraries(${_target} PRIVATE m pthread stdc++fs)
    endif()
    if(WIN32)
        target_link_libraries(${_target} PRIVATE Ws2_32)
    endif()
endfunction()

set(SCRIPTS_TARGET_PROJECT "" CACHE STRING
    "If set, only define scripts module targets for this one project. Empty = all projects.")

# MSVC owns the generated cmake_pch.pch file while checking dependencies.
# Sync providers such as OneDrive can briefly hold that intermediate and turn
# an ordinary script save into C1083 "Permission denied". Hot reload builds
# one wrapper at a time, so use the reliable no-PCH path by default. The
# option remains available for a local, non-synchronised checkout.
option(SCRIPTS_MODULE_USE_PCH
    "Use MSVC precompiled headers for hot-reload script modules (unsafe on sync folders)" OFF)

# Export block for per-script DLLs
set(_export_block
    "#undef SCRIPT_PROJECT_PREFIX\n\n"
    "extern \"C\" {\n"
    "#if defined(_WIN32)\n"
    "__declspec(dllexport)\n"
    "#endif\n"
    "void RegisterAllScripts(ScriptRegistry* host_registry, scriptfields::InstanceRegistry* host_field_registry, Screen::State* host_screen_state, Input::State* host_input_state, SceneManager::State* host_scene_manager_state, Network::State* host_network_state, Matchmaking::State* host_matchmaking_state, EventBus::State* host_event_bus_state, Debug::State* host_debug_state, const char* registration_project) {\n"
    "    if (host_field_registry) scriptfields::InstanceRegistry::set_host_instance(host_field_registry)\;\n"
    "    if (host_screen_state) Screen::bind_state(host_screen_state)\;\n"
    "    if (host_input_state) Input::bind_state(host_input_state)\;\n"
    "    if (host_scene_manager_state) SceneManager::bind_state(host_scene_manager_state)\;\n"
    "    if (host_network_state) Network::bind_state(host_network_state)\;\n"
    "    if (host_matchmaking_state) Matchmaking::bind_state(host_matchmaking_state)\;\n"
    "    if (host_event_bus_state) EventBus::bind_state(host_event_bus_state)\;\n"
    "    if (host_debug_state) Debug::bind_state(host_debug_state)\;\n"
    "    if (host_registry) ::scriptregistry_detail::drain_into(*host_registry, registration_project)\;\n"
    "}\n"
    "}\n"
)
set(_export_block_str "")
foreach(_elem IN LISTS _export_block)
    set(_export_block_str "${_export_block_str}${_elem}")
endforeach()

foreach(_project ${_module_game_projects})
    if(NOT IS_DIRECTORY "${ACTIVE_GAME_DIR}/${_project}/scripts")
        continue()
    endif()
    if(NOT SCRIPTS_TARGET_PROJECT STREQUAL "" AND NOT _project STREQUAL SCRIPTS_TARGET_PROJECT)
        continue()
    endif()

    string(REGEX REPLACE "[^A-Za-z0-9_]" "_" _ns "${_project}")
    if(_ns MATCHES "^[0-9]")
        set(_ns "project_${_ns}")
    endif()

    # ── Scan at configure time only (NO CONFIGURE_DEPENDS) ─────────────────
    # Script-set changes are reconciled during CMake configuration.
    # Direct hot reload invokes MSBuild on one project file, so a normal
    # source save never needs to regenerate or build every script target.
    #
    # The editor reconfigures explicitly for a create, delete, or class-list
    # change. An explicit full Scripts rebuild also always reconciles the graph.
    # but the per-script wrappers use content-aware writing — unchanged
    # wrappers do not get rewritten, so their timestamps stay the same.
    #
    # physics.cpp and the script_system.hpp/unity2d_script_api.hpp/
    # feature_systems.hpp include chain are now each built ONCE per project
    # (see the shared physics static-lib and shared PCH targets below)
    # instead of being recompiled into every single per-script DLL. Both are
    # EXCLUDE_FROM_ALL and keyed by a name that doesn't change when scripts
    # are added/removed, so a reconfigure never forces either to rebuild —
    # only actual changes to physics.cpp or the shared headers do. This
    # keeps the original cascading-relink problem (see the shared-physics
    # comment below) from coming back while cutting a single-script rebuild
    # from ~10-12s down to ~1-2s.
    file(GLOB _project_script_sources CONFIGURE_DEPENDS
        "${ACTIVE_GAME_DIR}/${_project}/scripts/*.cpp")
    list(SORT _project_script_sources)

    # Shared headers
    set(_project_shared_headers)
    file(GLOB _shared_hpp CONFIGURE_DEPENDS
        "${ACTIVE_GAME_DIR}/${_project}/scripts/*_shared.hpp")
    foreach(_hpp ${_shared_hpp})
        file(RELATIVE_PATH _hpp_rel "${CMAKE_CURRENT_BINARY_DIR}" "${_hpp}")
        list(APPEND _project_shared_headers "${_hpp_rel}")
    endforeach()

    set(_physics_source "${CMAKE_CURRENT_SOURCE_DIR}/../../engine_cpp/engine/physics.cpp")

    # ── Shared per-project physics static lib ──────────────────────────────
    # physics.cpp is ~4500 lines. Previously it was compiled DIRECTLY into
    # EVERY per-script DLL (see git history / old comment below) specifically
    # to dodge a *different* problem: linking a shared engine_physics.lib
    # meant that whenever cmake reconfigured (e.g. because a brand-new script
    # was added), the .lib got a fresh timestamp, and MSBuild would then
    # relink every already-built script DLL against it even though nothing
    # about physics itself had changed.
    #
    # That fix came at a much bigger cost: it made physics.cpp recompile
    # FROM SCRATCH on every single script save, which is most of why a
    # single-script rebuild took 10-12 seconds instead of the target 1-2.
    #
    # Fix: still build physics as its own target, but as EXCLUDE_FROM_ALL
    # (so a plain reconfigure never forces it to rebuild — only `cmake
    # --build` explicitly requesting it, or a per-script target that
    # depends on it, does) and give it a name that does NOT change when
    # scripts are added/removed, so its own .vcxproj timestamp is stable
    # across reconfigures. MSBuild only recompiles it when physics.cpp's
    # OWN content changes — not when an unrelated script is added — so the
    # original cascading-relink problem does not come back.
    set(_physics_lib_target "${_ns}_physics_shared")
    if(NOT TARGET ${_physics_lib_target})
        add_library(${_physics_lib_target} STATIC EXCLUDE_FROM_ALL "${_physics_source}")
        if(MSVC)
            set_property(TARGET ${_physics_lib_target} PROPERTY VS_USER_PROPS
                "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/msbuild_host_x64.props")
        endif()
        _scripts_module_link_common(${_physics_lib_target})
        if(MSVC)
            set_target_properties(${_physics_lib_target} PROPERTIES
                COMPILE_PDB_OUTPUT_DIRECTORY "${_scripts_local_debug_root}/${_project}/physics"
                PDB_OUTPUT_DIRECTORY "${_scripts_local_debug_root}/${_project}/physics"
                VS_GLOBAL_IntDir "${_scripts_local_debug_root}/${_project}/physics/$(Configuration)/")
            # Match the per-script DLLs' codegen settings exactly (/Od debug,
            # no LTCG) so the .obj this produces is ABI/ODR-compatible with
            # what each DLL expects when it links this .lib in.
            #
            # /FS: force synchronous PDB writes. Without this, when MSBuild
            # schedules this target's cl.exe alongside the PCH target's (or
            # alongside another script target already using /Zi), multiple
            # compiler processes can race to open the same .pdb/.idb and one
            # loses with "Cannot open precompiled header file" or a PDB lock
            # error (C1041/C1083) — this is what caused the mixed
            # success/failure pattern on the very first multi-script build.
            # /Z7 embeds symbols in the object file instead of sharing a
            # compiler PDB, eliminating PDB contention in rapid rebuilds.
            target_compile_options(${_physics_lib_target} PRIVATE /Od /Ob0 /Oy- /GL- /Z7 /FS)
        endif()
        message(STATUS "  Script Module: ${_project} shared physics -> ${_physics_lib_target} (built once, linked by every script)")
    endif()

    # ── Per-script precompiled header ──────────────────────────────────────
    # Every script wrapper starts with the same heavy include chain:
    #   script_system.hpp (~2200 lines) -> unity2d_script_api.hpp (~2250
    #   lines) -> feature_systems.hpp (~1900 lines) -> physics.hpp (~500
    #   lines) — roughly 6800 lines of headers. Compiling these from
    #   scratch on every single script save is most of why a single-script
    #   rebuild used to take 10-12s instead of ~1-2s.
    #
    # Each script target builds its OWN .pch from the shared headers.
    # Per-script PCH avoids MSBuild's REUSE_FROM file-cleanup race where
    # ComputeOutOfDateSources deletes the shared .pch during evaluation
    # of consumer targets. No file sharing means no racing.
    set(_pch_headers
        "${CMAKE_CURRENT_SOURCE_DIR}/../../engine_cpp/script_system.hpp"
    )

    # ── Per-script DLLs (one per .cpp file) ───────────────────────────────
    # Each script gets its own DLL so a compile error in ONE script never
    # blocks others from loading. Physics is now linked from the shared
    # ${_physics_lib_target} static lib above instead of being recompiled
    # into every DLL — see the comment there for why that's safe against
    # the cascading-relink problem it used to cause.
    set(_all_script_targets)
    foreach(_script ${_project_script_sources})
        file(RELATIVE_PATH _script_rel "${CMAKE_CURRENT_BINARY_DIR}" "${_script}")
        file(READ "${_script}" _script_src)
        string(REGEX MATCHALL
            "class[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]*:[ \t]*public[ \t]+(ScriptBase|MonoBehaviour|Behaviour2D|Behaviour)"
            _class_matches "${_script_src}")
        if(NOT _class_matches)
            continue()
        endif()

        get_filename_component(_script_base "${_script}" NAME_WE)
        set(_script_target "${_ns}_${_script_base}")
        list(APPEND _all_script_targets ${_script_target})

        set(_wrapper "${CMAKE_CURRENT_BINARY_DIR}/${_script_target}_wrapper.cpp")
        # Build new wrapper content.
        #
        # SCRIPT_PROJECT_PREFIX used to be a source-level `#define` written
        # ABOVE the `#include "script_system.hpp"` line. That's incompatible
        # with the shared PCH below: MSVC's /Yu requires every consumer of a
        # precompiled header to have an IDENTICAL token stream up to (and
        # including) the precompiled #include, so a project-specific #define
        # sitting before it would make each project's wrapper disagree with
        # the PCH's own frozen prefix. It's now passed as a compiler command
        # -line definition instead (see target_compile_definitions below,
        # applied to both the PCH target and every script target for this
        # project) — same effective value, but it no longer has to appear in
        # the wrapper's own source text, so the include line the PCH covers
        # is byte-identical for every script in the project.
        set(_new_content "#include \"${CMAKE_CURRENT_SOURCE_DIR}/../../engine_cpp/script_system.hpp\"\n")
        set(_new_content "${_new_content}#include \"${CMAKE_CURRENT_SOURCE_DIR}/../../engine_cpp/net/matchmaking.hpp\"\n")
        foreach(_sh ${_project_shared_headers})
            set(_new_content "${_new_content}#include \"${_sh}\"\n")
        endforeach()
        set(_new_content "${_new_content}#include \"${_script_rel}\"\n")
        foreach(_match ${_class_matches})
            string(REGEX REPLACE
                "class[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]*:[ \t]*public[ \t]+(ScriptBase|MonoBehaviour|Behaviour2D|Behaviour)"
                "\\1" _class_name "${_match}")
            set(_new_content "${_new_content}REGISTER_SCRIPT(${_class_name});\n")
        endforeach()
        set(_new_content "${_new_content}${_export_block_str}")

        # Content-aware writing: only write if content changed
        set(_needs_update TRUE)
        if(EXISTS "${_wrapper}")
            file(READ "${_wrapper}" _old_content)
            if(_old_content STREQUAL _new_content)
                set(_needs_update FALSE)
            endif()
        endif()
        if(_needs_update)
            file(WRITE "${_wrapper}" "${_new_content}")
        endif()

        add_library(${_script_target} SHARED EXCLUDE_FROM_ALL
            "${_wrapper}"
        )
        if(MSVC)
            set_property(TARGET ${_script_target} PROPERTY VS_USER_PROPS
                "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/msbuild_host_x64.props")
        endif()
        target_link_libraries(${_script_target} PRIVATE ${_physics_lib_target})
        target_compile_definitions(${_script_target} PRIVATE SCRIPT_PROJECT_PREFIX="${_project}")

        set_target_properties(${_script_target} PROPERTIES
            OUTPUT_NAME "${_script_target}"
            PREFIX ""
        )
        if(MSVC)
            set_target_properties(${_script_target} PROPERTIES
                COMPILE_PDB_OUTPUT_DIRECTORY "${_scripts_local_debug_root}/${_project}/${_script_base}"
                PDB_OUTPUT_DIRECTORY "${_scripts_local_debug_root}/${_project}/${_script_base}"
                VS_GLOBAL_IntDir "${_scripts_local_debug_root}/${_project}/${_script_base}/$(Configuration)/")
        endif()
        _scripts_module_link_common(${_script_target})

        if(MSVC)
            # Avoid shared /Zi PDB writes: they are the main source of
            # intermittent long rebuilds in sync-provider project folders.
            target_compile_options(${_script_target} PRIVATE /Od /Ob0 /Oy- /GL- /Z7 /FS)
            # The workspace is OneDrive-backed.  MSVC's manifest embedding
            # helper can hang after it truncates the destination DLL, leaving
            # a 2 MB zero-byte-looking file that the hot reloader cannot
            # LoadLibrary.  Script DLLs do not require an embedded manifest,
            # so disable that post-link tool exactly as the editor target
            # already does.  This is essential: an unfinished manifest step
            # made a source change look successfully rebuilt while the running
            # game still executed the old controller.
            target_link_options(${_script_target} PRIVATE /LTCG:OFF /MANIFEST:NO)
            if(SCRIPTS_MODULE_USE_PCH)
                target_precompile_headers(${_script_target} PRIVATE ${_pch_headers})
            endif()
        endif()

        message(STATUS "  Script Module: ${_project}/${_script_base} -> ${_script_target}")
    endforeach()

    # Meta-target: build all per-script DLLs for this project at once
    if(_all_script_targets)
        add_custom_target(scripts_project_${_ns})
        add_dependencies(scripts_project_${_ns} ${_all_script_targets})
    endif()

    # A removed C++ source can leave a generated wrapper/project file behind
    # in an existing Visual Studio build tree. Remove those generated files so
    # an old target cannot later include a source that no longer exists.
    file(GLOB _existing_wrappers "${CMAKE_CURRENT_BINARY_DIR}/${_ns}_*_wrapper.cpp")
    foreach(_existing_wrapper IN LISTS _existing_wrappers)
        get_filename_component(_existing_name "${_existing_wrapper}" NAME)
        string(REGEX REPLACE "_wrapper\\.cpp$" "" _existing_target "${_existing_name}")
        if(NOT _existing_target IN_LIST _all_script_targets)
            message(STATUS "  Script Module: removing stale generated target ${_existing_target}")
            file(REMOVE
                "${_existing_wrapper}"
                "${CMAKE_CURRENT_BINARY_DIR}/${_existing_target}.vcxproj"
                "${CMAKE_CURRENT_BINARY_DIR}/${_existing_target}.vcxproj.filters")
            # Visual Studio stores object/PCH intermediates below CMakeFiles/.
            # Removing only the historical top-level path left a deleted
            # script's old .dir tree behind; it normally could not build, but
            # it was misleading in diagnostics and could be picked up by
            # manual project-file invocations. Clear both layouts.
            file(REMOVE_RECURSE
                "${CMAKE_CURRENT_BINARY_DIR}/${_existing_target}.dir"
                "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${_existing_target}.dir")
            foreach(_config Debug Release RelWithDebInfo MinSizeRel)
                file(REMOVE
                    "${CMAKE_CURRENT_BINARY_DIR}/${_config}/${_existing_target}.dll"
                    "${CMAKE_CURRENT_BINARY_DIR}/${_config}/${_existing_target}.lib"
                    "${CMAKE_CURRENT_BINARY_DIR}/${_config}/${_existing_target}.exp"
                    "${CMAKE_CURRENT_BINARY_DIR}/${_config}/${_existing_target}.pdb")
            endforeach()
        endif()
    endforeach()

    # Once a prior configure has already removed the wrapper, the wrapper scan
    # above cannot discover that target on a later run. Sweep CMake's target
    # intermediate directories as well, retaining only the current scripts
    # and this project's shared physics library. This is what removes the
    # final `CMakeFiles/<project>_DeletedScript.dir` remnant after a delete.
    file(GLOB _existing_target_dirs RELATIVE "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles"
        "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${_ns}_*.dir")
    foreach(_existing_target_dir IN LISTS _existing_target_dirs)
        get_filename_component(_existing_target "${_existing_target_dir}" NAME)
        string(REGEX REPLACE "\\.dir$" "" _existing_target "${_existing_target}")
        if(NOT _existing_target IN_LIST _all_script_targets AND
           NOT _existing_target STREQUAL _physics_lib_target)
            message(STATUS "  Script Module: removing stale generated intermediate ${_existing_target}")
            file(REMOVE
                "${CMAKE_CURRENT_BINARY_DIR}/${_existing_target}.vcxproj"
                "${CMAKE_CURRENT_BINARY_DIR}/${_existing_target}.vcxproj.filters")
            file(REMOVE_RECURSE "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${_existing_target_dir}")
            foreach(_config Debug Release RelWithDebInfo MinSizeRel)
                file(REMOVE
                    "${CMAKE_CURRENT_BINARY_DIR}/${_config}/${_existing_target}.dll"
                    "${CMAKE_CURRENT_BINARY_DIR}/${_config}/${_existing_target}.lib"
                    "${CMAKE_CURRENT_BINARY_DIR}/${_config}/${_existing_target}.exp"
                    "${CMAKE_CURRENT_BINARY_DIR}/${_config}/${_existing_target}.pdb")
            endforeach()
        endif()
    endforeach()

endforeach()

message(STATUS "")
message(STATUS "  GameEngine2D Pro — Scripts Modules (hot-reloadable, per-script DLLs)")
if(SCRIPTS_MODULE_USE_PCH)
    message(STATUS "  Per-script PCH enabled (local non-synchronised checkout)")
else()
    message(STATUS "  Per-script PCH disabled (reliable hot reload in sync folders)")
endif()
message(STATUS "  Shared physics static lib (EXCLUDE_FROM_ALL, built once, linked by every script)")
message(STATUS "  — cuts a single-script rebuild to ~1-2s; build sequentially to save disk space")
message(STATUS "")
