cmake_minimum_required(VERSION 3.16)
project(GameEngine2DPro CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_STANDALONE "Build the standalone game executable" ON)
option(ENGINE_DETERMINISTIC "Disable fast-math and enable deterministic helpers" OFF)

# Name of the standalone executable target/binary. Each project gets its own
# exe (e.g. -DGAME_PROJECT_NAME=game3 -> game3.exe) instead of every project
# sharing one generic "standalone_game.exe". Defaults to "standalone_game"
# for backward compatibility when not specified by the editor's export step.
if(NOT GAME_PROJECT_NAME)
  set(GAME_PROJECT_NAME "standalone_game")
endif()

# ── Compiler flags ─────────────────────────────────────────────────────────────
if(MSVC)
  # CMake Debug uses /RTC1, which cannot be combined with /O2 on MSVC.
  # Apply optimisation only to non-Debug configurations.
  add_compile_options($<$<NOT:$<CONFIG:Debug>>:/O2> /W3 /wd4244 /wd4267 /wd4305 /wd4996)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  if(NOT ENGINE_DETERMINISTIC)
    add_compile_options(-O3 -Wall -Wextra -Wno-unused-parameter -ffast-math)
  else()
    add_compile_options(-O3 -Wall -Wextra -Wno-unused-parameter)
  endif()
endif()

# ── Disable Python bindings (pure C++ standalone) ─────────────────────────────
add_compile_definitions(STANDALONE_CPP)
if(ENGINE_DETERMINISTIC)
  add_compile_definitions(ENGINE_DETERMINISTIC)
endif()

# ── SDL_image — REMOVED ─────────────────────────────────────────────────────
# Image decoding (PNG/JPG/BMP/GIF/TGA/PSD) is now handled by stb_image.h
# (engine_cpp/third_party/stb_image.h, implementation in
# vk_render/stb_image_impl.cpp) — a single cross-platform decoder replacing
# the old SDL2_image / Windows-WIC / SDL_LoadBMP-only fallback chain. No
# SDL2_image dependency, no Windows-only code path.
set(HAVE_SDL_IMAGE TRUE)

# ── SDL2 ──────────────────────────────────────────────────────────────────────
find_package(SDL2 CONFIG QUIET)
if(NOT SDL2_FOUND)
  if(NOT SDL2_DIR)
    if(DEFINED SDL2_ROOT_DIR AND EXISTS "${SDL2_ROOT_DIR}/include/SDL2/SDL.h")
      set(SDL2_DIR "${SDL2_ROOT_DIR}" CACHE PATH "SDL2 root" FORCE)
    endif()
  endif()
  if(NOT SDL2_DIR)
    set(_SDL2_SEARCH_ROOTS
      "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sdl2"
      "${CMAKE_CURRENT_SOURCE_DIR}/../SDL2"
      "${CMAKE_CURRENT_SOURCE_DIR}/../../SDL2"
      "$ENV{SDL2_ROOT}"
      "$ENV{SDL2_DIR}"
      "/usr/local"
      "/usr"
    )
    foreach(_root ${_SDL2_SEARCH_ROOTS})
      if(EXISTS "${_root}/include/SDL2/SDL.h")
        set(SDL2_DIR "${_root}" CACHE PATH "SDL2 root" FORCE)
        break()
      endif()
    endforeach()
  endif()
  if(NOT SDL2_DIR)
    message(FATAL_ERROR "SDL2 development files were not found. Set SDL2_ROOT or use the engine-local third_party/sdl2 SDK installed by GameEngine2D Pro.")
  endif()

  message(STATUS "SDL2_DIR = ${SDL2_DIR}")
  find_path(SDL2_INCLUDE SDL2/SDL.h HINTS "${SDL2_DIR}/include" REQUIRED)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    find_library(SDL2_LIB     NAMES SDL2     HINTS "${SDL2_DIR}/lib/x64" "${SDL2_DIR}/lib" REQUIRED)
    find_library(SDL2MAIN_LIB NAMES SDL2main HINTS "${SDL2_DIR}/lib/x64" "${SDL2_DIR}/lib")
    set(SDL2_DLL "${SDL2_DIR}/lib/x64/SDL2.dll")
  else()
    find_library(SDL2_LIB     NAMES SDL2     HINTS "${SDL2_DIR}/lib/x86" "${SDL2_DIR}/lib" REQUIRED)
    find_library(SDL2MAIN_LIB NAMES SDL2main HINTS "${SDL2_DIR}/lib/x86" "${SDL2_DIR}/lib")
    set(SDL2_DLL "${SDL2_DIR}/lib/x86/SDL2.dll")
  endif()
  message(STATUS "SDL2 (manual): ${SDL2_LIB}")
