# script bridges

Six small libraries, one per engine library with a Lua surface: `audio_script`,
`input_script`, `physics_script`, `renderer_script` (post-processing) and
`terrain_script` and `particles_script`. Each adds its library's Lua bindings to the script host, so
neither side links the other.

`mir::audio_script`, `mir::input_script`, `mir::physics_script`, `mir::renderer_script`, `mir::terrain_script`, `mir::particles_script` · each depends `mir::script`, `script_bindings` (public) and its own library (private) · API `modules/<name>_script/include/<name>_script/` · maintainer Sumin Park · reviewed 2026-10-27

## Model

A bridge is a module whose constructor takes the `ScriptModule` and calls
`AddBindings(scripts.Host(), &Register...Bindings)`. The host runs the function
on its Lua state at once and keeps it, to run again on any state it builds
later. The returned token is the module's only member. Which Lua functions each
bridge adds is in [script.md](script.md), and the physics ones are listed below.

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
| `ParticlesScriptModule`, `RegisterParticleBindings` | `particles_script/` | `world:particle_system_start/particle_system_stop/particle_system_pause/particle_system_reset/particle_system_destroy/particle_system_state/particle_system_parameters`, see [particles.md](particles.md) |

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

*As of 2026-10-27.* The original five work, moved from `script/src/script/bindings/`
unchanged. Their tests moved with them: `AudioBindings`, `InputBindings`,
`PhysicsBindings`, `CollisionBindings`, `TerrainContactScript`,
`PostProcessingBindings` and `TerrainBindings`, each test adding its bridge's
bindings to a `ScriptHost` the way the bridge does.

`particles_script` adds CPU emitter controls, verified by `ParticleBindings`
against the headless playback resource. It does not create a renderer dependency.

## Backlog

1. Function reflection in `core`, if bridges multiply. Queries, callbacks and
   tables do not fit a reflection table today.

## Physics Lua API

Every function is on the `World` usertype, called as `world:name(entity, ...)`. A call that does not do what it says returns `false` (or `nil` for a read), and the reason is logged. A bad call made every frame logs every frame, because the log throttle is not built yet.

**The forms.** A vector is a table `{ x = number, y = number, z = number }`, a rotation adds `w`, which is 1 when left out. A missing key, a value that is not a number, NaN or infinity, an array such as `{ 1, 2, 3 }`, or any other table is refused with one error that names the call, the argument and the form expected, and it never becomes a zero vector. A position is the same table in meters. A layer is a name from the project's table or an index from 0 to 31.

**Body fields.** Each `RigidBody` field has a reader `name(entity)` and a writer `set_name(entity, value)`, with the field name in snake case. The writer returns whether the value was taken, and it is applied to the live body on the next step. A value outside what the field allows is refused with one error, and the field keeps its value: a choice from a list must be a whole number in the list, and a number must be inside the range the field declares (`mass` above 0, `restitution` from 0 to 1, `friction` from 0). A reader answers `nil` for an entity with no `RigidBody`.

| Reader | Writer | Value |
|---|---|---|
| `motion` | `set_motion` | `PhysicsMotion.Static`, `.Dynamic` or `.Kinematic` |
| `shape` | `set_shape` | `PhysicsShape.Box`, `.Sphere` or `.Capsule` |
| `half_extent` | `set_half_extent` | vector, every axis above 0 |
| `radius`, `half_height` | `set_radius`, `set_half_height` | number above 0 |
| `capsule_axis` | `set_capsule_axis` | `PhysicsAxis.X`, `.Y` or `.Z`. Not `axis`, which is the input bridge's `world:axis(action)` |
| `offset` | `set_offset` | vector, in the entity's own axes |
| `rotation_offset` | `set_rotation_offset` | rotation |
| `interpolate`, `is_trigger`, `detect_stay`, `allow_sleeping` | `set_interpolate`, `set_is_trigger`, `set_detect_stay`, `set_allow_sleeping` | boolean |
| `mass` | `set_mass` | number above 0, kilograms |
| `gravity_factor`, `friction`, `restitution` | `set_gravity_factor`, `set_friction`, `set_restitution` | number, `restitution` from 0 to 1 |
| `layer` | `set_layer` | a layer name or index |

