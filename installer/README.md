# Windows Installer Build Notes

This folder contains the Inno Setup definition and packaging notes for the GameEngine2D Pro Windows release. It is for release maintenance; it is not the end-user installation guide.

## Build the installer

DOWNLOAD THE INSTALLER AND ITS READY TO BE USED

## Installed payload

The installer places the engine under `%LocalAppData%\GameEngine2D Pro` by default. The installed payload includes:

- GameEngine Hub and Editor
- SDL runtime, shaders, runtime assets, and editor symbols
- The **Abyss of Hollows** Hub template
- Engine and editor source/headers required for native C++ script synchronization
- A writable `games\` directory for Hub-created projects

## Required dependencies

The setup handles the authoring dependencies used by the native script workflow:

- Microsoft Visual C++ x64 Redistributable
- Visual Studio Code, when it is not already registered for the current user
- Visual Studio Build Tools with MSVC, MSBuild, and CMake components
- Vulkan Runtime loader when Windows does not already provide one
- Vulkan SDK for Editor and standalone-build authoring

SDL is bundled with the engine and does not require a separate SDK installation.

The Microsoft Build Tools bootstrapper downloads its selected workload during installation. Reserve approximately 10 GB and an internet connection for that step. The installer does not attempt to replace NVIDIA, AMD, or Intel graphics drivers; a Vulkan-capable GPU driver remains a system requirement.

## Release notes

The installer is currently unsigned. We'll Sign the final executable with an Authenticode certificate before a public commercial release.

`THIRD_PARTY_NOTICES.md` is intentionally included in the installed payload. It records the license and attribution information required for bundled third-party components and assets.

## Upstream prerequisite sources

- Microsoft VC++ Redistributable: `https://aka.ms/vc14/vc_redist.x64.exe`
- Microsoft Build Tools: `https://aka.ms/vs/17/release/vs_buildtools.exe`
- Visual Studio Code: `https://update.code.visualstudio.com/latest/win32-x64-user/stable`
- Vulkan SDK and Runtime: LunarG's official download service
