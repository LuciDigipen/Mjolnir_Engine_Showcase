# Mjolnir Engine

A modular 3D engine. It has an archetype ECS, a Vulkan 1.3 renderer, an offline
asset pipeline, a Lua scripting layer and an ImGui editor shell, and every part
of it is its own static library.

A game is data, not a build target. Each game is a directory under `games/`
holding configuration, assets, Lua scripts and scenes. One shared runtime loads
that directory, so changing a game needs no rebuild.

## Folder structure

```
CMakeLists.txt        root build: module list, MjolnirEngine target, game discovery
CMakePresets.json     Debug / Release, the only supported configure path
vcpkg.json            dependency manifest
engine.json           defaultGame, loaded when --game is not given
main.cpp              MjolnirEngine entry point: loads one game directory

cmake/                build helpers
  EngineModule.cmake    engine_add_module, engine_add_module_tests
  EnginePlatform.cmake  ENGINE_PLATFORM and family flags
  Shaders.cmake         slangc compilation
  Assets.cmake          asset cooking
  Fonts.cmake           font copying
  Branding.cmake        icon and splash copying
  GameBuild.cmake       game build template install rules, game_build_info.json

modules/              one static library each, mir::<name>
  core/                 ECS, logging, paths, file IO, the surface contract
  window/               GLFW window behind an interface
  renderer/             Vulkan 1.3 renderer, render components, RenderSystem
  assets/               cooked blobs, manifest, cache
  scene/                scene save and load
  serial/               JSON documents on disk
  script/               Lua embedding and ECS bindings
  audio/                Wwise lifetime and the per-frame pump
  imguiruntime/         ImGui context lifetime, fonts, backends, frame bracket
  editor/               editor shell, drawn into imguiruntime's context
  editortheme/          the editor ImGui themes (Midnight, Slate), data only
  app/                  composition root and main loop
    include/<name>/       public API — the whole surface of the module
    src/<name>/           implementation and private headers
    tests/                Catch2 tests, one CTest case per TEST_CASE

games/                one directory per game — no C++
  HelloWorld/
    game.json             title, assetsRoot, sceneDir, relative to the game
    assets/               meshes and Lua scripts, cooked separately
    scenes/               scene.json plus one file per entity
  EmptyScene/           a game with an empty scene, for the smoke test
  SoundtrackCheck/      a streamed soundtrack and an SFX loop, with saved Lua volumes and mutes

tools/                AssetCooker, MjolnirBuild, new_module.ps1, new_file.ps1, wwise/ (SoundtrackCheck's banks)
templates/            what the scaffolding scripts stamp out
third_party/          imgui (submodule), imgui_config, IconFontCppHeaders
assets/               engine source assets and shaders, cooked into the build tree
fonts/, branding/     copied next to the executable
docs/                 build, architecture, conventions, extending, per module
builds/, logs/        gitignored
```

How the modules depend on each other, and why:
[docs/architecture.md](docs/architecture.md#module-graph).

## Read next

| Document | Answers |
|---|---|
| [docs/build.md](docs/build.md) | How do I build, run and debug it? |
| [docs/architecture.md](docs/architecture.md) | What exists, how does it fit, why this shape? |
| [docs/conventions.md](docs/conventions.md) | How do I write code that fits in? |
| [docs/extending.md](docs/extending.md) | How do I add a module, component, system, shader, asset? |
| [docs/modules/](docs/modules/) | How does one module work? |

## Requirements

Windows only. CMake >= 3.26, Ninja, MSVC (VS 2026 or Build Tools), Vulkan SDK
>= 1.4.309, a bootstrapped vcpkg with `VCPKG_ROOT` set, and VS Code with
`ms-vscode.cmake-tools` + `ms-vscode.cpptools`.

## Quick start

```
git clone --recurse-submodules <repo-url>
git submodule update --init --recursive
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --test-dir builds/windows-msvc-debug --output-on-failure
```

Run those from a Developer PowerShell, or from a plain one after
`vcvars64.bat`. In VS Code, select the `windows-msvc-debug` configure preset,
then `F7` to build and `F5` to debug.

This assumes vcpkg and the Vulkan SDK are already set up. Full setup,
troubleshooting and the VS Code workflow are in [docs/build.md](docs/build.md).

## Git LFS

The 4K TerrainCheck material sources for the Icelandic sand and rocks scan are
tracked with Git LFS:

```
games/TerrainCheck/assets/textures/icelandic_sand_and_rocks/*.jpg
```

Only that directory is in LFS; every other image in the repository stays a
regular Git object. The six committed maps are BaseColor, Normal,
Displacement, Roughness, Specular and AO. Bump, Cavity and Gloss are not used
and are not included. The six JPEGs are byte-identical copies of the originals;
nothing was re-encoded, cooked or migrated from history. `tcigfcgn.json` is the
vendor metadata manifest and stays a normal text file. It describes the full
source set, so it lists maps beyond the six committed here.

Install the Git LFS client once, then fetch the objects after cloning or
pulling:

```
git lfs install
git lfs pull
```

A JPEG that opens as small text beginning with `version https://git-lfs` is an
unresolved pointer. Run `git lfs install`, then `git lfs pull` from the
repository root, and check the result with `git lfs status` or `git lfs fsck`.
Do not edit or commit pointer text by hand.

### Source and licensing

The set was obtained as Fab purchased assets. This repository is PRIVATE and
the textures are for project collaborators only; they are not redistributed as
standalone files. The Fab EULA applies:
https://www.fab.com/eula?lang=en.

The organization's Git LFS quota could not be read because the account lacks
the `admin:org` scope, so available storage and bandwidth are unconfirmed. This
is not a verified quota pass. No LFS objects have been uploaded.

## Third-party notices

Dear ImGui is a `third_party/imgui` submodule, MIT licensed, with `LICENSE.txt`
inside it.

`fonts/Inter.ttf` is Inter by Rasmus Andersson, SIL OFL, `fonts/Inter-OFL.txt`.

Icon glyphs are Font Awesome Free, `fonts/fa-solid-900.ttf`, licensed in
`fonts/FontAwesome-LICENSE.txt` and addressed through
`third_party/IconFontCppHeaders/IconsFontAwesome6.h`, zlib.

Everything else comes from vcpkg, through `vcpkg.json`.
