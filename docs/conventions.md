# Conventions

Rules. How to apply them to a specific task is in [extending.md](extending.md).

## Layout

One static library per module, in `modules/<name>/`, named lowercase
`[a-z][a-z0-9_]*`.

`include/<module>/` is the public API and `src/<module>/` is everything else. A
header's include path mirrors its directory, so `core/ecs/World.h` sits at
`modules/core/include/core/ecs/World.h`.

Tests go in `modules/<name>/tests/`, named `<Subject>.tests.cpp`.

Create files and modules with the scaffolding rather than by hand. The two
mirrored trees and the `target_sources` edit are the parts that get forgotten,
and the symptom arrives later as a link error.

Every `.cpp` that needs to be compiled must be listed in `target_sources`.
Nothing is globbed, so a file left off that list is never compiled and its
symbols never reach the link.

## Naming

| Kind | Form | Example |
|---|---|---|
| Namespace | lowercase, all in `mir` | `mir::detail` |
| Type | `PascalCase` | `AssetCache` |
| Interface | `I` prefix | `ISystem` |
| Function, method | `PascalCase` | `CreateEntity` |
| Local, parameter | `camelCase` | `imageIndex` |
| Member | `mPascalCase` | `mSwapchain` |
| Constant, `constexpr` | `k` prefix | `kFramesInFlight` |
| Macro | `MIR_SCREAMING_CASE` | `MIR_ASSERT` |
| CMake function | `engine_snake_case` | `engine_add_module` |
| CMake target | `engine_<module>`, alias `mir::<module>` | `engine_core` |
| File | `PascalCase.h`/`.cpp`, matching its main type | `VulkanRenderer.cpp` |

Members take `mPascalCase` engine-wide, including the fields of a `*Desc`
struct. The prefix separates a member from a parameter at a glance, which the
Vulkan code leans on heavily.

Plain-data types that describe a **layout** are the exception and take no
prefix — `AssetBlobHeader`, `AssetManifestEntry`, component fields. Those names
are the format itself, so renaming one changes the file it describes.

## Includes and formatting

Include groups go in this order, with a blank line between each and alphabetical
order within:

1. Own module, quoted: `#include "TypeId.h"`
2. Other modules, angled: `#include <core/log/Log.h>`
3. Third party
4. Standard library

Headers use `#pragma once`.

Don't forget header formats for each file. Check [build.md](build.md#3-setting-name-in-headers-optional)
```
/**
 * @file App.h
 * @author Sumin Park
 * @brief Owns the engine subsystems and drives the main loop.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
```

`templates/.clang-format` sets `DisableFormat: true` and `SortIncludes: Never`,
so an editor with format-on-save does nothing. The Vulkan struct initialisers
and the log macro tables are laid out by hand to be read as tables, and a
formatter rewrites them. Sorting includes alphabetically across the whole file
also destroys the group order above, which is what makes a file's dependencies
visible.

## Comments

Plain `//` for everything, whether it documents a declaration or notes something
inline.

## ECS

Components are POD. Put `MIR_ASSERT_COMPONENT(T)` next to the declaration, so a
violation is a compile error at the offending struct rather than deep inside a
template instantiation.

Component names must be globally unique. `TypeIdOf` strips namespaces and hashes
the bare name, so two `Position` structs in different namespaces collide. A
collision asserts in Debug.

A component with no fields is a tag. It gets one signature bit and no column.
Add it with `AddTag<T>`, and filter on it with `With`, `Without` or `Or`. A tag
has nothing to read, so naming it as a query data term is a compile error.

Never add, remove or destroy a component while iterating. An add moves the entity
to another table and swap-removes its old row, so an entity you have not visited
slides into a row you already passed and is skipped silently. Record the change
on the `CommandBuffer` instead.

Systems cache their queries in `OnStart`.

Ordering between systems is a phase and a `SystemOrder`, never registration order.
Inside a phase the lowest order runs first, and equal orders are unspecified. Two
systems that depend on each other get different orders, and a system with no
dependency passes `SystemOrder::kNormal`.

Read-only components are `const T` in the query. Queries then carry the access
information a parallel scheduler will need.

There are at most 256 component types, counting C++ and script-declared ones
together. Raising that is a deliberate edit to `Signature.h`.

Register a new C++ component in its module's `ComponentRegistration.cpp`. Give it
a field table if scripts, the inspector or serialization should reach its values.
Forgetting to register is not an error — it produces a name that nothing can find
— and a component with no field table serializes as present but empty.

Register the C++ components before loading any script. A name belongs to whoever
claims it first.

## Errors

`MIR_ASSERT` is for programmer error and compiles out in Release. Its condition
is not evaluated there at all, so nothing inside it may have a side effect the
program depends on.

`MIR_CHECK` stays in Release. Use it whenever the bad input can come from data
rather than from C++

Recoverable failure returns `false`, `nullptr` or `std::optional`, and logs once.

Every setup failure is fatal. Setup is everything `App`'s constructor builds,
every module constructor and the first scene load included. A failing step
throws a `SetupError` (`core/log/SetupError.h`) instead of returning. C++ then
destroys the members already built, in reverse, and `main` reports the error.
A module destructor never throws: it logs what fails and carries on, and `main`
exits with 15 when teardown logged an error.

A `SetupError` carries what failed, the path, the cause and the fix. The fix
names a concrete action, such as the file to restore or the command to run. The
reader is a person or an agent without the engine's source, so the fix must be
enough on its own. Native codes (`AKRESULT`, `VkResult`, a Win32 or GLFW error,
a JSON parse position) go into the cause.

