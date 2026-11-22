# flight_debug

The `Flight Debug` imgui window: a Tools drawer that shows the active Play world's aircraft, its configuration
and its live flight state, edits the selected aircraft's configuration while it flies, saves it back to the
source file in an editor build, and switches the aerodynamic tier at runtime. It does not edit the authored
scene or plot curves.

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
mir::FlightDebugModule flightDebug(context, imgui, sourceAssetsRoot);
flightDebug.ActivateWorld(&runtime.state.world);
```

The window lists the entities with `Aircraft`, defaulting to the first, and shows the config path and whether
the store loaded it. The tier combo writes `Aircraft::tier` on the Play-world component and marks the profiles
the config lacks. The read-only sections are built as `FlightDebugRow` label/value pairs by
`BuildFlightDebugConfigRows`, `BuildFlightDebugTelemetryRows`, `BuildFlightDebugForceRows` and
`BuildFlightDebugTableRows`, away from the ImGui calls; angles are converted to degrees there. The stall margin
row carries `warning`, which the window draws highlighted. Forces and moments are body frame, X nose, Y right
wing, Z belly.

The Edit section comes from `BuildFlightDebugEditableFields`, a table of section, label, getter and setter into
`AircraftConfig`, plus a flag for values stored in radians and edited in degrees. The module draws one drag
widget per entry and calls `MarkEdited` on the store when a value changes; engines and gear points contribute
one group each, so their fields follow the parsed lists. Curves, detent lists and the L2 and L3 tables are
shown as sizes in the read-only table rows.

The L3 section reads `SampleL3` for the selected aircraft's current alpha, beta, Mach and flap and shows the
lower cell index per axis and the six base coefficients; the out-of-table row carries `warning`. The
`Generate L3 from L2` button is enabled only for a config with an L2 profile, rebuilds the L3 profile from the
config's own L2 with the default grids and marks the path edited, so the normal Save writes it back.

`FlightDebugModule` takes the source assets root from `App` and passes it to `SaveAircraftConfigToSource`, which
rewrites `<root>/<config path>` through `WriteTextAtomic` and clears the dirty flag on success. A non-editor
build gets an empty root, so the Save button stays disabled.

## API

| Type | Header | Role |
|---|---|---|
| `FlightDebugModule` | `<flight_debug/FlightDebugModule.h>` | the Tools drawer, its Play-world hand-over and the save root |
| `FlightDebugRow` | `<flight_debug/FlightDebugRows.h>` | one label, its value text and an optional warning |
| `FlightDebugField` | `<flight_debug/FlightDebugRows.h>` | one editable scalar: its section, label, accessors and degree flag |
| `BuildFlightDebugConfigRows`, `BuildFlightDebugTelemetryRows`, `BuildFlightDebugForceRows`, `BuildFlightDebugL3Rows`, `BuildFlightDebugTableRows` | `<flight_debug/FlightDebugRows.h>` | the row text without ImGui |
| `BuildFlightDebugEditableFields` | `<flight_debug/FlightDebugRows.h>` | the editable field table |
| `SetFlightDebugTier`, `FlightDebugTierAvailable` | `<flight_debug/FlightDebugRows.h>` | write `Aircraft::tier` and report a missing profile |
| `FlightDebugWindowVisible`, `FlightDebugDegrees` | `<flight_debug/FlightDebugRows.h>` | the visibility rule and the radian-to-degree conversion |
| `SaveAircraftConfigToSource` | `<flight_debug/FlightDebugSaving.h>` | serialize and atomically rewrite the source `*.aircraft.json` |

## Rules

**`imgui.h` appears only in `FlightDebugModule.cpp`.** The public headers name no ImGui type, so `App.h` can
hold the module without reaching ImGui.

**The window reads the Play world only in editor builds.** The authoring world has no flight runtime; stopping
Play restores the authored scene. `ActivateWorld(nullptr)` returns the module to the authoring world and hides
the window again.

**The tier switch writes the Play-world component only.** Saving a tuned tier back into the scene is
out of scope; Stop discards it with the Play snapshot.

**Edits go through the World's store.** The module reads `AircraftConfigStoreRef` from the active world and
edits the store's object for the path, so every aircraft on that file sees the change and the next fixed step
re-applies it. With no store in the world, or a path that did not load, the Edit section is hidden.

**Only a dirty path saves, and only with a source root.** The `* unsaved` marker follows `IsDirty`; a
successful write clears it. The text is parsed before it is written, so an edit the parser rejects leaves the
source file as it was. That and a failed write keep the marker and show the error, which is logged too. A reload
that replaces unsaved edits leaves a notice the window shows until it is dismissed.

**L3 is read and generated, not edited.** The L3 section and its out-of-table highlight are read-only; the
generate button replaces the store's L3 profile through `Edit` + `MarkEdited` and saves with the same path as
the scalar edits. The tables themselves are still edited in the JSON file.

**The row builders and the field table are pure.** They take the config or the runtime, so the tests need no
window, GPU or ImGui context.

## State

*As of 2026-11-18.* The panel lists aircraft, shows the config path and load state, the tier combo, the
configuration summary, the live telemetry in degrees, the body-frame forces and moments by source, the L3 cell
and coefficients with the out-of-table highlight, the editable scalar table and the read-only curve and table
sizes, generates L3 from L2, and saves the edited config to the source file. `FlightDebugToggle` is F3 with no
gamepad binding. `tests/FlightDebug.tests.cpp` covers the telemetry labels and degree conversion, the
stall-warning highlight, the configuration summary, the force rows, the L3 cell rows and their flag, the tier
switch and its missing profiles, and the visibility rule; `tests/FlightDebugTuning.tests.cpp` covers the save
helper, the field table and the read-only table rows; target `engine_flight_debug_tests`.

## Backlog

Curve plotting and table editing in the panel are tracked separately.

## Changed

*2026-11-18* — **Added the L3 section and its generator.** Was: the panel showed and edited L1/L2 profiles
only. Now: it shows the L3 cell and base coefficients with the out-of-table highlight and generates an L3
profile from the config's L2. Because: the table model needed a visible starting point and a way to build it
without hand-writing hundreds of values.

*2026-11-18* — **Added live tuning and save.** Was: a panel that showed the Play world and switched the tier.
Now: the panel edits the shared config of the selected path, marks unsaved edits, saves them to the source file
in an editor build and reports a reload that overwrote them. Because: tuning the flight model needed the values
to change while flying, not only in the JSON file.

*2026-11-18* — **Added.** Was: no runtime flight inspection outside the Lua telemetry bindings. Now: a
Play-world Tools panel over the same telemetry. Because: tuning the flight model needed the live values next
to the configuration.
