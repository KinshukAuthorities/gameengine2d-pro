#!/bin/bash
# build_linux.sh — Build game engine + editor on Linux
# sudo apt install libsdl2-dev libsdl2-image-dev libsdl2-ttf-dev nlohmann-json3-dev

set -e
python3 scripts/fetch_deps.py

cmake -B build \
  -DIMGUI_DIR="$(pwd)/editor/third_party/imgui" \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --parallel $(nproc)

echo ""
echo "Build complete. Binary: build/editor"

echo "Standalone export is built from inside the editor."
