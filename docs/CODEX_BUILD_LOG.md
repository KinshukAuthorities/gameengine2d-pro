# Development and Release Record

This is a concise record of the work and release checks behind my GameEngine2D Pro OpenAI Build Week submission. I am keeping it separate from the raw conversation history so the repository stays focused on the project and does not expose unrelated diagnostics or personal information.

## Session reference

- **Codex `/feedback` Session ID:** `019f6664-fcff-7812-8dce-f3910c4b6615`
- **Core development thread:** `Fix hot reload rebuild`
- **Workspace:** `C:\Users\lenovo\OneDrive\Desktop\game_engine`
- **Workflow:** Codex desktop with GPT-5.6

## Work completed with Codex and GPT-5.6

I used Codex and GPT-5.6 as implementation and verification tools while I directed the product, set priorities, tested changes, and approved the final release.

### Engine and authoring work

- Diagnosed stale generated C++ wrapper references after scripts were deleted.
- Reworked the project-specific native-script path so scripts compile as independent reloadable modules instead of requiring an Editor rebuild.
- Added project-lock and build/reload safeguards.
- Improved project discovery, manifests, Hub project actions, template copying, and Editor launch behavior.
- Improved authoring workflows across the asset browser, inspector components, prefabs, tilemaps, visual scripting, export flow, and close-with-unsaved-work handling.

### Packaging work

- Built a Windows installer around Hub, Editor, runtime files, assets, shaders, templates, C++ authoring sources/headers, and required prerequisites.
- Configured the installed engine under `%LocalAppData%\GameEngine2D Pro` so Hub-created projects stay writable under `games\`.
- Kept **Abyss of Hollows** as the only bundled Hub sample template.
- Added staging checks for development residue, stale templates, and retired script files.

## Template validation

I copied the packaged `abyss-of-hollows` template into an isolated temporary engine root and checked it with the same native C++ module path used after installation.

| Check | Result |
| --- | --- |
| Shipped sample template | `abyss-of-hollows` only |
| Native C++ modules configured | 24 |
| Native C++ modules compiled | 24 / 24 |
| Script classes registered | 24 |
| Scene references without registration | 0 |
| Retired `NewScript*.cpp` files shipped | 0 |
| Required installed authoring files | Present |

The package review caught missing Editor source and Dear ImGui headers needed by the installed native-script build. I added them to the staged payload, rebuilt the installer, and repeated the clean-template validation.

## Release artifact

- **File:** `GameEngine2DPro-Setup-1.0.0-x64.exe`
- **Version:** `1.0.0`
- **Size:** 552,130,649 bytes
- **SHA-256:** `E7AED7F539BA626850703CCFAC48B29414C4628DDCD6DF160FC61DCE8D6AA413`

The executable is distributed as a GitHub Release asset rather than committed to Git.
