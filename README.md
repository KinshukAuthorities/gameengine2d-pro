# GameEngine2D Pro

> A Windows-first 2D game-engine workspace built with Codex and GPT-5.6: a project Hub, Vulkan/SDL editor, native C++ hot reload, visual-scripting tools, standalone export, and the **Abyss of Hollows** playable showcase.

**OpenAI Build Week 2026 - Developer Tools**

GameEngine2D Pro is for creators who want a focused 2D workflow without having to assemble a launcher, scene editor, runtime, scripting pipeline, and project exporter from separate tools. It ships as an installable Windows application: create a project in the Hub, open it in the Editor, iterate on scenes and scripts, then build a standalone game from the same workspace.

## What is included

| Area | What it provides |
| --- | --- |
| **GameEngine Hub** | Project discovery, search, favorites, safe rename/duplicate/import, project templates, project settings, lock awareness, and Editor status/build feedback. |
| **Editor** | Vulkan-rendered 2D viewport, hierarchy, inspector, assets browser, scene/prefab workflows, tilemap tools, animator, console, project settings, and build settings. |
| **Scripting** | Per-script native C++ modules for fast hot reload, project locking during script/build work, direct VS Code opening for source files, and a visual-scripting graph workflow. |
| **Runtime and export** | SDL input/windowing, Vulkan renderer, scene loading, 2D physics, UI, prefab and tilemap support, plus standalone Windows export. |
| **Showcase template** | **Abyss of Hollows**, an original action-adventure sample used to exercise the full project-to-export path. |

## Why this project

Small 2D projects commonly lose momentum to setup friction: scattered project folders, editor restarts after a code change, incomplete export packaging, and unclear paths from prototype to runnable build. GameEngine2D Pro treats that workflow as one product:

1. **Start in the Hub** and create a project in the engine-managed `games/` location.
2. **Author in the Editor** with scenes, entities, components, assets, prefabs, tilemaps, native code, and visual graphs in one workspace.
3. **Iterate safely** with project locks and targeted per-script reloads rather than rebuilding the editor for every script change.
4. **Ship from the same tool** with a packaged standalone export and a Windows installer for the engine itself.

## Judge quick start

### Recommended: use the packaged installer

> **Evaluators:** use the [v1.0.0 GitHub Release](../../releases/latest) to run the product. **Code -> Download ZIP** is the reviewable source archive, not a portable engine installation; it intentionally excludes generated build output and the 526 MiB installer.

1. Download `GameEngine2DPro-Setup-1.0.0-x64.exe` from the repository's **v1.0.0 GitHub Release**.
2. Let the installer place the engine under `%LocalAppData%\GameEngine2D Pro` and install its required authoring prerequisites.
3. Start **GameEngine Hub** from the Start Menu.
4. Select **New project** and choose the **Abyss of Hollows** template, or open an existing project.
5. Open the project in the Editor. Press Play to run the active scene.

The release installer is Windows x64 only. A Vulkan-capable GPU driver is required; the installer bundles the Vulkan loader/runtime check but correctly does not replace graphics drivers.

### Verify the native script path

1. On first project open, the Editor automatically locks and synchronizes the active project's native scripts. The progress popup explains that a first synchronization can take a few minutes; no manual **Reload Scripts** click is required.
2. In the opened project, inspect `scripts/` in the Assets panel.
3. Edit and save one existing `.cpp` script, or create a new script in that folder. The workflow rebuilds the changed native script module rather than the Editor executable.
4. Return to Play mode and confirm the project runs.

The final packaged Abyss template was checked in an isolated staging root: all 24 included native script modules compiled successfully, every scene script reference had a registered class, and retired numbered scripts were excluded.

### Export a game

Open **Project Settings -> Build Settings**, select the scenes to include, choose the product metadata, then use **Build** or **Build & Run**. The resulting standalone game is written to the project export folder shown by the build dialog.

## Build from source

Use the installer for the fastest judge path. Source builds are available for engine contributors.

### Requirements

- Windows 10/11 x64
- Visual Studio 2022 / Build Tools with MSVC C++ x64 tools and CMake
- CMake 3.16 or newer
- Vulkan SDK and a Vulkan-capable driver
- Python only when downloading missing development dependencies