endif()

# ── SDL_ttf — REMOVED ──────────────────────────────────────────────────────────
# UIText/UIButton now render through a real Vulkan-uploaded font atlas
# (engine_cpp/vk_render/vk_font_atlas.hpp, stb_truetype-based) rather than
# SDL_ttf. The only consumer of SDL_ttf/TTF_*/USE_SDL_TTF left in this repo
# is engine_cpp/engine/render_system.cpp, the old SDL_Renderer-based file,
# which neither this build nor the editor build compiles anymore (see
# ENGINE_SOURCES below) — so SDL2_ttf is no longer found or linked.
set(HAVE_SDL_TTF FALSE)

# ── Vulkan ──────────────────────────────────────────────────────────────────────
# Replaces SDL2's SDL_Renderer as the rendering backend (see vk_render/).
# SDL2 itself is still required above — window creation + input only.
# A normal editor launch on Windows does not inherit a developer command
# prompt's VULKAN_SDK variable. FindVulkan would consequently miss a valid
# SDK and the Build button would fail before compiling. The installer installs
# the SDK and sets VULKAN_SDK; developers may override Vulkan_ROOT explicitly.
if(NOT Vulkan_ROOT AND DEFINED ENV{VULKAN_SDK} AND EXISTS "$ENV{VULKAN_SDK}")
  set(Vulkan_ROOT "$ENV{VULKAN_SDK}" CACHE PATH "Vulkan SDK root" FORCE)
endif()
find_package(Vulkan REQUIRED)
message(STATUS "Vulkan: ${Vulkan_LIBRARIES} (include: ${Vulkan_INCLUDE_DIRS})")

