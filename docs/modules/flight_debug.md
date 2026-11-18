# flight_debug

The `Flight Debug` imgui window: a Tools drawer that shows the active Play world's aircraft, its configuration
and its live flight state, and switches the aerodynamic tier at runtime. It does not edit the authored scene,
save tuning or plot curves.

`mir::flight_debug` · depends `mir::core` (public); `mir::flight`, `mir::imguiruntime`, `mir::input`, `imgui`
(private) · API `modules/flight_debug/include/flight_debug/` · maintainer Jackey Cen · reviewed 2026-11-18

## Model

`FlightDebugModule` registers a `DrawerGroup::Tools` drawer with `ImGuiModule::AddDrawer` and follows the
world `App` hands it, the way `GameUIImGuiModule` follows the game UI. `ActivateWorld` takes the Play world at
Play start and after a runtime scene swap, and takes `nullptr` on Stop. The drawer reads the active world's
`InputState`; a fresh `FlightDebugToggle` press flips the window, and the window's close button clears it.

Editor builds show the window only while a Play world is active. A game build shows it whenever it is toggled
on, which keeps it usable as a developer overlay. `FlightDebugWindowVisible` holds that rule so the tests cover
the build flag without ImGui.

```cpp
mir::FlightDebugModule flightDebug(context, imgui);
flightDebug.ActivateWorld(&runtime.state.world);
```

The window lists the entities with `Aircraft`, defaulting to the first, and shows the config path and whether
the store loaded it. The tier combo writes `Aircraft::tier` on the Play-world component and marks the profiles
the config lacks. The remaining sections are built as `FlightDebugRow` label/value pairs by
`BuildFlightDebugConfigRows`, `BuildFlightDebugTelemetryRows` and `BuildFlightDebugForceRows`, away from the
ImGui calls; angles are converted to degrees there. The stall margin row carries `warning`, which the window
draws highlighted. Forces and moments are body frame, X nose, Y right wing, Z belly.

## API

| Type | Header | Role |
|---|---|---|
| `FlightDebugModule` | `<flight_debug/FlightDebugModule.h>` | the Tools drawer and its Play-world hand-over |
| `FlightDebugRow` | `<flight_debug/FlightDebugRows.h>` | one label, its value text and an optional warning |
| `BuildFlightDebugConfigRows`, `BuildFlightDebugTelemetryRows`, `BuildFlightDebugForceRows` | `<flight_debug/FlightDebugRows.h>` | the row text without ImGui |
| `SetFlightDebugTier`, `FlightDebugTierAvailable` | `<flight_debug/FlightDebugRows.h>` | write `Aircraft::tier` and report a missing profile |
| `FlightDebugWindowVisible`, `FlightDebugDegrees` | `<flight_debug/FlightDebugRows.h>` | the visibility rule and the radian-to-degree conversion |

## Rules

**`imgui.h` appears only in `FlightDebugModule.cpp`.** The public headers name no ImGui type, so `App.h` can
hold the module without reaching ImGui.

**The window reads the Play world only in editor builds.** The authoring world has no flight runtime; stopping
Play restores the authored scene. `ActivateWorld(nullptr)` returns the module to the authoring world and hides
the window again.

**The tier switch writes the Play-world component only.** Saving a tuned tier back into the scene is
out of scope; Stop discards it with the Play snapshot.

**The row builders are pure.** They take the config or the runtime and return strings, so the tests need no
window, GPU or ImGui context.

## State

*As of 2026-11-18.* The panel lists aircraft, shows the config path and load state, the tier combo and the
configuration summary, the live telemetry in degrees and the body-frame forces and moments by source.
`FlightDebugToggle` is F3 with no gamepad binding. `tests/FlightDebug.tests.cpp` covers the telemetry labels and
degree conversion, the stall-warning highlight, the configuration summary, the force rows, the tier switch and
its missing profiles, and the visibility rule; target `engine_flight_debug_tests`.

## Backlog

Live parameter editing and saving (#460), L3 grid display (#462) and curve plotting are tracked separately.

## Changed

*2026-11-18* — **Added.** Was: no runtime flight inspection outside the Lua telemetry bindings. Now: a
Play-world Tools panel over the same telemetry. Because: tuning the flight model needed the live values next
to the configuration.
