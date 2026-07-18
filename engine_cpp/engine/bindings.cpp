/*
 * bindings.cpp — Python/pybind11 bridge (DISABLED in standalone build)
 *
 * This file is a no-op in the pure C++ standalone build (STANDALONE_CPP).
 * The CMakeLists.txt no longer builds the pybind11 extension target.
 *
 * To re-enable Python bindings (e.g. for editor integration):
 *   1. Remove -DSTANDALONE_CPP from CMake definitions.
 *   2. Add pybind11 back to CMakeLists.txt.
 *   3. Uncomment the full binding code below.
 */

// Intentionally empty for standalone build.
