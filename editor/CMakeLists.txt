cmake_minimum_required(VERSION 3.16)
project(GameEngine2DPro_Editor CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENGINE_DETERMINISTIC "Disable deterministic fast-math paths" OFF)

if(MSVC)
  # /RTC1 is injected by CMake's Debug profile and MSVC rejects it together
  # with /O2. Keep Debug builds genuinely debuggable; optimise the profiles
  # users ship instead.
  add_compile_options($<$<NOT:$<CONFIG:Debug>>:/O2> /bigobj /W3 /wd4244 /wd4267 /wd4305 /wd4996)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_options(-O2 -Wall -Wextra -Wno-unused-parameter)
endif()
add_compile_definitions(STANDALONE_CPP NO_SDL_MIXER)
if(ENGINE_DETERMINISTIC)
  add_compile_definitions(ENGINE_DETERMINISTIC)
endif()

# ── Vulkan ──────────────────────────────────────────────────────────────────────
# Replaces SDL_Renderer/imgui_impl_sdlrenderer2 as the editor's rendering
# backend (see ../engine_cpp/vk_render/ and src/panels.hpp's ViewportPanel).
# SDL2 itself is still required below — window + input only.
find_package(Vulkan REQUIRED)
message(STATUS "Vulkan: ${Vulkan_LIBRARIES} (include: ${Vulkan_INCLUDE_DIRS})")
set(_vk_shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/../engine_cpp/vk_render/shaders")

# ── Auto-build SPIR-V shaders from GLSL (reuses engine_cpp's targets) ─────
# If compile_shaders target exists (from engine_cpp/CMakeLists.txt), depend on it.
if(TARGET compile_shaders)
  # We reference it; the add_subdirectory in the parent already makes it available.
endif()

# ── SDL2 ──────────────────────────────────────────────────────────────────────
find_package(SDL2 CONFIG QUIET)
if(NOT SDL2_FOUND)
  if(NOT SDL2_DIR)
    set(_ENGINE_SDL2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sdl2")
    if(DEFINED SDL2_ROOT_DIR AND EXISTS "${SDL2_ROOT_DIR}/include/SDL2/SDL.h")
      set(_ENGINE_SDL2_ROOT "${SDL2_ROOT_DIR}")
    elseif(DEFINED ENV{SDL2_ROOT} AND EXISTS "$ENV{SDL2_ROOT}/include/SDL2/SDL.h")
      set(_ENGINE_SDL2_ROOT "$ENV{SDL2_ROOT}")
    elseif(DEFINED ENV{SDL2_DIR} AND EXISTS "$ENV{SDL2_DIR}/include/SDL2/SDL.h")
      set(_ENGINE_SDL2_ROOT "$ENV{SDL2_DIR}")
    endif()
    set(SDL2_DIR "${_ENGINE_SDL2_ROOT}" CACHE PATH "SDL2 root (engine-local or environment supplied)")
  endif()
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
endif()

# ── SDL_image — REMOVED ─────────────────────────────────────────────────────
# Same as engine_cpp/CMakeLists.txt: image decoding now goes through
# stb_image.h, so SDL2_image is no longer needed by the editor either.
set(HAVE_SDL_IMAGE TRUE)

# ── SDL_ttf — REMOVED ──────────────────────────────────────────────────────────
# Same as engine_cpp/CMakeLists.txt: UIText/UIButton now use the Vulkan font
# atlas (vk_font_atlas.hpp), so SDL2_ttf is no longer needed by the editor.
set(HAVE_SDL_TTF FALSE)

# ── nlohmann/json ─────────────────────────────────────────────────────────────
find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
  if(NOT NLOHMANN_DIR)
    set(NLOHMANN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "nlohmann root (engine-local)")
  endif()
  find_path(NLOHMANN_INCLUDE nlohmann/json.hpp
    HINTS "${NLOHMANN_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../third_party" "${CMAKE_CURRENT_SOURCE_DIR}/.." REQUIRED)
endif()

# ── Dear ImGui ────────────────────────────────────────────────────────────────
if(NOT IMGUI_DIR)
  set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui" CACHE PATH "ImGui source root")
endif()

if(NOT EXISTS "${IMGUI_DIR}/imgui.cpp" OR NOT EXISTS "${IMGUI_DIR}/imgui.h")
  message(FATAL_ERROR
    "\n"
    "Dear ImGui source not found at: ${IMGUI_DIR}\n"
    "\n"
    "Fix this by running, from the project root, BEFORE cmake:\n"
    "    python scripts/fetch_deps.py        (Windows)\n"
    "    python3 scripts/fetch_deps.py       (Linux/Mac)\n"
    "\n"
    "This downloads Dear ImGui automatically (no git required).\n"
    "Alternatively, manually download:\n"
    "    https://github.com/ocornut/imgui/archive/refs/tags/v1.90.1.zip\n"
    "and extract it so that '${IMGUI_DIR}/imgui.h' exists.\n"
  )
endif()

set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
)

set(ENGINE_SOURCES
    ../engine_cpp/engine/core.cpp
    ../engine_cpp/engine/physics.cpp
    ../engine_cpp/engine/render_system_vk.cpp
    ../engine_cpp/engine/bindings.cpp
    ../engine_cpp/vk_render/vk_mem_alloc_impl.cpp
    ../engine_cpp/vk_render/stb_truetype_impl.cpp
    ../engine_cpp/vk_render/stb_image_impl.cpp
)

# ── Scripts now live in a separate hot-reloadable module ──────────────────────
# See editor/scripts_module/CMakeLists.txt. The editor no longer compiles any
# project's scripts/*.cpp directly — it LoadLibrary's/dlopen's the built
# game_scripts module at runtime instead (see script_module_loader.hpp). This
# is what lets a script rebuild happen without restarting the editor process:
# only the small scripts DLL gets relinked, not the whole editor binary.

# ── Editor executable ─────────────────────────────────────────────────────────
add_executable(editor editor_main.cpp ${ENGINE_SOURCES} ${IMGUI_SOURCES})

# VS global properties are emitted before Microsoft.Cpp.props and are reset to
# x86 by the C++ defaults.  Import this sheet afterwards so normal Editor
# builds use HostX64 tools (the documented MSBuild placement).
if(MSVC)
  set_property(TARGET editor PROPERTY VS_USER_PROPS
    "${CMAKE_SOURCE_DIR}/cmake/msbuild_host_x64.props")
  # The workspace is sync-backed and Windows has repeatedly left the MSVC
  # manifest-resource helper (`rc.exe`) hung after it wrote editor.exe.  That
  # produces a truncated, non-launchable executable even though compilation
  # succeeded.  The editor does not require an embedded application manifest,
  # so disable only that post-link embedding step; normal code signing and the
  # installer manifest remain independent of this executable setting.
  target_link_options(editor PRIVATE /MANIFEST:NO)
endif()

# Auto-build shaders before editor build (if glslc available)
if(TARGET compile_shaders)
  add_dependencies(editor compile_shaders)
endif()

# Auto-rebuild (Rebuild Scripts / before Play) links to a fresh, uniquely
# named exe instead of overwriting the one currently running — on Windows
# especially, a running .exe is kept open for write by the OS for as long as
# it's mapped into a live process, so linking directly onto
# build/editor.exe while that exact binary is executing this build command
# fails with LNK1104. EDITOR_OUTPUT_NAME defaults to "editor" so a normal
# first build / manual `cmake --build` is unaffected.
set(EDITOR_OUTPUT_NAME "editor" CACHE STRING "Output filename for the editor executable (override on rebuild to avoid overwriting the running exe)")
set_target_properties(editor PROPERTIES OUTPUT_NAME "${EDITOR_OUTPUT_NAME}")

target_include_directories(editor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../engine_cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../engine_cpp/vk_render
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(editor PRIVATE Vulkan::Vulkan)

if(WIN32)
  target_link_libraries(editor PRIVATE shell32 ole32)
endif()

if(TARGET SDL2::SDL2)
  target_link_libraries(editor PRIVATE SDL2::SDL2 SDL2::SDL2main)
else()
  target_include_directories(editor PRIVATE ${SDL2_INCLUDE} ${SDL2_INCLUDE}/SDL2)
  target_link_libraries(editor PRIVATE ${SDL2_LIB})
  if(SDL2MAIN_LIB)
    target_link_libraries(editor PRIVATE ${SDL2MAIN_LIB})
  endif()
endif()


# SDL_image intentionally not linked — see the "SDL_image — REMOVED" comment above.
# SDL_ttf intentionally not linked — see the "SDL_ttf — REMOVED" comment above.

if(nlohmann_json_FOUND)
  target_link_libraries(editor PRIVATE nlohmann_json::nlohmann_json)
else()
  target_include_directories(editor PRIVATE ${NLOHMANN_INCLUDE})
endif()

if(UNIX AND NOT APPLE)
  target_link_libraries(editor PRIVATE m pthread stdc++fs dl)
endif()

if(WIN32)
  target_link_libraries(editor PRIVATE Ws2_32 Dbghelp)
endif()

if(WIN32)
  foreach(_dll IN ITEMS SDL2_DLL)
    if(DEFINED ${_dll} AND EXISTS "${${_dll}}")
      add_custom_command(TARGET editor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${${_dll}}" $<TARGET_FILE_DIR:editor>)
    endif()
  endforeach()
endif()

# ── Copy ALL compiled SPV shaders next to editor.exe ─────────────────────────
# Covers sprite batch + post-process composite + GPU particle compute.
# Add new shaders here whenever a new .spv is introduced.
set(_vk_spv_shaders
    "${_vk_shader_dir}/sprite.vert.spv"
    "${_vk_shader_dir}/sprite.frag.spv"
    "${_vk_shader_dir}/sprite_lit.vert.spv"
    "${_vk_shader_dir}/sprite_lit.frag.spv"
    "${_vk_shader_dir}/fullscreen.vert.spv"
    "${_vk_shader_dir}/composite.frag.spv"
    "${_vk_shader_dir}/particle.comp.spv"
    "${_vk_shader_dir}/sprite_inst.vert.spv"
    "${_vk_shader_dir}/sprite_custom_example.frag.spv"
)

set(_all_spv_found TRUE)
foreach(_spv IN LISTS _vk_spv_shaders)
  if(NOT EXISTS "${_spv}")
    set(_all_spv_found FALSE)
    message(WARNING "Missing SPV shader: ${_spv} — compile with: glslc <source> -o <output>.spv")
  endif()
endforeach()

if(_all_spv_found)
  add_custom_command(TARGET editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:editor>/shaders
    COMMENT "Copying all SPV shaders → editor/shaders/")
  foreach(_spv IN LISTS _vk_spv_shaders)
    add_custom_command(TARGET editor POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${_spv}"
              "$<TARGET_FILE_DIR:editor>/shaders/")
  endforeach()
else()
  message(WARNING "One or more SPV shaders missing — compile them with glslc before building.")
endif()

# ── Copy default UIText/UIButton font next to editor.exe ─────────────────────
# RenderSystem::load_default_font() (engine_cpp/render_system_vk.hpp) loads
# "assets/fonts/default.ttf" relative to the process's cwd at startup — same
# relative-path lookup pattern shaders use (see editor_main.cpp's
# get_executable_dir() fix), so it needs to exist next to editor.exe too.
set(_default_font "${CMAKE_CURRENT_SOURCE_DIR}/../engine_cpp/assets/fonts/default.ttf")
if(EXISTS "${_default_font}")
  add_custom_command(TARGET editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:editor>/assets/fonts
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_default_font}" $<TARGET_FILE_DIR:editor>/assets/fonts
    COMMENT "Copying default UIText font (default.ttf)")
else()
  message(WARNING "engine_cpp/assets/fonts/default.ttf not found — "
                   "UIText/UIButton will fall back to the built-in pixel font at runtime.")
endif()

message(STATUS "")
message(STATUS "  GameEngine2D Pro — Editor")
message(STATUS "  ImGui : ${IMGUI_DIR}")
message(STATUS "  Vulkan: ${Vulkan_LIBRARIES}")
message(STATUS "  SDL2  : OK | SDL_image: removed (stb_image.h) | SDL_ttf: removed (Vulkan font atlas)")
message(STATUS "")