### Commands

```powershell
python scripts/fetch_deps.py
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target editor hub --parallel 1
.\build\hub\Release\hub.exe
```

If dependencies are already present, the first command is not needed. To create the production installer from a completed Release build:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_installer.ps1
```

See [`installer/README.md`](installer/README.md) for installer behavior, prerequisite handling, and packaging notes.

### Repository and release packaging

The repository tracks source, project assets, documentation, and release definitions. It deliberately excludes generated build folders, crash reports, temporary installation-smoke-test folders, standalone exports, and the 526 MB installer binary. After pushing the private repository, upload `GameEngine2DPro-Setup-1.0.0-x64.exe` as a **GitHub Release** asset named `v1.0.0`; invited Devpost/OpenAI reviewers can download it without rebuilding the engine.

## Project layout

```text
GameEngine2DPro/
+-- hub/              # GameEngine Hub source and visual assets
+-- editor/           # Editor application, UI panels, and script-module builder
+-- engine_cpp/       # Runtime, renderer, physics, scene, and export code
+-- games/game7/      # Abyss of Hollows source showcase project
+-- scripts/          # Build, dependency, and packaging automation
+-- installer/        # Inno Setup release definition and notices
+-- dist/             # Release installer output
`-- docs/             # Submission evidence and maintenance notes
```

## Codex and GPT-5.6

This project was developed in the Codex desktop workspace with **GPT-5.6**. Codex accelerated the implementation and review loop across the engine, Hub, installer, native-script build path, and the Abyss showcase. The human project owner directed product scope, selected priorities, reviewed the resulting behavior, and made the final submission decisions.

Key Codex-assisted work included:

- tracing and correcting stale generated script wrappers and per-project native script reload behavior;
- building and packaging the Hub, manifest-based project workflow, template handling, and project locking;
- improving authoring and export workflows across the Editor;
- creating the installer staging/verification pipeline and ensuring the template has the source headers needed for post-install C++ reloads;
- validating the final release template with a clean, isolated CMake/MSVC build.

For a concise, judge-readable evidence record, see [`docs/CODEX_BUILD_LOG.md`](docs/CODEX_BUILD_LOG.md).

### Required Devpost session evidence

- **Confirmed `/feedback` Codex Session ID:** `019f6664-fcff-7812-8dce-f3910c4b6615`
- Enter this exact value in Devpost's required **/feedback Session ID** field.

The repository intentionally keeps a concise engineering evidence log instead of exporting a raw conversation transcript, which could contain personal details or unrelated diagnostics.

## Third-party software and assets

The release uses Vulkan, SDL2, Dear ImGui, nlohmann/json, Microsoft development tooling, and LunarG/Khronos distribution components. Their respective licenses remain in force. See [`installer/THIRD_PARTY_NOTICES.md`](installer/THIRD_PARTY_NOTICES.md) and the Abyss asset provenance/notice files for details.

## License and submission access

GameEngine2D Pro's original source and original assets are proprietary and **all rights are reserved**. The repository is intended to remain private; Devpost, OpenAI, and authorized competition judges receive limited review-and-evaluation access only. See [`LICENSE`](LICENSE). Third-party dependencies and assets retain their own terms and notices.

## Devpost submission checklist

Before submitting to OpenAI Build Week, the project owner should:

- add a public repository URL, or share the private repository with `testing@devpost.com` and `build-week-event@openai.com`;
- if the repository is private, create a `v1.0.0` GitHub Release and attach `GameEngine2DPro-Setup-1.0.0-x64.exe` for the no-rebuild judge path;
- add a public YouTube demo video under three minutes, with narration explaining the project plus use of Codex and GPT-5.6;
- run `/feedback` in this conversation and enter its returned Session ID in Devpost;
- choose the **Developer Tools** category;
- attach the repository README and follow the Judge quick start above;
- confirm the Devpost submission is submitted, not left as a draft.

The field-level checklist is available in [`docs/DEVPOST_SUBMISSION_CHECKLIST.md`](docs/DEVPOST_SUBMISSION_CHECKLIST.md).