# Copy the prebuilt sprite shaders next to the executable. vk_sprite_batch.hpp
# loads sprite.vert.spv/sprite.frag.spv from disk at runtime (shader_dir arg
# to RenderSystem's constructor, default "shaders"), not compiled in, so they
# need to exist next to the built binary. If you edit the .vert/.frag source,
# the .spv files are auto-regenerated below via glslc (Vulkan SDK).
set(_vk_shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/vk_render/shaders")

# ── Auto-build SPIR-V from GLSL sources (Vulkan SDK glslc) ────────────────
# This replaces the manual "compile with glslc" workflow. Any .vert/.frag/.comp
# file in the shaders directory is automatically compiled when its source changes.
# This is a massive quality-of-life improvement over Unity which requires manual
# shader compilation or a separate asset post-processor.
find_program(GLSLC_EXECUTABLE glslc
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
    DOC "Path to glslangValidator or glslc from Vulkan SDK")

if(GLSLC_EXECUTABLE)
    message(STATUS "glslc found: ${GLSLC_EXECUTABLE}")
    file(GLOB_RECURSE _glsl_sources
        "${_vk_shader_dir}/*.vert"
        "${_vk_shader_dir}/*.frag"
        "${_vk_shader_dir}/*.comp"
    )
    foreach(_src IN LISTS _glsl_sources)
        get_filename_component(_name "${_src}" NAME)
        set(_spv "${_src}.spv")
        add_custom_command(
            OUTPUT "${_spv}"
            DEPENDS "${_src}"
            COMMAND "${GLSLC_EXECUTABLE}" "${_src}" -o "${_spv}"
            COMMENT "Compiling ${_name} → ${_name}.spv"
        )
        list(APPEND _generated_spv "${_spv}")
    endforeach()
    # Create a custom target that depends on ALL shaders so CMake rebuilds them
    # when any GLSL source changes. Targets that need the shaders can depend on this.
    add_custom_target(compile_shaders DEPENDS ${_generated_spv})
    message(STATUS "Shader auto-build enabled: ${_glsl_sources} sources → ${_generated_spv} SPIR-V")
else()
    message(WARNING "glslc not found — SPIR-V shaders will NOT be auto-compiled. "
                    "Install Vulkan SDK or compile manually with glslc.")
endif()

# ── nlohmann/json ─────────────────────────────────────────────────────────────
find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
  if(NOT NLOHMANN_DIR)
    set(NLOHMANN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "Path containing nlohmann/json.hpp (engine-local)")
  endif()
  find_path(NLOHMANN_INCLUDE nlohmann/json.hpp
    HINTS "${NLOHMANN_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/third_party" "${CMAKE_CURRENT_SOURCE_DIR}/.." REQUIRED)
  message(STATUS "nlohmann/json (manual): ${NLOHMANN_INCLUDE}")
endif()

# ── Generated script-include header ───────────────────────────────────────────
# The standalone entry point includes generated_game_scripts.hpp, which in turn
# includes either the engine default script registry header or a project-local
# header written by the editor during export.
set(_generated_scripts_header "${CMAKE_CURRENT_BINARY_DIR}/generated_game_scripts.hpp")
if(NOT EXISTS "${_generated_scripts_header}")
  file(WRITE "${_generated_scripts_header}"
    "#pragma once
"
    "// Generated header intentionally left empty until the editor exports project scripts.
"
    "#include \"script_system.hpp\"
")

endif()

# ── Engine sources ─────────────────────────────────────────────────────────────
set(ENGINE_SOURCES
    engine/core.cpp
    engine/physics.cpp
    engine/physics_ext.cpp
    engine/render_system_vk.cpp
    vk_render/vk_mem_alloc_impl.cpp
    vk_render/stb_truetype_impl.cpp
    vk_render/stb_image_impl.cpp
)

# ── Helper: link SDL2 + json to target ────────────────────────────────────────
function(attach_deps target)
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/engine)
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vk_render)

  target_link_libraries(${target} PRIVATE Vulkan::Vulkan)

  if(TARGET SDL2::SDL2)
    target_link_libraries(${target} PRIVATE SDL2::SDL2 SDL2::SDL2main)
  else()
    target_include_directories(${target} PRIVATE ${SDL2_INCLUDE})
    target_link_libraries(${target} PRIVATE ${SDL2_LIB})
    if(SDL2MAIN_LIB)
      target_link_libraries(${target} PRIVATE ${SDL2MAIN_LIB})
    endif()
  endif()

  # ── SDL2_mixer (audio) ───────────────────────────────────────────────────────
  # Re-enabled as part of task 3 (audio parity fix). Find either the modern
  # CMake target or fall back to the SDL2MIXER_LIB / SDL2MIXER_INCLUDE cache
  # variables set by the project's find_package / manual SDL2 setup.
  if(TARGET SDL2_mixer::SDL2_mixer)
    target_link_libraries(${target} PRIVATE SDL2_mixer::SDL2_mixer)
  elseif(TARGET SDL2::SDL2_mixer)
    target_link_libraries(${target} PRIVATE SDL2::SDL2_mixer)
  elseif(DEFINED SDL2MIXER_LIB)
    if(DEFINED SDL2MIXER_INCLUDE)
      target_include_directories(${target} PRIVATE ${SDL2MIXER_INCLUDE})
    endif()
    target_link_libraries(${target} PRIVATE ${SDL2MIXER_LIB})
  else()
    find_library(SDL2_MIXER_LIB NAMES SDL2_mixer SDL2_mixer-2.0)
    if(SDL2_MIXER_LIB)
      target_link_libraries(${target} PRIVATE ${SDL2_MIXER_LIB})
    else()
      message(WARNING "SDL2_mixer not found — audio will be unavailable at runtime.")
      target_compile_definitions(${target} PRIVATE NO_SDL_MIXER)
    endif()
  endif()

  # SDL_image intentionally not linked — see the "SDL_image — REMOVED"
  # comment near the top of this file (stb_image.h handles decoding now).

  # SDL_ttf intentionally not linked — see the "SDL_ttf — REMOVED" comment
  # near the top of this file.

  if(nlohmann_json_FOUND)
    target_link_libraries(${target} PRIVATE nlohmann_json::nlohmann_json)
  else()
    target_include_directories(${target} PRIVATE ${NLOHMANN_INCLUDE})
  endif()

  # windowscodecs/ole32 (WIC) no longer needed — image decoding moved to
  # stb_image.h, which has no platform-specific COM dependency.

  # Linux
  if(UNIX AND NOT APPLE)
    target_link_libraries(${target} PRIVATE m pthread)
  endif()

  if(WIN32)
    target_link_libraries(${target} PRIVATE Ws2_32 Dbghelp)
  endif()
endfunction()

