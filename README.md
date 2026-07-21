# GameEngine2D Pro

GameEngine2D Pro is an integrated 2D game-development platform. It brings together a project Hub, a Vulkan/SDL editor, native C++ scripting with targeted reloads, visual authoring tools, and standalone Windows export in one workflow.

This repository also includes **Abyss of Hollows**, an original action-adventure showcase project used to demonstrate the complete project-to-export path.

**OpenAI Build Week 2026 — Developer Tools**

## What I built

- **GameEngine Hub** — create, find, import, duplicate, rename, and open projects from one place.
- **Editor** — a 2D viewport, hierarchy, inspector, assets browser, scene and prefab workflow, tilemap tooling, animator, console, build settings, and visual scripting.
- **Native scripting** — C++ gameplay scripts used.
- **Visual Scripting**- C++ Scripts but in Node Scripting format, so no need for writing long scripts
- **Runtime and export** — SDL input/windowing, Vulkan rendering, scene loading, 2D physics, UI, prefabs, tilemaps, and Windows standalone builds.
- **Abyss of Hollows** — the bundled sample template, including scenes, assets, and native gameplay scripts.

## Judge quick start

> **Run the release installer, not Code → Download ZIP.** The ZIP is source code for review and reproducible builds. It intentionally excludes generated build output and the installer executable.

1. Download `GameEngine2DPro-Setup-1.0.0-x64.exe` from the repository's [v1.0.0 GitHub Release](../../releases/latest).
2. Run the installer. It installs GameEngine2D Pro to `%LocalAppData%\GameEngine2D Pro` by default and sets up the authoring prerequisites needed for native C++ scripts.
3. Launch **GameEngine Hub** from the Start Menu.
4. Create a project with the **Abyss of Hollows** template or create a new blank 2d, then open it in the Editor.
5. The Abyss of Hollows template includes prebuilt native gameplay modules, just click on play in editor to start playing..
6. Press **Play** in the Editor to run the active scene.
7. **PLEASE NOTE THAT IF THE HUB DOSENT WORK AT START, THEN CLOSE AND REOPEN THE HUB**
8. Open **Project Settings → Build Settings** and use **Build** or **Build & Run** to create a standalone Windows game.
9. YOUTUBE VIDEO LINK -- https://youtu.be/GHu1yg2hNl8?si=JQH7Iv7sQoOtcEY8
## Supported platform

- Windows 10/11, x64
- A Vulkan-capable GPU driver
- Keyboard and mouse

The installer bundles SDL runtime files with the engine. It installs the supported authoring dependencies needed for the native script workflow; it does not replace GPU drivers.

## Source build ( to view/edit the core engine files locally )

The release installer is the recommended evaluation path and is the main project(GameEngine) to download.. but if you want to edit the core engine files locally, you can download the code (*THE CODE MAY NOT WORK PROPERLY LOCALLY IF REQUIRED DEPENDENCIES OR COMMAND LINES ARE NOT RUNNED) . The source tree is provided for code review and contributors who want to build the engine locally.

Requirements:

- Visual Studio 2022 or Build Tools with MSVC x64 tools and CMake
- CMake 3.16 or newer
- Vulkan SDK and a Vulkan-capable driver

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target editor hub --parallel 1
```

The built executables are placed under `build\editor\Release\editor.exe` and `build\hub\Release\hub.exe`.

## Repository and release contents

The repository contains source, project assets, build definitions, documentation, and the installer pipeline. Generated `build/` folders, crash reports, exports, and release executables are intentionally excluded from Git as they are only generated after building the game engine locally and do not contain any core engine files. The production installer is published as a GitHub Release asset instead.

The repository is shared publicly for Devpost/OpenAI build week evaluation. The included license is GNU General Public License v3.0; third-party dependencies and assets retain their own notices and terms.

## How I used Codex and GPT-5.6

I used Codex with GPT‑5.6 throughout development to build the whole editor, c++ scripting base, standalone game feature, all the components,tools,panels investigate runtime and hot-reload issues, implement editor and installer workflows, build hub, improved visual scripting, improve project packaging and validate everything The Provided Feedback ID Contains the longest And most descriptive chat I have did for This Engine.

I directed the product scope, chose the features and priorities, tested the editor and game workflows, and made the final release decisions.

- **Codex Session ID:** `019f6664-fcff-7812-8dce-f3910c4b6615`
- **Development record:** [`docs/CODEX_BUILD_LOG.md`](docs/CODEX_BUILD_LOG.md)
- **Submission reference:** [`docs/DEVPOST_SUBMISSION_CHECKLIST.md`](docs/DEVPOST_SUBMISSION_CHECKLIST.md)

## License and notices

Copyright (C) 2026 Kinshuk12-PRO.

GameEngine2D Pro is licensed under the GNU General Public License v3.0. See [LICENSE](LICENSE).
Third-party components and assets remain subject to their respective licenses and notices; see [`installer/THIRD_PARTY_NOTICES.md`](installer/THIRD_PARTY_NOTICES.md) and the Abyss asset notices.
