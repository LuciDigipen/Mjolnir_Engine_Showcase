# HelloWorld scene demo

Reopen HelloWorld after rebuilding, enter Play, and click the Game view to give it keyboard input.

| Key | Action |
|---|---|
| **N** | Cycle `default` → `demo_gallery` → `demo_plaza` → `default` |
| **B** | Load or unload `demo_beacons` additively |

The gallery and plaza have fixed cameras and different colored columns. The
additive scene adds a bobbing gold cube and an orbiting pink satellite to the
current scene. The current primary scene keeps rendering and simulating while
the additive scene loads.

Try **B**, wait for the beacons to appear, then press **B** again. Both objects
disappear. The gold cube is saved in `demo_beacons`, while its satellite is
spawned by Lua and inherits the additive instance's ownership. The engine removes
both when that instance unloads. Pressing **B** again loads a fresh instance.
Pressing **N** while the beacons are loaded replaces the primary and additive
scenes together.

`assets/scripts/scene_demo.lua` persists a controller across replacements and
uses `world:session_data()` to retain the current cycle index, completed swaps,
and additive visit count. Each primary scene contains the controller so the demo
also works when starting Play directly in the gallery or plaza. A revisited scene
discards its duplicate controller and keeps the existing session controller.
Stop ends the session and resets the demo.

`assets/scripts/scene_beacon.lua` demonstrates `scene_instance`, implicit spawn
ownership, and session data. The controller uses `find_scene_entity(instance, 1)`
to resolve the gold beacon after `scene_loaded(instance)` reports readiness.
Status messages and the visit count appear in the engine log/console.

The controls are project input actions `SceneDemoNext` and `SceneDemoAdditive`
in `project_settings.json`. Scene IDs and directories are registered in
`game.json`. The existing `newScene` scene remains available separately.
