@echo off
REM build_windows.bat — Build game engine + editor on Windows
REM Run from the project root. CMake first uses the engine-local SDK payload,
REM then SDL2_ROOT/SDL2_DIR and NLOHMANN_DIR if they are explicitly supplied.
REM Now includes auto SPIR-V shader compilation via glslc (Vulkan SDK).

if "%SDL2_DIR%"=="" if not "%SDL2_ROOT%"=="" set SDL2_DIR=%SDL2_ROOT%
if "%NLOHMANN_DIR%"=="" set NLOHMANN_DIR=%CD%

REM Check for Vulkan SDK (required for shader compilation)
if "%VULKAN_SDK%"=="" (
    echo [WARN] VULKAN_SDK not set — shaders will NOT be auto-compiled.
    echo [WARN] Install Vulkan SDK from https://vulkan.lunarg.com/
    echo [WARN] SPIR-V .spv files must already exist or rendering will fail.
) else (
    echo [INFO] Vulkan SDK found: %VULKAN_SDK%
    echo [INFO] Shaders will be auto-compiled from GLSL on build.
)

python scripts/fetch_deps.py
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] fetch_deps.py failed. Is Python installed? Check internet connection.
    pause & exit /b 1
)

cmake -B build -DSDL2_DIR="%SDL2_DIR%" -DNLOHMANN_DIR="%NLOHMANN_DIR%" ^
      -DIMGUI_DIR="%CD%/editor/third_party/imgui"
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] CMake configuration failed.
    pause & exit /b 1
)

cmake --build build --config Release
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Build failed.
    pause & exit /b 1
)

echo.
echo ========================================================
echo  Build complete!
echo  Editor: build/editor/Release/editor.exe
echo  Hub:    build/hub/Release/hub.exe
echo  Scripts: build/editor/scripts_module/Release/
echo  Shaders: Auto-built from GLSL (if glslc available)
echo ========================================================
echo.
pause