# ── Standalone executable (built on demand) ─────────────────────────────────
if(BUILD_STANDALONE)
  add_executable(${GAME_PROJECT_NAME} ${ENGINE_SOURCES} main.cpp)
  if(MSVC)
    set_property(TARGET ${GAME_PROJECT_NAME} PROPERTY VS_USER_PROPS
      "${CMAKE_SOURCE_DIR}/cmake/msbuild_host_x64.props")
  endif()
  attach_deps(${GAME_PROJECT_NAME})
  # physics_ext.cpp is part of ENGINE_SOURCES for standalone exports. Tell
  # physics.cpp to use those real extension definitions instead of compiling
  # its editor/script-DLL fallback copies, which would otherwise duplicate
  # symbols at link time.
  target_compile_definitions(${GAME_PROJECT_NAME} PRIVATE PHYSICS_EXT_LINKED=1)

  # Set WIN32 subsystem only on Windows to suppress console window (optional)
  if(WIN32)
    # Keep console for now for debug output; remove CONSOLE flag for release:
    # set_target_properties(${GAME_PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
  endif()

  # ── Auto-build shaders before standalone (if glslc available) ──────────────
  if(TARGET compile_shaders)
    add_dependencies(${GAME_PROJECT_NAME} compile_shaders)
  endif()

  # ── Copy every runtime SPIR-V shader next to the executable ────────────────
  # RenderSystem also creates fullscreen/composite/particle pipelines.  Copying
  # only sprite shaders made a standalone pass compilation yet fail on launch
  # with "Cannot open shader: fullscreen.vert.spv".
  add_custom_command(TARGET ${GAME_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${GAME_PROJECT_NAME}>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_vk_shader_dir}/fullscreen.vert.spv" "${_vk_shader_dir}/composite.frag.spv"
        "${_vk_shader_dir}/particle.comp.spv"
        "${_vk_shader_dir}/sprite.vert.spv" "${_vk_shader_dir}/sprite.frag.spv"
        "${_vk_shader_dir}/sprite_lit.vert.spv" "${_vk_shader_dir}/sprite_lit.frag.spv"
        "${_vk_shader_dir}/sprite_inst.vert.spv" "${_vk_shader_dir}/sprite_custom_example.frag.spv"
        $<TARGET_FILE_DIR:${GAME_PROJECT_NAME}>/shaders
    COMMENT "Copying complete Vulkan runtime shader set")

  # ── Copy DLLs next to .exe on Windows ──────────────────────────────────────
  if(WIN32)
    foreach(_dll IN ITEMS SDL2_DLL)
      if(DEFINED ${_dll} AND EXISTS "${${_dll}}")
        add_custom_command(TARGET ${GAME_PROJECT_NAME} POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different "${${_dll}}" $<TARGET_FILE_DIR:${GAME_PROJECT_NAME}>
          COMMENT "Copying ${_dll}")
      endif()
    endforeach()
  endif()

  # ── Copy compiled sprite shaders next to the executable ───────────────────
  if(EXISTS "${_vk_shader_dir}/sprite.vert.spv" AND EXISTS "${_vk_shader_dir}/sprite.frag.spv")
    add_custom_command(TARGET ${GAME_PROJECT_NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${GAME_PROJECT_NAME}>/shaders
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${_vk_shader_dir}/sprite.vert.spv" "${_vk_shader_dir}/sprite.frag.spv"
              $<TARGET_FILE_DIR:${GAME_PROJECT_NAME}>/shaders
      COMMENT "Copying compiled sprite shaders (sprite.vert.spv / sprite.frag.spv)")
  else()
    message(WARNING "vk_render/shaders/sprite.{vert,frag}.spv not found — "
                     "compile them with glslangValidator (or glslc) before building, "
                     "or RenderSystem's default shader_dir=\"shaders\" lookup will fail at runtime.")
  endif()

  # ── Copy compiled sprite_lit shaders (task 6: real Light2D) ────────────────
  # Compile: glslc vk_render/shaders/sprite_lit.vert -o vk_render/shaders/sprite_lit.vert.spv
  #          glslc vk_render/shaders/sprite_lit.frag -o vk_render/shaders/sprite_lit.frag.spv
  if(EXISTS "${_vk_shader_dir}/sprite_lit.vert.spv" AND EXISTS "${_vk_shader_dir}/sprite_lit.frag.spv")
    add_custom_command(TARGET ${GAME_PROJECT_NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${_vk_shader_dir}/sprite_lit.vert.spv" "${_vk_shader_dir}/sprite_lit.frag.spv"
              $<TARGET_FILE_DIR:${GAME_PROJECT_NAME}>/shaders
      COMMENT "Copying compiled Sprite-Lit shaders (sprite_lit.vert.spv / sprite_lit.frag.spv)")
  else()
    message(WARNING "vk_render/shaders/sprite_lit.{vert,frag}.spv not found — "
                     "compile them with glslc before building (Sprite-Lit materials will crash at runtime): "
                     "  glslc vk_render/shaders/sprite_lit.vert -o vk_render/shaders/sprite_lit.vert.spv"
                     "  glslc vk_render/shaders/sprite_lit.frag -o vk_render/shaders/sprite_lit.frag.spv")
  endif()

  # ── Copy default UIText/UIButton font next to the executable ──────────────
  # RenderSystem::load_default_font() is called from core.cpp with an
  # exe-relative path (get_executable_dir() + "/assets/fonts/default.ttf") —
  # same resolution pattern as shader_dir above — so the file needs to exist
  # there at runtime.
  set(_default_font "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/default.ttf")
  if(EXISTS "${_default_font}")
    add_custom_command(TARGET ${GAME_PROJECT_NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${GAME_PROJECT_NAME}>/assets/fonts
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${_default_font}" $<TARGET_FILE_DIR:${GAME_PROJECT_NAME}>/assets/fonts
      COMMENT "Copying default UIText font (default.ttf)")
  else()
    message(WARNING "engine_cpp/assets/fonts/default.ttf not found — "
                     "UIText/UIButton will fall back to the built-in pixel font at runtime.")
  endif()
endif()

if(BUILD_TESTING)
  add_executable(runtime_safety_tests
    tests/runtime_safety_tests.cpp
    engine/physics.cpp)
  if(MSVC)
    set_property(TARGET runtime_safety_tests PROPERTY VS_USER_PROPS
      "${CMAKE_SOURCE_DIR}/cmake/msbuild_host_x64.props")
  endif()
  target_include_directories(runtime_safety_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  # The test reaches the scripting header and core physics implementation to
  # validate host-bound diagnostics plus lifecycle/cache safety; both expose
  # the Input API and therefore need SDL's include path.
  if(TARGET SDL2::SDL2)
    target_link_libraries(runtime_safety_tests PRIVATE SDL2::SDL2)
  else()
    target_include_directories(runtime_safety_tests PRIVATE ${SDL2_INCLUDE})
    target_link_libraries(runtime_safety_tests PRIVATE ${SDL2_LIB})
  endif()
  if(nlohmann_json_FOUND)
    target_link_libraries(runtime_safety_tests PRIVATE nlohmann_json::nlohmann_json)
  else()
    target_include_directories(runtime_safety_tests PRIVATE ${NLOHMANN_INCLUDE})
  endif()
  # Keep the lightweight runtime test target buildable on developer machines
  # that intentionally use the engine's no-audio fallback.  It exercises the
  # particle system but does not exercise AudioSystem, and systems.hpp hides
  # the mixer API behind this same production compile-time switch.
  if(NOT TARGET SDL2_mixer::SDL2_mixer AND NOT TARGET SDL2::SDL2_mixer AND
     NOT DEFINED SDL2MIXER_LIB AND NOT SDL2_MIXER_LIB)
    target_compile_definitions(runtime_safety_tests PRIVATE NO_SDL_MIXER)
  endif()
  if(WIN32)
    target_compile_definitions(runtime_safety_tests PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  endif()
  add_test(NAME runtime_safety_tests COMMAND runtime_safety_tests)
  set_tests_properties(runtime_safety_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

message(STATUS "")
message(STATUS "  GameEngine2DPro — Pure C++ Standalone")
message(STATUS "  SDL2      : ${SDL2_LIB}${SDL2_LIBRARIES}")
message(STATUS "  SDL_image : removed (texture loading now uses stb_image.h — PNG/JPG/BMP/GIF/TGA/PSD, cross-platform)")
message(STATUS "  SDL_ttf   : removed (UIText now uses the Vulkan font atlas — see vk_font_atlas.hpp)")
message(STATUS "  nlohmann  : OK")
message(STATUS "  Python    : NOT REQUIRED (standalone)")
message(STATUS "")
