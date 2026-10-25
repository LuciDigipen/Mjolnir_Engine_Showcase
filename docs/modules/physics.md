# physics

Owns the Jolt world, the `RigidBody` component and the systems that keep bodies and Transforms in step. It stays out of Lua, which the `physics_script` bridge adds, and of terrain, which builds its collision on the height field type physics exposes.

`mir::physics` · depends `mir::core` (public), `mir::serial` and Jolt (private) · API `modules/physics/include/physics/` · maintainer Sumin Park · reviewed 2026-10-25

## Model

Every entity with a `Transform` and a `RigidBody` has one Jolt body, held by the `PhysicsWorld` resource. Jolt is private to the module, so nothing under `include/physics` names a Jolt type, and a body is a `BodyHandle` that stays valid until the body is destroyed.

A body has a motion type. A **Static** body never moves and acts as infinitely heavy. A **Dynamic** body is moved by gravity, forces, impulses and contacts. A **Kinematic** body is moved by code, by a velocity or by a target, pushes dynamic bodies and is not pushed back. Jolt describes the three under Bodies, Types in its documentation.

A body has a shape, a box, a sphere or a capsule that stands along an axis, and a collider offset and rotation that place the shape inside the body. Sizes are in meters, masses in kilograms, times in seconds, and positions are `WorldPosition`. The shape follows the entity's world scale: a box scales per axis, a sphere by the largest axis, and a capsule by its axis for the height and by the larger of the other two for the radius. The offset scales with it.

A body can be a trigger, which reports contacts and never blocks. Jolt calls it a sensor, and by default it reports dynamic and kinematic bodies and not static ones (Sensors).

A body is on one of 32 layers. The project's layer table says which pairs of layers collide, and a pair of static bodies never collides. Jolt keeps this as an object layer per body (Collision Filtering), here the layer and whether the body moves, and the broad phase keeps its two layers, static and moving (Broad Phase).

**The step.** `PhysicsSystem` runs in `FixedUpdate` at order `kNormal`, once per fixed substep. A script's `OnFixedUpdate` runs before it, at `kEarly`, so a force or a pose a script sets acts on the same step. Each step applies edits to the live bodies, pushes Transform changes to Jolt, runs the step gates, steps Jolt and writes the poses back. `PhysicsInterpolationSystem` runs in `PostUpdate` and draws interpolating bodies between their last two steps.

**When bodies exist.** A body is built the moment a `RigidBody` is added while the simulation is live, and the PlayBegin pass builds the ones already in the scene. They are destroyed with the component, with the entity, or when Stop restores the scene. In Edit mode there are no bodies, so an edit only changes the component and the next Play builds every body from its current fields.

**Transforms.** The simulation is in world space. A body spawns at the entity's resolved world pose, and a result is written back through the parent chain, so a parented body behaves like the same body unparented. A Transform write, or a move of any parent, is found by comparing the resolved pose with the pose physics last placed, and only the fields that changed count. A Dynamic body is teleported to the written value and keeps its own value for the fields nobody wrote. A Kinematic body is moved to the target over the step, so dynamic bodies in the way are pushed, except for its first write after it exists, which teleports it. When a target and a velocity land in one step, the target wins. `teleport` always teleports.

**Edits during Play.** An edit to any `RigidBody` field, from the Inspector, Lua or code, reaches the live body on the next step, in place. The motion type, the shape, the trigger flag, the layer and the properties are set through Jolt's setters, so the handle, the contacts and the `Restrict` lists stay. Velocity follows Jolt on a motion change: it is kept going to Kinematic or Dynamic and dropped going to Static. Jolt wakes nothing by itself, so an edit wakes the bodies around the one it changed.

**Events.** `CollisionEvents` holds last frame's events, one per body pair: `Begin`, `End`, and `Stay` once per frame for a pair that still touches and has a body with `detectStay`. They are drained once per frame in `LateUpdate`.

## API

| Type | Header | Role |
|---|---|---|
| `PhysicsWorld` | `physics/PhysicsWorld.h` | The Jolt world: create and destroy bodies, edit them in place, forces and velocities, ray casts, the layer table, `Step`, the event drain |
| `RigidBody`, `AppliedBody` | `physics/components/RigidBody.h` | The component, and the copy of its fields the live body was last built from |
| `PhysicsSystem` | `physics/PhysicsSystem.h` | The step, the body lifetime hooks, `Teleport` |
| `PhysicsInterpolationSystem` | `physics/PhysicsInterpolationSystem.h` | The blend between the last two steps |
| `PhysicsStepGate`, `PhysicsStepGates` | `physics/PhysicsStepGate.h` | Lets another module act on bodies between the sync and the step |
| `CollisionEvents`, `CollisionEventSystem` | `physics/CollisionEvents.h` | The per-frame event list and its drain |
| `PhysicsLayerTable`, `ReadPhysicsLayers` | `physics/PhysicsLayers.h`, `physics/PhysicsLayerSettings.h` | The 32 layers and the `physicsLayers` section of `project_settings.json` |
| `PhysicsMotion`, `PhysicsShape`, `PhysicsAxis`, `BodyHandle` | `physics/PhysicsTypes.h` | The enums and the handle, free of Jolt |
| `HeightFieldDesc`, `SharedHeightField` | `physics/HeightField.h` | Static height field bodies, built by terrain |

## Components

`RigidBody` is plain data. Every field below is saved, shows in the Inspector, can be read and written from Lua, and is applied to the live body on the next step, except `interpolate`, which the interpolation system reads directly.

