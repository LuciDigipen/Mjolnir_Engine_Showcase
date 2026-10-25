# script bridges

Five small libraries, one per engine library with a Lua surface: `audio_script`,
`input_script`, `physics_script`, `renderer_script` (post-processing) and
`terrain_script`. Each adds its library's Lua bindings to the script host, so
neither side links the other.

`mir::audio_script`, `mir::input_script`, `mir::physics_script`, `mir::renderer_script`, `mir::terrain_script` · each depends `mir::script`, `script_bindings` (public) and its own library (private) · API `modules/<name>_script/include/<name>_script/` · maintainer Sumin Park · reviewed 2026-10-24

## Model

A bridge is a module whose constructor takes the `ScriptModule` and calls
`AddBindings(scripts.Host(), &Register...Bindings)`. The host runs the function
on its Lua state at once and keeps it, to run again on any state it builds
later. The returned token is the module's only member. Which Lua functions each
bridge adds is in [script.md](script.md).

`script_bindings` is an interface target over `script/BindingApi.h`, the one
header that includes sol2. Only the bridges link it, so sol2 stays out of every
other library, `script`'s other public headers included.

## API

| Type | Header | Role |
|---|---|---|
| `AudioScriptModule`, `RegisterAudioBindings` | `audio_script/` | `world:post_event/stop_event/set_rtpc/set_switch/set_state` |
| `InputScriptModule`, `RegisterInputBindings` | `input_script/` | `world:is_down/just_pressed/just_released/axis/axis2d/active_device` |
| `PhysicsScriptModule`, `RegisterPhysicsBindings` | `physics_script/` | Rigid bodies, every `RigidBody` field, `teleport`, sleep control, raycasts and collision events |
| `RendererScriptModule`, `RegisterPostProcessingBindings` | `renderer_script/` | `world:set_fog/get_fog` and the other post-processing pairs |
| `TerrainScriptModule`, `RegisterTerrainBindings` | `terrain_script/` | `world:terrain_height_at` |

## Rules

**A bridge comes after `ScriptModule` and its own library's module.** That is
the dependency edge, and `App` declares the members in that order.

**Bridges stay independent.** Each adds to its own functions on the `World`
usertype. If two ever touch one table, the later depends on the earlier, as with
a same-phase system order.

**Dropping the token keeps what it bound.** A Lua state cannot unbind a function
cleanly, so the token only stops the bindings from running on a state built
later.

## State

*As of 2026-10-24.* All five work, moved from `script/src/script/bindings/`
unchanged. Their tests moved with them: `AudioBindings`, `InputBindings`,
`PhysicsBindings`, `CollisionBindings`, `TerrainContactScript`,
`PostProcessingBindings` and `TerrainBindings`, each test adding its bridge's
bindings to a `ScriptHost` the way the bridge does.

## Backlog

1. Function reflection in `core`, if bridges multiply. Queries, callbacks and
   tables do not fit a reflection table today.

## Changed

*2026-10-25* — **Layers and ray cast filters in the physics bridge.** Was: every body collided with every other, and
`world:raycast` hit whatever was first, its caster's own body included. Now: a body has a `layer` that a script sets by
name or index (`set_layer`, `layer_index`), and `world:raycast(origin, direction, distance, options)` takes `layers`,
`motion`, `exclude`, `ignoreSelf` (default true) and `hitTriggers` (default false). The host remembers the entity whose
callback is running, which is how a cast knows its caster. Because: a script cannot hit itself by accident, and a game
needs to say what a cast is for.

*2026-10-25* — **The physics bridge reaches every `RigidBody` field.** Was: scripts could push a body with
forces and velocities, but not read or change its settings. Now: each `RigidBody` field has a getter named
after it and a `set_` setter (`friction`, `set_friction`, `half_extent`, `set_offset`), both going through
the same field write the editor uses. `wake`, `is_sleeping` and `teleport` cover what no field can, and
collision events gain a `"stay"` phase. Because: a script should be able to do whatever the editor can.

*2026-10-24* — **Added.** Was: every binding lived in `script`, and
`ScriptHost`'s constructor called each library's `Register...Bindings` by hand.
Now: one bridge per library. Because: `script` linked five libraries for their
bindings alone, and a list in the host had to change for every new one.