**Forces and velocities.** All return `true` when the body took the call. They return `false` for an entity with no live body, a bad vector, a force, torque or impulse on a body that is not Dynamic, and a velocity on a Static body. A body that is held takes impulses and velocities and no force.

| Call | Arguments | Effect |
|---|---|---|
| `add_force`, `add_impulse` | `entity, vector[, offset]` | A world-space force or impulse, at the center or at a point given in the body's own axes |
| `add_torque`, `add_angular_impulse` | `entity, vector` | A torque or an angular impulse |
| `set_linear_velocity`, `set_angular_velocity` | `entity, vector` | Replaces the velocity |
| `add_linear_velocity`, `add_angular_velocity` | `entity, vector` | Adds to the velocity |
| `linear_velocity`, `angular_velocity` | `entity` | The velocity as a vector, zero for an entity with no body |

**Poses.** The entity's pose is its Transform, written with `world:set` on `Transform`. A write moves a Dynamic body there, moves a Kinematic body there over the step, and carries a parented body along with its parent. `teleport(entity, position[, rotation])` puts the entity and its body at a world pose with no sweep, whatever the motion type, and keeps the rotation when none is given. `wake(entity)` wakes a sleeping body, and `is_sleeping(entity)` says whether it sleeps.

**Layers.** `layer_index(name)` answers the index of a layer, or `nil`. An unknown name or an index outside 0 to 31 is an error that lists the layers that exist.

**`raycast(origin, direction, maxDistance, options)`.** `maxDistance` defaults to 1000. It returns a table with `hit`, and when it hit, `point`, `normal`, `distance` and `entity`. `options` is optional, and an unknown key is an error that lists the valid ones.

| Option | Default | Meaning |
|---|---|---|
| `layers` | every layer | A list of layer names or indices to hit |
| `motion` | all three | A list of `PhysicsMotion` values to hit |
| `exclude` | none | A list of entities to pass through |
| `ignoreSelf` | `true` | Passes through the body of the entity whose callback is running |
| `hitTriggers` | `false` | Whether triggers can be hit |

**`collisions(entity)`.** A list of this frame's events that involve the entity, each `{ other, point, normal, phase }`, with `phase` `"begin"`, `"end"` or `"stay"`, and `normal` pointing from `other` toward the entity. `"stay"` arrives once per frame for the pairs a body with `detect_stay` keeps touching. `other` is `nil` for a body with no entity.

**The tables.** `PhysicsMotion`, `PhysicsShape` and `PhysicsAxis` map each name to the integer the fields hold.

## Changed

*2026-10-25*, **Layers and ray cast filters in the physics bridge.** Was: every body collided with every other, and
`world:raycast` hit whatever was first, its caster's own body included. Now: a body has a `layer` that a script sets by
name or index (`set_layer`, `layer_index`), and `world:raycast(origin, direction, distance, options)` takes `layers`,
`motion`, `exclude`, `ignoreSelf` (default true) and `hitTriggers` (default false). The host remembers the entity whose
callback is running, which is how a cast knows its caster. Because: a script cannot hit itself by accident, and a game
needs to say what a cast is for.

*2026-10-25*, **The physics bridge reaches every `RigidBody` field.** Was: scripts could push a body with
forces and velocities, but not read or change its settings. Now: each `RigidBody` field has a getter named
after it and a `set_` setter (`friction`, `set_friction`, `half_extent`, `set_offset`), both going through
the same field write the editor uses. `wake`, `is_sleeping` and `teleport` cover what no field can, and
collision events gain a `"stay"` phase. Because: a script should be able to do whatever the editor can.

*2026-10-24* — **Added.** Was: every binding lived in `script`, and
`ScriptHost`'s constructor called each library's `Register...Bindings` by hand.
Now: one bridge per library. Because: `script` linked five libraries for their
bindings alone, and a list in the host had to change for every new one.
