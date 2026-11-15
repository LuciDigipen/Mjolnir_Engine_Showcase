# Build

Windows only. Linux sources exist for the window and surface backends, but there
is no Linux preset.

## Prerequisites

| Tool | Version | Notes |
|---|---|---|
| CMake | >= 3.26 | |
| Ninja | recent | Ships with the VS "C++ CMake tools" component |
| MSVC | VS 2026 or Build Tools | C++20 |
| Vulkan SDK | >= 1.4.309 | Also supplies `slangc`, required for shaders |
| vcpkg | any bootstrapped clone | Not vendored |
| VS Code | - | `ms-vscode.cmake-tools`, `ms-vscode.cpptools` |

## First-time setup

Steps 1–3 are once per machine; step 4 is once per clone.

### 1. vcpkg

```
git clone https://github.com/microsoft/vcpkg C:/dev/vcpkg
C:/dev/vcpkg/bootstrap-vcpkg.bat
setx VCPKG_ROOT C:/dev/vcpkg
```

Then **close every terminal and quit VS Code entirely**.

Do not vendor a second copy into this repo. One clone per machine means one
shared binary cache in `%LOCALAPPDATA%\vcpkg`, so only your first configure
anywhere pays the build cost.

### 2. Vulkan SDK

Install it from [vulkan.lunarg.com](https://vulkan.lunarg.com/sdk/home), version
1.4.309 or newer. The installer will set `VULKAN_SDK` env_path, and the
build reads `slangc` out of `$VULKAN_SDK/Bin`.

Restart terminals and VS Code again if they were open during the install.

### 3. Setting name in headers (optional)

```
setx MJOLNIR_AUTHOR "Your Name"
setx MJOLNIR_ORG "DigiPen (USA) Corporation"
```

Skip it and the scaffolding falls back to `git config user.name`, then the OS
user.

### 4. Clone and build

```
git clone --recurse-submodules <repo-url> C:/dev/MitosisEngine
cd C:/dev/MitosisEngine
cmake --preset windows-msvc-debug
```

From a Developer PowerShell, or a plain one after running `vcvars64.bat`.

This first configure installs every dependency and takes a few minutes. Later
ones are fast. If you already cloned without `--recurse-submodules`, run
`git submodule update --init --recursive` before configuring.

Then build and check it: [Build loop](#build-loop) and [Verify](#verify).

## RTXGI DDGI SDK

GI support is enabled by default:

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
```

The default is `ON`. Configuring fetches RTXGI-DDGI SDK 1.3.6 at commit
`f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6`, without samples or submodules.
It requires DXC with SPIR-V support from the Vulkan SDK or PATH; override its
location with `-DRTXGI_DXC_EXECUTABLE=<path>`. To build without GI support, pass
`-DMIR_WITH_RTXGI=OFF`. Existing build caches keep their previous setting until
you change it explicitly. Review the SDK's `License.txt`
before redistributing it.

The engine owns probe resources and copies the SDK compute shaders to
`shaders/rtxgi` beside the executable. Enable ray tracing and GI in Project
Settings to use them. DLSS is configured independently. See the
[renderer reference](modules/renderer.md) for settings and runtime behavior.

## Build loop

Every build is the same three steps.

1. Choose a preset. Skippable when you are reusing the previous selection, which
   is the usual case.
2. Configure.
3. Build.

`CMakePresets.json` is the only supported way to configure. There are two
presets for everyday work, `windows-msvc-debug` and `windows-msvc-release`;
two that build the game without the editor, `windows-msvc-game-debug` and
`windows-msvc-game-release` (see [A build without the editor](#a-build-without-the-editor));
and `windows-msvc-debug-float`, with float world positions (see World precision
below).
All use Ninja and all build into `builds/<presetName>/`.

The command line and VS Code read the same presets, so everyone builds the same
way.

VS Code:

1. `Ctrl+Shift+P` → **CMake: Select Configure Preset** → `Debug` / `Release`
2. `F7` to build
3. `Ctrl+Shift+P` → **CMake: Set Launch/Debug Target** → `MjolnirEngine`
4. `Ctrl+F5` to run, `F5` to debug

After the first time, `F7` then `F5`.

With no arguments, `MjolnirEngine` loads the game named by `defaultGame` in
`engine.json`. To run another one, pass `--game games/<name>`, for example
through `args` in `.vscode/launch.json`.

Terminal, from a Developer PowerShell (or after `vcvars64.bat`):

```
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --test-dir builds/windows-msvc-debug --output-on-failure
```

Output goes to `builds/<presetName>/`, which is gitignored. Build into that tree
rather than making a second one.

Dependencies are declared in `vcpkg.json` and installed during configure. They
resolve against a pinned `builtin-baseline`, so everyone gets the same versions.

Jolt is the one package that does not come from the registry as is:
`vcpkg-overlays/joltphysics`, registered by `vcpkg-configuration.json`, replaces
the port to add a `double-precision` feature (`JPH_DOUBLE_PRECISION`), which
the upstream port lacks. vcpkg builds it from source on the first configure
after it changes. A build directory whose cache has `VCPKG_MANIFEST_INSTALL=OFF`
needs one configure with `-DVCPKG_MANIFEST_INSTALL=ON` to pick it up.

**World precision.** The CMake option `MIR_LARGE_WORLD` (default ON) stores
world positions as double and builds Jolt with double precision. OFF stores
them as float: smaller and faster, but only for small maps. Before `project()`,
the root `CMakeLists.txt` turns ON into the `large-world` vcpkg feature, so the
C++ setting and the Jolt binary always agree; `modules/physics/CMakeLists.txt`
stops the configure if they do not. The `windows-msvc-debug-float` preset
builds with it OFF, into its own `builds/windows-msvc-debug-float/`. Tests that
need double positions far from the origin report Skipped there. After changing
code that handles positions, build and test both presets.

**Precompiled headers.** Every compiled engine module privately precompiles stable
standard-library headers, with selected GLM, ImGui and JSON headers added by the
modules that use them. Each module builds its own PCH with its compiler settings.
C sources, tests and sources with separate compiler settings keep their normal
include order. To disable precompiled headers for an include-dependency check:

```powershell
cmake --preset windows-msvc-debug -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON
cmake --build --preset windows-msvc-debug
```

Configure with `-DCMAKE_DISABLE_PRECOMPILE_HEADERS=OFF` to enable them again.
Keep normal includes in source files so both configurations build independently.

**Offline asset cooking.** MSVC Debug builds optimize `AssetCooker` and its
`gltf_importer` library, including image decoding and mip generation. They retain
Debug symbols and the Debug runtime, engine modules and test sources retain their
usual Debug settings. To step through unoptimized cooker code, configure with
`-DMIR_OPTIMIZE_ASSET_COOKER=OFF` and rebuild the tool. Release builds keep their
normal settings.

**Reconfigure** every time a `CMakeLists.txt` or `vcpkg.json` changes.

Ninja does it for you at build time, and cmake-tools can do it on save — see
`cmake.configureOnEdit`. To force one: `Ctrl+Shift+P` → **CMake: Configure**.

## A build without the editor

`windows-msvc-game-debug` and `windows-msvc-game-release` configure with
`MJOLNIR_WITH_EDITOR=OFF`. `MjolnirEngine` is then the game alone: the
`editor` and `editortheme` modules and ImGuizmo are not built, nothing links
`engine_editor`, and the editor's asset import queue is left out of `app`. The
game opens in its own window, not maximized and with the system title bar, and
plays from the first frame, with the game UI fullscreen.

```
cmake --preset windows-msvc-game-debug
cmake --build --preset windows-msvc-game-debug
ctest --test-dir builds/windows-msvc-game-debug --output-on-failure
```

It is a second build tree, the one exception to building into the tree you
already have, since the option changes which modules exist. The same tests and
smoke tests run in it. `--no-editor` is accepted and changes nothing there.

A module that starts linking `mir::editor` unconditionally fails this
configure, because the target does not exist, so the separation cannot erode
without someone noticing.

## A game build template

A game preset's build folder holds every game, their cooked assets and the build's own
leftovers. `cmake --install` copies out only what the game build itself needs, so a
build tool can add one game to it:

```
cmake --build --preset windows-msvc-game-release
cmake --install builds/windows-msvc-game-release --prefix builds/game-build/release
```

The template holds `MjolnirEngine.exe`, `MjolnirErrorReporter.exe`, the DLLs
they import, the current shader generation (plus `shaders/rtxgi/` with RTXGI),
`fonts/`, `branding/`, `game_build_info.json`, and `nvngx_dlssd.dll` with DLSS. It
has no `games/`, `cooked/` or `engine.json`, which belong to a game. The debug
tree installs the same way, with the debug DLLs. Only the game presets have
install rules.

Install into an empty folder: `cmake --install` never removes what an earlier
install left there.

Build in the editor and MjolnirBuild's `--game-build-preset` do all of this
for you (see [Compiling the game build](#compiling-the-game-build)), so these
commands are only for a template made by hand.

DLLs are found from the executables' imports rather than
`$<TARGET_RUNTIME_DLLS>`, because LuaJIT is linked through pkg-config and its
target names no DLL. Windows system DLLs and the MSVC runtime are left out, so a
machine without Visual Studio needs the Visual C++ Redistributable.

Every build writes `game_build_info.json` next to `MjolnirEngine`, the editor's
included: the engine version, commit, configuration, and whether the editor,
large world, audio, DLSS and RTXGI were built in. Comparing the editor's copy
with a template's tells whether the two came from the same engine.

## Packaging a game

`MjolnirBuild` puts one game and a game build template together into a folder of
its own:

```
cmake --build --preset windows-msvc-debug --target MjolnirBuild
builds/windows-msvc-debug/tools/MjolnirBuild/MjolnirBuild.exe --game games/HelloWorld --game-build builds/game-build/release --out builds/game-build-out/HelloWorld --result builds/game-build-out/HelloWorld.json
```

It copies the template, then cooks the engine's `assets/` and the game's
`assetsRoot` into `cooked/` under the same id roots the CMake cook uses, so the
ids match the editor's. From the source tree, never a build folder's copy, it
copies what the game reads raw: `game.json`, `project_settings.json`,
`terrain_streaming.json`, the `sceneDir` scene, the assets folder without
`prefabs/`, and the SoundBanks. The copied `game.json` is unchanged, so its
`sceneDir` and `soundBanksRoot` must be relative paths inside the game folder.
Each `*.terrain.json` gets the release that
`TerrainTool package` finds current, which the last engine build published, so
build the engine after editing a recipe. A game with no SoundBanks or terrain
skips those steps.

The output starts its game with no arguments, so a double-click plays it:

- `MjolnirEngine.exe` is renamed to `game.json`'s `name`, e.g. `HelloWorld.exe`.
  A name Windows cannot use as a file name, or one that would replace another
  template file, is refused. The renamed exe still finds
  `MjolnirErrorReporter.exe` and logs to `logs/` beside it.
- `engine.json` names the game as `defaultGame`, and the window takes
  `game.json`'s `title`.
- The copied `project_settings.json` leaves out the sections that describe one
  machine, such as `ide` with the IDE's install path. They are listed in
  `kMachineSettingsSections` in `modules/project/include/project/ProjectSettings.h`;
  add a new machine-specific section there. The source file is never changed.

The template's `game_build_info.json` is compared with the one beside the
`MjolnirEngine` that MjolnirBuild was built with, which for an editor build is
the editor's. A different commit or engine version, or a different large
world, audio, DLSS or RTXGI option, is a warning, as is a game with SoundBanks
packaged with a template built without audio.

The build happens in `<out>.tmp` and replaces `<out>` only when every step
succeeds, so a failed build leaves the previous one where it was; the previous
build waits in `<out>.old` during the swap. `<out>`, `<out>.tmp` and
`<out>.old` must lie outside the checkout or under its `builds/`, outside any
CMake build folder and the template. `<out>` must be missing, empty, or a
previous build, which holds `mjolnir_build.json`, and `<out>.tmp` and
`<out>.old` must be missing or previous builds. A folder so deep that a file in
it would pass 259 characters is refused too: the engine cannot open such a path.
A program running from `<out>`, `<out>.tmp` or `<out>.old`, such as the game
from the last build, holds files the swap must move, so `check` refuses the
build in seconds, naming it: `HelloWorld.exe is running from <out> (process
1234), which keeps the previous build locked; close it and build again`. With
`--game-build-preset`, the template and its `.tmp` and `.old` are checked the
same way.

`--result` gets a JSON report, a failed build's included: the status, each
step's status and time, warnings, the error (or, once built, the exe's path as
`executable`), the asset count and the size. The exit code is 0 when built, 1
when the build failed, 2 for bad arguments. The editor finds the tool through
`buildTool` in `asset_workspace.json`.

`--progress <file>` is rewritten as each step starts, and in the `game build`
step as each phase starts and as Ninja moves through the compile, read from
the log every 250 ms:

```json
{"version":1,"step":"game build","phase":"compile","done":412,"total":1310}
```

`phase` (`environment`, `configure`, `compile` or `install`) is only there in
the `game build` step, and `done` and `total` only once Ninja has printed its
`[done/total]`. `--result` and `--progress` must be different files outside
`<out>`, `<out>.tmp` and `<out>.old`, and their folders are made if missing. A
reader holding the progress file open delays the next write for a moment;
MjolnirBuild retries for about 200 ms. Either way the terminal gets a line as
each step and phase starts, such as `MjolnirBuild: game build: compile...`,
and a table of every step's time at the end.

A game preset's CTest packages every game the same way. `build.game_build` installs
the build as a template, `build.package.<game>` runs MjolnirBuild into
`builds/smoke-build/<build folder>/<game>`, and `smoke.build.<game>` starts the
exe with no `--game`, as a double-click does. All are labelled `smoke`:

```
ctest --test-dir builds/windows-msvc-game-release -R "^smoke\.build\." --output-on-failure
```

### Compiling the game build

With `--game-build-preset <preset>`, a `game build` step between `check` and
`template` compiles the game build from the checkout and installs it as the
template first, so no template has to be made by hand:

```
builds/windows-msvc-debug/tools/MjolnirBuild/MjolnirBuild.exe --game games/HelloWorld --game-build builds/game-build/release --game-build-preset windows-msvc-game-release --out builds/game-build-out/HelloWorld
```

It runs from any shell, or from the editor started from Explorer: the step
loads Visual Studio itself. It writes `mjolnir_game_build.bat` into the
preset's build folder (its `binaryDir`, e.g. `builds/<preset>`) and runs it
with `cmd /d /c`. The script:

1. Loads Visual Studio with `call vcvars64.bat`, finding it in this order:
   - nothing, when this is already a Developer PowerShell or Developer Command
     Prompt (`VCToolsInstallDir` is set);
   - the `CMAKE_CXX_COMPILER` in the build folder's `CMakeCache.txt`, with
     `-vcvars_ver=<toolset>`, so a build folder keeps the Visual Studio and
     toolset it was configured with, even when a newer one is installed;
   - the same entry in the editor's own build folder, for a first compile;
   - the newest install with the C++ tools, whether a release, a preview or the
     Build Tools, from `vswhere -latest -prerelease -products *`.

   It then checks that `cl`, `cmake` and `ninja` are on PATH. Without any
   Visual Studio, `check` fails asking for the "Desktop development with C++"
   workload; without `cmake` or `ninja`, the compile fails naming "C++ CMake
   tools for Windows".
2. Configures with `cmake --preset <preset>`, passing `MIR_LARGE_WORLD`,
   `MIR_WITH_AUDIO`, `MIR_WITH_DLSS` and `MIR_WITH_RTXGI` as the game chose
   them (see [Build options](#build-options) below), and the rest as the
   editor's `game_build_info.json` records them. It configures on every
   compile, so a preset whose settings changed, such as `CMAKE_BUILD_TYPE`,
   or an option that changed, is never packaged with the old ones; with
   nothing changed, configure is quick and Ninja has nothing to do.
3. Compiles only `MjolnirEngine` and `MjolnirErrorReporter`, with
   `cmake --build --preset <preset> --target ...`, not the tests and tools
   the preset also builds.
4. Installs with `cmake --install <build folder> --prefix <template>.tmp`.
   That folder replaces `<template>` only after every phase succeeds, so a
   failed compile keeps the previous template and the previous build. The
   previous template is moved to `<template>.old` first and deleted only once
   the new one is in place, so a locked file cannot leave it half removed. The
   install is fresh every time, because `cmake --install` only adds files and
   would keep stale shader generations.

The script stays in the build folder, so the same commands can be run by
hand, and its output goes to `mjolnir_game_build.log` beside it, replaced
on each compile. The script echoes `mjolnir_game_build: <phase>` into it as
each phase starts. A failure names the phase, quotes the first three distinct
error lines in the log (`error C…`, `error LNK…`, vcvars' `[ERROR:…]`, or
`CMake Error` with the line after it, each cut at 300 characters) and gives
the log's path:

```
compiling the game build with windows-msvc-game-release failed:
  C:\alpha\modules\render\src\Renderer.cpp(88): error C2065: 'frameIndex': undeclared identifier
The whole output is in C:\alpha\builds\windows-msvc-game-release\mjolnir_game_build.log
```

From a plain shell with no build folder, a first compile of
`windows-msvc-game-release` took 199 s on an Intel Core Ultra 9 185H, with
vcpkg restoring its packages from the binary cache. A compile with nothing
changed took 13 s, most of it configure.

`check` makes sure of everything it can before the compile, so a bad folder
fails in seconds: the game and output checks, a non-hidden configure preset
and a build preset of that name in `CMakePresets.json`, `CMakeUserPresets.json`
or a file they include, a `binaryDir` it sets or inherits using only
`${sourceDir}`, `${presetName}` and the other plain macros (not `$env{}`), and a template folder that can be replaced. That is a
missing or empty folder, or a game build template (a `game_build_info.json` with
`editor: false`); a CMake build folder, a packaged game or other files are
refused and left alone. The build folder, the template and the output must
not overlap, and neither may their `.tmp` and `.old` folders, and the build
folder must not be the folder this MjolnirBuild was built in. The template checks (the exe, name conflicts, path length, the
comparison with the game's options and the editor) run on what was installed, in the `template` step.

Without `--game-build-preset`, `game build` is skipped and the template is used as it
is.

#### Build options

A game chooses its game build's options in its `project_settings.json`, so the
editor, the command line and CI build the same thing:

```json
"build": { "profile": "release", "gameBuildOptions": { "dlss": false, "audio": true } }
```

The keys are those of `game_build_info.json` (`largeWorld`, `audio`, `dlss`,
`rtxgi`; `kGameBuildFeatures` in `modules/project/include/project/GameBuildInfo.h`),
each `true` or `false`. An option the game does not choose follows the
editor's `game_build_info.json`, as before. With no editor info either, the
preset decides: configure gets `-U<option>`, so the build folder's cache,
which every game of the profile shares, drops what an earlier build set and
the option's default in `CMakeLists.txt` applies. An option the preset sets in
`cacheVariables`, itself or through `inherits`, gets nothing, since CMake
applies the preset's value on every configure and `-U` would remove it. The
choices hold no paths or machine settings. An entry that
is not one of those keys, or not `true` or `false`, is left out and named in a
warning; it never stops the build.

The template is compared with the choices first: a template built as the game
chose is not warned about where the editor differs, and one built otherwise
is, such as a ready-made template with DLSS ON for a game that chose it OFF.
The warning about SoundBanks in a game whose template has audio OFF stays,
chosen or not.

Turning on an option the editor was built without may need what this machine
lacks: the Wwise SDK (`WWISESDK`) for audio, a download of the NGX SDK for
DLSS, or the RTXGI SDK and DXC from the Vulkan SDK for RTXGI. Without it,
configure fails and the build's error quotes CMake's message. Changing large
world reinstalls Jolt through vcpkg. Every game shares the profile's build
folder and template, so building games with different options one after the
other recompiles what the options touch each time.

### Build Settings in the editor

File > Build Settings... holds what a build from the editor uses:

- **Profile.** Development packages the game-debug build, Release the
  game-release one. It is shared through git, as
  `"build": { "profile": "development" }` in the game's
  `project_settings.json`; a missing or unknown profile is Release.
- **Build options**, also shared through git: a checkbox for each of
  [the game build's options](#build-options), saved per game in
  `build.gameBuildOptions` when one is clicked. An option the game has not
  chosen shows "editor default"; one it has chosen shows the editor's own
  setting where it differs, such as "the editor has DLSS ON", and a **Reset**
  button that makes it follow the editor again. Each tooltip says what the
  option builds in and what turning it on needs. An entry the panel cannot
  use, and Audio OFF for a game with SoundBanks, are shown as warnings. There
  is no free-text field for CMake or launch arguments.
- **Game build template**, one per profile, and the **output folder**, one per game.
  Paths differ per machine, so they live in the editor preferences
  (`%LOCALAPPDATA%/Mjolnir/editor/editor_preferences.json`) and never in
  `project_settings.json`. Left empty, the template is
  `builds/game-build/debug` or `builds/game-build/release` and the output is
  `builds/game-build-out/<game>`; a relative path is from the checkout.
- **Compile the game build**, on by default, and **Run after build**, also per
  machine (`compileGameBuild` and `runAfterBuild` in the preferences' `build`).

With **Compile the game build** on, Build passes MjolnirBuild
`--game-build-preset` for the profile (`windows-msvc-game-debug` or
`windows-msvc-game-release`), so it compiles the game build from the checkout
and installs it into the template folder before packaging; see
[Compiling the game build](#compiling-the-game-build). It works with the editor
started from Explorer, and never touches the editor's own build folder. A
missing template never disables Build: the panel says "Not compiled yet; the
first Build compiles it", or shows the template's engine version and options,
and which options the compile will change.

With it off, Build uses the template folder as it is, and the build options
only decide what it is checked against. The panel shows its
`game_build_info.json` and whether it matches the game's options and the
editor, with the same checks MjolnirBuild warns about
(`modules/project/include/project/GameBuildInfo.h`), plus a template built for
the other profile, and a missing or broken one disables Build. The panel never shows commands to run by hand; to make a
template without the editor, see [A game build template](#a-game-build-template).

**Build**, at the bottom of the window, runs MjolnirBuild (the `buildTool` in
`asset_workspace.json`) with the template and output above. It runs in the
background: the editor stays usable, and the window shows the time so far, a
Cancel button and the step MjolnirBuild is on, read from its `--progress` file
four times a second. While the game build compiles that is Ninja's count, such
as `Compiling the game build: 412 of 1310`, with a progress bar. The result
replaces them when the build ends. The result is
the exe that starts the game, or MjolnirBuild's error, then its warnings and a
Log of the steps and their times. The same lines go to the Output log. With
**Run after build** on, a successful Build also starts the game.

The File menu starts the same build without opening the window:

- **Build** packages the game, and starts it when Run after build is on.
- **Build and Run** (Ctrl+B) packages the game and always starts it.
- **Build Settings...** (Ctrl+Shift+B) opens the window.

The shortcuts work in Edit mode only, so a game in Play keeps its keys. A build
that cannot start, because one is already running or for one of the reasons
below, says why in a note at the bottom right instead. The game starts as a
double-click starts it: with no arguments, in its own folder, and with
`LaunchProcess(..., false)`, not a `ChildProcess`, so it keeps running when the
editor closes. Where the editor itself runs in a job object that would end its
processes with it, the game is started outside it if the job allows that.

A toast at the bottom right shows a build the Build Settings window is not
showing: the time, the step and Ninja's count, with Cancel. When it ends, the
toast says how: "Built HelloWorld in 12.3 s" with **Show in Explorer**, which
opens the output folder with the exe selected, or "Built and started" after
Build and Run. A build with warnings, a failure, or a game that could not start
also offers Build Settings for the whole message and Log. Successes fade after
8 s; a failure, or a game that could not start, stays until it is closed.
Build Settings' result keeps it either way: it says when the game was started,
or why it could not be, next to the warnings.

The build reads the game from disk, so:

- **Unsaved changes** to the scene or any Prefab ask Save and Build or Cancel.
  Save writes every unsaved Prefab and the scene, and the build starts the frame
  after, once the scene is written. If the save fails, nothing is built.
  `--non-interactive` answers Cancel. The question is asked in Build Settings,
  so while it waits the window is opened, expanded and brought in front of any
  tab docked with it; a build from the File menu or Ctrl+B shows it too. Save
  and Build keeps whether it was Build and Run.
- **The game still running** from the last build fails the build in `check`,
  saying which program to close; see [Packaging a game](#packaging-a-game).
- **Play, a running asset import, or an unusable template** with Compile the
  game build off disable Build; the reason shows beside the button and in its
  tooltip.
- **Cancel** stops MjolnirBuild and everything it runs: the AssetCooker,
  TerrainTool, or the compile's cmd, CMake, Ninja and every `cl.exe`. The
  previous template stays as it was, since the new one replaces it only after
  the compile, and is put back from `<template>.old` if the stop came while
  MjolnirBuild was swapping it; if it cannot be put back, the result is a
  failure that says where it is, before the next compile deletes it. The next
  compile also removes a half-made `<template>.tmp`. The
  previous build stays where it was, put back if the stop came while
  MjolnirBuild was swapping it out; if it cannot be put back, the result is a
  failure that says so. A Cancel that comes after the new build moved in, but
  before MjolnirBuild finished, says it was too late: the new build is in
  `<out>`. A half-made `<out>.tmp` stays until the next build removes it. A
  Cancel, or closing the editor, after MjolnirBuild finished but before the
  editor saw it keeps the finished build, but Build and Run does not start it.

## Builds without proprietary SDKs and daily CI

`MIR_WITH_AUDIO` defaults to `ON`. Local builds with audio require the Wwise
SDK (`WWISE_SDK_ROOT`, or the launcher's `WWISESDK` environment variable).
Set it to `OFF` to omit `audio`, `audio_script`, their tests, and the app's
audio registration and Lua bindings. Games run silently; saved audio components
are skipped as unknown components. Scripts that call audio bindings should
check that those methods exist first, as the HelloWorld audio demo does.
`MIR_WITH_DLSS` defaults to `OFF`, which avoids fetching or linking NVIDIA NGX.

The `ci-windows-editor-debug` and `ci-windows-game-release` presets explicitly
disable both options and enable tests. Each has matching configure, build and
test presets, so no Wwise or NVIDIA SDK is needed:

```powershell
cmake --preset ci-windows-editor-debug
cmake --build --preset ci-windows-editor-debug
ctest --preset ci-windows-editor-debug

cmake --preset ci-windows-game-release
cmake --build --preset ci-windows-game-release
ctest --preset ci-windows-game-release
```

The test presets enable output on failure and exclude the `smoke` label:
GitHub-hosted runners do not have the GPU/display required by the engine smoke
tests. To check startup and gameplay without audio on a development machine,
run `ctest --test-dir builds/ci-windows-editor-debug -L smoke --output-on-failure`
(and likewise for the game Release tree).

[Daily Windows build](../.github/workflows/daily-build.yml) checks out `master`
with recursive submodules every day at 09:23 UTC and on manual dispatch. It
uses the explicit `windows-2025-vs2026` image, verifies VS 2026 C++ tools,
bootstraps the runner's vcpkg and installs Vulkan SDK 1.4.357.0 for `slangc`.
vcpkg downloads and binary archives are cached between runs, keyed by preset
and dependency definitions. Build options, parallelism and test filters live
in `CMakePresets.json`.

To start it manually, open **Actions > Daily Windows build > Run workflow**
and select `master`. Scheduled workflows require this file on the repository's
default branch, which must be `master`. Failed runs appear in Actions, upload
CTest diagnostics when available, and open or comment on a shared failure
issue to notify repository subscribers. Close that issue after resolving the
failure. The notification job requires GitHub Actions issue write permission.

## Tests

### Run every test

Build first. Tests are built by the ordinary build, so `F7` or
`cmake --build` is all the preparation there is.

Terminal:

```
cmake --build --preset {selected_preset}
ctest --test-dir builds/{selected_preset} --output-on-failure
```

VS Code: `Ctrl+Shift+P` → **CMake: Run Tests**, or `F7` then the flask icon in
the sidebar to get the Testing panel.

The Testing panel is empty until the first build, because test cases are
discovered after the executables link. Build once and they appear.

Run the full `ctest` before pushing.

### Run one module, or one test

```
cmake --build builds/windows-msvc-debug --target engine_core_tests
ctest --test-dir builds/windows-msvc-debug -R "archetype" --output-on-failure
```

`--target` picks the executable, and the targets are named
`engine_<module>_tests`. `-R` then filters by Catch2 test case name, and takes a
regex, so `-R "archetype"` runs every case with that word in its name.

### Equal system orders

Each test executable also runs whole as `<target>.ties-reverse` and
`<target>.ties-shuffle`, with `MIR_SYSTEM_TIES` reordering systems that share a
`SystemOrder` (see [core.md](modules/core.md)). A suite that never runs a
scheduler passes `NO_SYSTEM_TIES` to `engine_add_module_tests` and skips them, as
`core_base` and `terrain` do. They run one at a time and carry the label
`system-ties`, so `-L system-ties` runs only them and `-LE system-ties` skips
them. A shuffled run prints its seed; to repeat it:

```powershell
$env:MIR_SYSTEM_TIES = "shuffle:<seed>"
builds/windows-msvc-debug/engine_<module>_tests.exe
```

In the Testing panel the same thing is a click on one case or one file.

### Smoke tests

Every game with a `game.json` gets a `smoke.<game>` test. It runs
`MjolnirEngine --game games/<game> --hidden --frames 120`, which exits after
120 frames and exits non-zero if anything was logged as an error.

`smoke.switch` restarts from HelloWorld into EmptyScene and back — what
Project > Open Project does, minus the dialog. `--switch-to <dir>` asks for the
restart halfway through `--frames`. The next process gets the other half and the
remaining `--switch-to` entries, and in a `--frames` run each process waits for
the next one and exits with its code.

They create a hidden native window, so they need a GPU and a display. `--hidden`
requires `--frames` and suppresses both splash and game/editor window, normal
launches remain visible. `-L smoke` runs only
them, `-LE smoke` skips them:

```
ctest --test-dir builds/windows-msvc-debug -L smoke --output-on-failure
ctest --test-dir builds/windows-msvc-debug -LE smoke --output-on-failure
```

### How it is wired

Catch2 v3, one test executable per module, built under the `BUILD_TESTING`
option. That option is `ON` by default, which is why a plain build produces them.

`engine_add_module_tests` calls `catch_discover_tests`, so every `TEST_CASE`
becomes its own CTest case and a failure names the behaviour that broke.

`core`, `assets`, `input`, `scene`, `script` and `window` have tests. `renderer`
and `editor` need a device and a display, and are verified by running the engine,
which the smoke tests do once per game. The root CMakeLists registers those with
plain `add_test`, one per `games/*/game.json`.

There is no aggregate `tests` target. To build the test executables without the
games, list them:

```
cmake --build builds/windows-msvc-debug --target engine_core_tests engine_assets_tests engine_input_tests engine_scene_tests engine_script_tests engine_window_tests
```

To drop tests from the build entirely:

```
cmake --preset windows-msvc-debug -DBUILD_TESTING=OFF
```

## Making New Files

We have scripts for creating new files and modules.

Both are VS Code tasks: `Ctrl+Shift+P` → **Tasks: Run Task** → **New Module** or
**New File**. Both are PowerShell, so the convenience is Windows-only.

More info: [extending.md](extending.md).

## Verify

`MjolnirEngine` opens HelloWorld and renders the scene, `ctest` is all green, and
`logs/engine.log` next to the executable has trace output.

## Troubleshooting

| Symptom | Cause |
|---|---|
| Configure fails, toolchain file not found | `VCPKG_ROOT` unset or wrong. `setx` only affects new processes -> restart the terminal and all of VS Code. |
| `slangc not found` | No Vulkan SDK, or `VULKAN_SDK` unset. |
| Configure fails right after **New Module** | A module with no sources is an empty library. Add a file. |
| A new `.cpp` never links | Nothing is globbed. List it in its module's `target_sources`. |
| A new asset file is not cooked | A new *file* needs a reconfigure to join the glob; editing an existing one does not. |
| IntelliSense misses a new dependency | **CMake: Delete Cache and Reconfigure**. |
| A header gets no IntelliSense at all | `C_Cpp.intelliSenseEngineFallback` is off, so a header no compiled `.cpp` reaches gets nothing. Include it from a test. |
| `cl.exe` not found in a plain terminal | MSVC environment not loaded. Use a Developer PowerShell or prefix with `vcvars64.bat`. |
| The engine exits with code 10-14 | A fatal startup error. `logs/last-error.json` next to the executable holds its cause and fix; the codes are in [conventions.md](conventions.md#exit-codes-and-the-error-report). |
| A fatal error shows no box | `MjolnirErrorReporter` is missing next to the engine. Rebuild; the log and exit code still carry the error. |

## Changed

*2026-10-01* — **World precision is a compile-time option.** `MIR_LARGE_WORLD`
picks double or float world positions (`WorldReal`) and, through the vcpkg
`large-world` feature, the matching Jolt. A runtime switch, or templating every
module on precision, was rejected: either would cost hot loops or spread
templates through every module's headers. A float build pays nothing for
doubles. Terrain addressing, audio (Wwise takes double) and camera-relative
rendering keep the same code in both builds.

*2026-09-11* — **`toolchainFile` in presets, not a wrapper `.cmake`.** vcpkg
resolution moved from `cmake/VcpkgToolchain.cmake` into each preset's
`toolchainFile: $env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake`. Fewer moving
parts, and matches how vcpkg's own docs and cloud CI setups expect it wired. The
cost is a plainer error when `VCPKG_ROOT` is unset or wrong: CMake reports the
toolchain file as missing instead of the old message's exact fix, which is why
that fix now lives in Troubleshooting below.

*2026-09-01* — **CMake, not premake.** Premake is pleasanter to write, but
vcpkg, CMake presets and the VS Code tooling all assume CMake, and the ecosystem
around it is not close.

Single-config generators were chosen over multi-config, because single-config
keeps a configuration component out of the output paths, which the shader and
asset copy steps depend on.

### Scene catalog

Register scenes in the game's `game.json`:

```json
{
  "startupScene": "intro",
  "scenes": [
    { "id": "intro", "path": "scenes/default" },
    { "id": "level-two", "path": "scenes/level-two" }
  ]
}
```

IDs are unique within a game. Keep the ID when moving a scene and update its
path. Each registered directory must contain a `scene.json` to be packaged.
The packaged `game.json` serves as the runtime catalog and uses paths relative
to the packaged game. Legacy games can keep `sceneDir`. To register a legacy
startup scene explicitly, give it the ID `default` and use its existing path.
Opening or packaging a game leaves the source manifest intact.
