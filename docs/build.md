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

## Optional RTXGI DDGI SDK

Configure with `cmake --preset windows-msvc-debug -DMIR_WITH_RTXGI=ON`.
This fetches RTXGI-DDGI SDK 1.3.6 at commit
`f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6` without its samples or
submodules. Review the SDK's `License.txt` before redistributing it.
The default `OFF` adds no RTXGI download or DXC requirement.

An enabled build requires DXC with SPIR-V support, found first in the Vulkan
SDK and then on PATH; override it with `-DRTXGI_DXC_EXECUTABLE=<path>`.
The SDK's Vulkan sources use the engine's Volk loader, with resource management
disabled so the renderer will own probe allocations. Host code and shaders use
right-handed, Y-up coordinates. SDK shader resources use set 0: volume constants
at binding 0, ray data at 1, irradiance at 2, distance at 3, probe data at 4,
and variability at 5; batch constants use binding 7 and the SDK push-constant
layout is used. The probe RT pipeline binds these resources as set 2, with
sampled irradiance/distance/state at bindings 8–10 and a sampler at 11.

`cmake --build builds/windows-msvc-debug --target engine_rtxgi` checks the SDK
and six probe-update/reset shaders. The engine build copies these shaders to
`shaders/rtxgi` beside the executable. Irradiance/distance atlas interiors are
6/14 texels; blending reads the ray count dynamically instead of specializing
it at compile time. Existing engine shaders continue to use Slang.

Enabled GI settings allocate probes and run bounded probe updates. Final camera
shading does not yet sample those probes. `renderer/RtxgiConfig.h` reports SDK
compilation separately from runtime resource availability. DLSS remains an
independent option. See [renderer](modules/renderer.md) for pass order and ownership.

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

In the Testing panel the same thing is a click on one case or one file.

### Smoke tests

Every game with a `game.json` gets a `smoke.<game>` test. It runs
`MjolnirEngine --game games/<game> --frames 120`, which closes the window after
120 frames and exits non-zero if anything was logged as an error.

`smoke.switch` restarts from HelloWorld into EmptyScene and back — what
Project > Open Project does, minus the dialog. `--switch-to <dir>` asks for the
restart halfway through `--frames`. The next process gets the other half and the
remaining `--switch-to` entries, and in a `--frames` run each process waits for
the next one and exits with its code.

They open a real window, so they need a GPU and a display. `-L smoke` runs only
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
