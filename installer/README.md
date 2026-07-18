# GameEngine2D Pro Windows installer

`scripts\build_installer.ps1` creates the release installer. It is the only
supported packaging entry point; it stages the exact engine files and obtains
prerequisite installers directly from their official publishers.

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_installer.ps1
```

The script performs the Release editor/Hub build first unless `-SkipBuild` is
specified. It then downloads or reuses a signed cache in
`build\installer_prerequisites`, validates each executable as a Windows PE
file and verifies its Authenticode publisher before embedding it. Use
`-SkipPrerequisiteDownload` only for a repeat packaging pass that already has
the verified cache. The generated installer displays the exact version, file
size and SHA-256 for the actual binaries embedded in that release.

## What the installer installs

The engine itself is per-user at `%LocalAppData%\GameEngine2D Pro`, including
Hub, Editor, game templates, shaders, assets, engine headers/source, and a
writable `games\` directory. The installer also installs or updates the
following required authoring dependencies:

- Microsoft Visual C++ x64 Redistributable
- Visual Studio Code (per-user x64 setup, skipped when a registered install is
  already present)
- Visual Studio Build Tools: MSVC x64/x86 tools, MSBuild, CMake project tools,
  and recommended components
- LunarG/Khronos Vulkan Runtime loader when Windows has no loader
- LunarG Vulkan SDK for compiling the Editor and standalone exports

SDL2 is not a machine prerequisite. `SDL2.dll`, headers and the x64 import
library are packaged alongside the engine, so a user never has to locate or
install a separate SDL SDK.

The Visual Studio Build Tools EXE is Microsoft’s small official bootstrapper:
the setup shows its exact bootstrap size, but Microsoft downloads the selected
MSVC/CMake workload during install. Reserve roughly 10 GB and require an
internet connection for that one installation phase. An entirely offline
Build Tools layout is deliberately not claimed by this installer because it
would add many gigabytes and must be generated from Microsoft’s current
workload manifest.

The setup can install the Vulkan loader/runtime, but no universal installer
can provide a Vulkan-capable GPU driver/ICD. That part comes from the actual
NVIDIA, AMD, or Intel driver for the user’s GPU. The wizard runs this check
automatically after setup and gives a clear blocking message if the loader is
still unavailable; it never overwrites graphics drivers.

Shared system dependencies (VC++ runtime, Build Tools, VS Code, Vulkan SDK and
Vulkan Runtime) are not removed by uninstalling the engine. That prevents an
engine uninstall from breaking other software. The engine’s own files, Hub
shortcuts, and per-user installation are removed normally.

The installer is currently unsigned. Add an Authenticode signing certificate
to the release pipeline before public distribution.

## Upstream sources and notices

- VC++ Redist: Microsoft `https://aka.ms/vc14/vc_redist.x64.exe`
- Build Tools: Microsoft `https://aka.ms/vs/17/release/vs_buildtools.exe`
- VS Code user setup: `https://update.code.visualstudio.com/latest/win32-x64-user/stable`
- Vulkan SDK/runtime: LunarG’s official version/download API

The package downloads these binaries unmodified. Review the upstream licence
terms and notices before distributing a public installer.