| Field | Type | Default | Meaning |
|---|---|---|---|
| `motion` | `PhysicsMotion` | Dynamic | Static, Dynamic or Kinematic |
| `shape` | `PhysicsShape` | Box | Box, Sphere or Capsule |
| `halfExtent` | vec3 | 0.5, 0.5, 0.5 | Box half size per axis |
| `radius` | float | 0.5 | Sphere and capsule radius |
| `halfHeight` | float | 1.0 | Capsule half height, caps included, raised to the radius when smaller |
| `axis` | `PhysicsAxis` | Y | The way a capsule stands |
| `offset` | vec3 | 0 | Where the collider sits in the entity's own axes |
| `rotationOffset` | quat | identity | The collider's rotation in the entity's own axes |
| `interpolate` | bool | false | Draw the body between its last two steps |
| `isTrigger` | bool | false | Report contacts and never block |
| `detectStay` | bool | false | Also report `Stay` for the pairs this body touches |
| `mass` | float | 1 | Kilograms, for a Dynamic body, with the shape giving the inertia |
| `gravityFactor` | float | 1 | Scale on gravity |
| `friction` | float | 0.2 | Contact friction |
| `restitution` | float | 0 | Bounciness, 0 to 1 |
| `allowSleeping` | bool | true | Whether a resting body may fall asleep |
| `layer` | int | 0 | The layer, 0 to 31 |

The `runtime` block is the live state physics keeps. It is not in the field table, so it is never saved or edited, and the `OnAdd` hook clears it so a copy never keeps the original's body handle.

## Rules

**A body is never built while physics steps.** The add hook asserts it, because Jolt's contact callbacks run on job threads and must not add components. Spawn through the `CommandBuffer` instead.

**A size at or below 0 leaves the entity without a body.** The reason and a hint are logged once, on the first attempt only. The entity gets a body when an edit makes the fields buildable, silently, and a second refusal says nothing new. A collider needs a size above 0 on every axis, for a sphere or capsule the radius, and for a capsule the half height.

**A copy starts without a body handle.** Duplicate, a prefab instance and a snapshot restore all copy the component's bytes, so the `OnAdd` hook clears `runtime` first and builds the copy its own body.

**A layer index keeps its meaning.** The table has 32 slots, and removing a name blanks the slot and shifts nothing, so a saved scene still names the layer it meant. A layer outside 0 to 31 is treated as layer 0 and said so once.

**Mass defaults to 1 kg.** A heavy body is a field value, not a size. Existing scenes without the field take the default.

**A Static body can become Dynamic.** Every `RigidBody` body is built allowing it, with the authored mass, so the switch is an in-place edit. Height field bodies are built another way and cannot.

**An edit to a held body waits.** A body held by the terrain release gate is kinematic for the length of the hold, so its edits land once it is resumed.

## Jolt behavior the engine relies on

Cited from the Jolt documentation at https://jrouwe.github.io/JoltPhysics/ by section name.

- **Motion types** (Bodies, Types): static, dynamic and kinematic bodies, and a static body carries no motion properties unless created with `mAllowDynamicOrKinematic`, which is what lets one become dynamic later.
- **Sensors** (Sensors): a sensor reports contacts with dynamic and kinematic bodies and not static ones unless `mCollideKinematicVsNonDynamic` is set. The engine keeps the default.
- **Moving a kinematic body** (`BodyInterface::MoveKinematic`): sets the velocity that reaches the target over the step, and leaves it set, so the engine clears it once the step has reached the target.
- **Sleeping** (Sleeping): a body at rest falls asleep and wakes on contact with an awake body or through an API call, and removing or editing a body wakes nothing, which is why an edit calls `ActivateBodiesInAABox` around it.
- **Layers** (Collision Filtering, Broad Phase): an object layer per body, a pair filter that says which object layers collide, and a map from object layer to broad phase layer.
- **Shapes** (`BodyInterface::SetShape`): replaces a body's shape in place, and with the mass properties updated it returns the shape's own volume mass, so the authored mass is put on again.

## Changed

*2026-10-25*, **Bodies at add time, and `OnFixedUpdate`.** Was: a body was built on the first fixed step, so anything a script did in `OnStart` was lost, and scripts had no callback on the step. Now: a body exists the moment its `RigidBody` is added or Play begins, and `OnFixedUpdate` runs before each step. Because: a body should exist before any script runs.

*2026-10-25*, **World-space simulation.** Was: a body read and wrote the entity's local Transform as if it were world space, so a parented body spawned and simulated in the wrong place, and writing one Transform field of an interpolated body dragged the body to the lagging interpolated pose. Now: the pose comes from the resolved world pose, results are written back through the parents, and only the written fields count. Because: a parented body should behave like the same body unparented.

*2026-10-25*, **Kinematic, capsule, collider offset, triggers, Stay and body properties.** Was: only static and dynamic boxes and spheres with Jolt's defaults for everything else. Now: a Kinematic motion type, a capsule with an axis, a collider offset and rotation, trigger bodies, `Stay` events, and mass, gravity factor, friction, restitution and sleeping as fields. Because: the body API had gaps a game hits at once.

*2026-10-25*, **Edits during Play reach the body.** Was: changing a `RigidBody` field in Play only changed the component. Now: every edit is applied in place on the next step. Because: the Inspector and Lua should show and change what the simulation uses.

*2026-10-25*, **Layers and ray cast filters.** Was: every body collided with every other, and a cast hit its own caster. Now: 32 layers with a collision table in `project_settings.json`, and a cast that filters by layer, motion type, trigger and excluded entities. Because: a game needs to say what passes through what and what a cast is for.

*2026-10-25*, **Input is validated and ignored calls say so.** Was: NaN reached Jolt, a vector written in another form became zero, a zero size logged every step, and a call Jolt ignored still returned true. Now: the bindings refuse bad input with one error, a refused size is logged once, and `PhysicsWorld` returns false and logs when a body ignores a force, torque, impulse or velocity. Because: a Lua return value should be trusted.
