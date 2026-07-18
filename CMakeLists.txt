cmake_minimum_required(VERSION 3.16)

project(GameEngine2DPro CXX)

# Enables the Hub's focused manifest/filesystem integration test in CTest.
include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The editor builds the engine code directly; the standalone export is built
# on demand from inside the editor.
set(BUILD_STANDALONE OFF CACHE BOOL "Build standalone engine executable" FORCE)

# ── Build all subprojects ─────────────────────────────────────────────────────
add_subdirectory(engine_cpp)
add_subdirectory(editor/scripts_module)
add_subdirectory(editor)
add_subdirectory(hub)

message(STATUS "")
message(STATUS "  GameEngine2DPro — Full C++ Build")
message(STATUS "  Targets: editor, hub (project manager), standalone export via editor")
message(STATUS "")
message(STATUS "  Before building, run:  scripts/fetch_imgui.py")
message(STATUS "  or manually clone Dear ImGui into editor/third_party/imgui")
message(STATUS "")
