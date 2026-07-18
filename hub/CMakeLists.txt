cmake_minimum_required(VERSION 3.16)

# A deliberately small standalone application: it shares the SDL2/Vulkan/
# ImGui UI stack with editor.exe but does not link any gameplay/editor runtime.
find_package(Vulkan REQUIRED)

find_package(SDL2 CONFIG QUIET)
if(NOT SDL2_FOUND)
  if(NOT SDL2_DIR)
    set(_HUB_SDL2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sdl2")
    if(DEFINED SDL2_ROOT_DIR AND EXISTS "${SDL2_ROOT_DIR}/include/SDL2/SDL.h")
      set(_HUB_SDL2_ROOT "${SDL2_ROOT_DIR}")
    elseif(DEFINED ENV{SDL2_ROOT} AND EXISTS "$ENV{SDL2_ROOT}/include/SDL2/SDL.h")
      set(_HUB_SDL2_ROOT "$ENV{SDL2_ROOT}")
    elseif(DEFINED ENV{SDL2_DIR} AND EXISTS "$ENV{SDL2_DIR}/include/SDL2/SDL.h")
      set(_HUB_SDL2_ROOT "$ENV{SDL2_DIR}")
    endif()
    set(SDL2_DIR "${_HUB_SDL2_ROOT}" CACHE PATH "SDL2 root (engine-local or environment supplied)")
  endif()
  find_path(SDL2_INCLUDE SDL2/SDL.h HINTS "${SDL2_DIR}/include" REQUIRED)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    find_library(SDL2_LIB NAMES SDL2 HINTS "${SDL2_DIR}/lib/x64" "${SDL2_DIR}/lib" REQUIRED)
    find_library(SDL2MAIN_LIB NAMES SDL2main HINTS "${SDL2_DIR}/lib/x64" "${SDL2_DIR}/lib")
    set(SDL2_DLL "${SDL2_DIR}/lib/x64/SDL2.dll")
  else()
    find_library(SDL2_LIB NAMES SDL2 HINTS "${SDL2_DIR}/lib/x86" "${SDL2_DIR}/lib" REQUIRED)
    find_library(SDL2MAIN_LIB NAMES SDL2main HINTS "${SDL2_DIR}/lib/x86" "${SDL2_DIR}/lib")
    set(SDL2_DLL "${SDL2_DIR}/lib/x86/SDL2.dll")
  endif()
endif()

find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
  if(NOT NLOHMANN_DIR)
    set(NLOHMANN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "nlohmann root (engine-local)")
  endif()
  find_path(NLOHMANN_INCLUDE nlohmann/json.hpp
    HINTS "${NLOHMANN_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../third_party" "${CMAKE_CURRENT_SOURCE_DIR}/.." REQUIRED)
endif()

if(NOT IMGUI_DIR)
  set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../editor/third_party/imgui" CACHE PATH "ImGui source root")
endif()
if(NOT EXISTS "${IMGUI_DIR}/imgui.cpp" OR NOT EXISTS "${IMGUI_DIR}/imgui.h")
  message(FATAL_ERROR "Dear ImGui source not found at: ${IMGUI_DIR}. Run scripts/fetch_deps.py before configuring.")
endif()

set(HUB_IMGUI_SOURCES
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
  ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
)

add_executable(hub
  hub_main.cpp
  ${HUB_IMGUI_SOURCES}
  ../engine_cpp/vk_render/vk_mem_alloc_impl.cpp
  ../engine_cpp/vk_render/stb_image_impl.cpp
)

if(MSVC)
  set_property(TARGET hub PROPERTY VS_USER_PROPS
    "${CMAKE_SOURCE_DIR}/cmake/msbuild_host_x64.props")
  # Avoid MSVC's asynchronous manifest-resource helper in the sync-backed
  # workspace.  It can hang after writing hub.exe and leave a corrupt file;
  # the Hub has no runtime dependency on an embedded application manifest.
  target_link_options(hub PRIVATE /MANIFEST:NO)
endif()

set_target_properties(hub PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/hub"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/hub/Release"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/hub/Debug"
)

target_include_directories(hub PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../engine_cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../engine_cpp/vk_render
  ${IMGUI_DIR}
  ${IMGUI_DIR}/backends
)
target_link_libraries(hub PRIVATE Vulkan::Vulkan)
if(TARGET SDL2::SDL2)
  target_link_libraries(hub PRIVATE SDL2::SDL2 SDL2::SDL2main)
else()
  target_include_directories(hub PRIVATE ${SDL2_INCLUDE} ${SDL2_INCLUDE}/SDL2)
  target_link_libraries(hub PRIVATE ${SDL2_LIB})
  if(SDL2MAIN_LIB)
    target_link_libraries(hub PRIVATE ${SDL2MAIN_LIB})
  endif()
endif()
if(nlohmann_json_FOUND)
  target_link_libraries(hub PRIVATE nlohmann_json::nlohmann_json)
else()
  target_include_directories(hub PRIVATE ${NLOHMANN_INCLUDE})
endif()
if(WIN32)
  target_compile_definitions(hub PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(hub PRIVATE Shell32 Ole32)
  if(DEFINED SDL2_DLL AND EXISTS "${SDL2_DLL}")
    add_custom_command(TARGET hub POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SDL2_DLL}" $<TARGET_FILE_DIR:hub>)
  endif()
endif()

# The Hub uses one authored backdrop image rather than a flat test-window
# colour.  Keep it beside hub.exe so the installed build has the same visual
# treatment as a source-tree launch.
add_custom_command(TARGET hub POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:hub>/assets"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:hub>/assets"
  COMMENT "Packaging Hub visual assets")
if(MSVC)
  target_compile_options(hub PRIVATE $<$<NOT:$<CONFIG:Debug>>:/O2> /bigobj /W3 /wd4244 /wd4267 /wd4996)
else()
  target_compile_options(hub PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
endif()

# The source projects stay usable samples. At Hub build time they are copied
# into the Hub's own package as immutable template snapshots; runtime project
# creation reads only these packaged copies, never a live games/ project.
# Game7 is the canonical authored Abyss showcase. The release template uses
# its player-facing name rather than a historical project number; it is the
# only bundled game sample. Blank 2D remains a native Hub creation mode.
set(_abyss_template_source "${CMAKE_CURRENT_SOURCE_DIR}/../games/game7")
if(NOT IS_DIRECTORY "${_abyss_template_source}")
  message(FATAL_ERROR "Abyss of Hollows source project is missing: ${_abyss_template_source}")
endif()

add_custom_command(TARGET hub POST_BUILD
  # A reused build directory must not retain obsolete samples in the final Hub.
  COMMAND ${CMAKE_COMMAND} -E rm -rf
    "$<TARGET_FILE_DIR:hub>/templates/novaSlash"
    "$<TARGET_FILE_DIR:hub>/templates/game5"
  COMMAND ${CMAKE_COMMAND}
    -DSOURCE_DIR="${_abyss_template_source}"
    -DDESTINATION_DIR="$<TARGET_FILE_DIR:hub>/templates/abyss-of-hollows"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/CopyTemplate.cmake"
  COMMENT "Packaging Abyss Hub template")

# POST_BUILD commands do not run when hub.exe itself is already up to date.
# Templates are content projects, so make their sync an explicit dependency:
# a level-art or gameplay change is carried into Hub-created Abyss projects
# even when no Hub C++ source changed in the same build.
add_custom_target(hub_templates
  COMMAND ${CMAKE_COMMAND} -E rm -rf
    "${CMAKE_BINARY_DIR}/hub/$<CONFIG>/templates/novaSlash"
    "${CMAKE_BINARY_DIR}/hub/$<CONFIG>/templates/game5"
  COMMAND ${CMAKE_COMMAND}
    -DSOURCE_DIR="${_abyss_template_source}"
    -DDESTINATION_DIR="${CMAKE_BINARY_DIR}/hub/$<CONFIG>/templates/abyss-of-hollows"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/CopyTemplate.cmake"
  COMMENT "Syncing the Abyss of Hollows Hub template")
add_dependencies(hub hub_templates)

message(STATUS "  GameEngine Hub target: hub (build/hub/Release/hub.exe)")

include(CTest)
if(BUILD_TESTING)
  add_executable(hub_tests tests/hub_tests.cpp)
  target_include_directories(hub_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
  if(nlohmann_json_FOUND)
    target_link_libraries(hub_tests PRIVATE nlohmann_json::nlohmann_json)
  else()
    target_include_directories(hub_tests PRIVATE ${NLOHMANN_INCLUDE})
  endif()
  if(WIN32)
    target_compile_definitions(hub_tests PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  endif()
  add_test(NAME hub_tests COMMAND hub_tests)
  set_tests_properties(hub_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()