Exceptions are allowed anywhere, with two limits. A function called from C code
or a foreign thread, such as a library callback or a thread entry point, is
`noexcept`. Unwinding through a C library's frames is undefined, and an
exception leaving a thread calls `std::terminate`. Its body may fail only by
allocation, which terminates and reaches the crash handler. Anything else that
can throw is caught inside, with a stated fallback. Each registration site
checks the `noexcept` with a `static_assert`. A worker job runs its body through
`RunGuarded` (`terrain/RunGuarded.h`), which turns an exception into that job's
error result and lets `std::bad_alloc` terminate. The second limit is about
state: at runtime, catch only to report and exit, or around an operation that leaves
nothing half-done, such as a model load. A catch that carries on mid-frame can
leave the `World` or a command buffer half-updated.

Nothing outside `tools/` uses `iostream`.

### Exit codes and the error report

A fatal error ends the process with the exit code of its category. The category
says what the reader does next. These numbers are a public contract that scripts
and agents branch on, so a number is never reused.

| Exit | Category | What to do |
|---|---|---|
| 0 | | Success |
| 1 | | A `--frames` run logged an error, or a project switch could not relaunch |
| 10 | Usage | Fix the command line |
| 11 | Project data | Fix or restore the game's files: `game.json`, the scene, `project_settings.json`, SoundBanks |
| 12 | Build output | Rebuild: the manifest, AssetCooker, `asset_workspace.json`, `engine.json`, the fonts |
| 13 | System | Fix the machine: GPU driver, display, audio device, permissions |
| 14 | Internal | Report a bug with the log attached |
| 15 | Shutdown | The run finished, but something failed while shutting down; the log says what |
| 20 | Crash | Report a bug with the log and `logs/crash.dmp` attached |

The log is `logs/engine.log` next to the executable, whatever the launch
directory. Its absolute path is the first line the engine prints. A fatal error
also writes `logs/last-error.json` with the same fields as the `SetupError`. Each
run deletes the previous one at startup, so a report on disk belongs to the
latest run.

A crash or `std::terminate` on any thread goes to the crash handler
(`core/platform/CrashHandler.h`), installed first in `main`. It writes the same
report with `kind` set to `crash`, and a minidump at `logs/crash.dmp`, from a
thread of its own, then ends the process with exit 20. Destructors do not run on
that path, so anything a crash must not lose is the handler's job, not a
module's. A caught failure's report has `kind` set to `caught`.

A person sees the error in a box. `MjolnirErrorReporter`, shipped next to the
engine, shows it after the engine has already exited, so the engine never waits
on a click. A prompt the engine needs answered, such as opening a scene with
missing entity files, takes its safe answer under `--non-interactive`.

Each module's doc lists the categories it raises under Rules → Errors.

Log levels: `Trace` and `Debug` for per-frame detail, `Info` for lifecycle,
`Warn` for degraded, `Error` for a failed operation, `Critical` for
unrecoverable. Anything below `Info` compiles out in Release, so `Trace` and
`Debug` are free to leave in hot paths.

## Ownership

`unique_ptr` owns, a raw pointer observes, and a reference is required to be
non-null.

A type that owns a resource deletes copy. It also deletes move, unless move is
genuinely correct for it.

A member that aliases another member is declared in destruction order, with a
comment saying so. `App::mAssetManifest` and `mAssetCache` are the example.

## Dependencies

Everything comes through `vcpkg.json`. Nothing is vendored or fetched at build
time.

Link `PRIVATE` unless one of the library's types appears in your public header,
and say why in a comment. A `PUBLIC` link puts that library's include path on
every consumer of your module, transitively.

`third_party/imgui` is the one exception. It is a git submodule.

## Tests

Catch2 v3 through `engine_add_module_tests`, one `TEST_CASE` per behaviour,
exercised through the module's public header.

A module's test executable links only that module. A test that needs a second
module is a signal that the boundary is in the wrong place.

New logic in `core`, `assets`, `scene` and `script` ships with tests. `renderer`,
`window` and `editor` need a device and a display, and are verified by running.

Run `ctest --test-dir builds/windows-msvc-debug --output-on-failure` before
pushing.

## Changed

*2026-10-24* — **Callbacks and thread entries are `noexcept`, and fail only by
allocation.** Was: each one caught or was `noexcept`, by choice. Now: all are
`noexcept`, checked by a `static_assert`, worker jobs go through `RunGuarded`,
and `std::bad_alloc` always terminates. Because: a throw inside a terrain job
aborted the process with no report, and swallowing an allocation failure
inside a callback leaves its state silently wrong.

*2026-10-20* — **Setup failures throw a `SetupError`, and exceptions are
allowed.** Was: initialization returned `bool` and left the caller to decide,
and engine code used no exceptions. Now: every setup failure is fatal, it throws
a `SetupError` with a fix, and `main` turns it into an exit code, a report file
and a box. Because: a `bool` carried no reason, so a failed launch showed nothing
and the fix lived only in the code. Exceptions were already thrown inside the
asset and animation code, and a constructor can only fail by throwing.

*2026-09-01* — **Members are `mPascalCase`.** Was: three conventions, roughly by
the age of the code. `core/ecs` and `assets` used `mPascalCase`, `renderer` used
`m_PascalCase`, and `app`, `window` and the `*Desc` structs used `m_camelCase`.
Now: one form everywhere. `mPascalCase` won because it was already the largest
and newest body of code, so the least code had to move. The rename landed as
isolated mechanical commits listed in `.git-blame-ignore-revs`; enable it per
clone with `git config blame.ignoreRevsFile .git-blame-ignore-revs`.
