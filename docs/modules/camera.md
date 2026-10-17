# camera

Gameplay camera control: moves an entity's `Transform` by following a target or
by free-fly input. It does not own the lens or draw anything; `Camera` and
`RenderSystem` stay in `renderer`.

`mir::camera` · depends `mir::core`, `mir::input` (private) · API `modules/camera/include/camera/` · maintainer Quy Dong Ta · reviewed 2026-10-15

## Model

A `CameraController` sits on the same root entity as a `Camera`.
`CameraControllerSystem` runs in `PostUpdate` right after
`TransformPropagateSystem`, so a follow target's `WorldTransform` is this
frame's, including a `RigidBody` that `PhysicsInterpolationSystem` blended just
before. After writing the camera's `Transform` it calls `ResolveWorld` on the
camera, because propagation has already run and `RenderSystem` reads the cache.

`mode` picks one of two behaviours:

- **Follow** chases `target` at `offset`, closing `1 - e^(-stiffness * dt)` of the
  gap each frame, and optionally looks at the target. Runs only while simulating.
- **FreeFly** moves from the `Camera*` input actions in `input/DefaultInputMap.h`
  at `moveSpeed` (times `fastMultiplier` while `CameraFast` is held), turning by
  `lookSensitivity` degrees per pixel while `CameraLookHold` is held and by
  `stickLookRate` degrees per second from the right stick. Runs during Play,
  paused or not, so a frozen frame can be inspected.

`ToggleCameraMode` (F2, gamepad Back) flips every controller between the two.
Setting `mode` from the Inspector or Lua works the same.

```cpp
mScheduler.AddSystem<TransformPropagateSystem>(SystemPhase::PostUpdate);
mScheduler.AddSystem<CameraControllerSystem>(SystemPhase::PostUpdate);
```

## API

| Type | Header | Role |
|---|---|---|
| `CameraController`, `CameraMode` | `camera/components/CameraController.h` | Mode, follow and free-fly settings, plus runtime state that is not saved |
| `CameraControllerSystem` | `camera/CameraControllerSystem.h` | The per-frame driver |
| `StepCameraFollow`, `CameraFollowGoal`, `SeedFreeFly`, `StepFreeFly` | `camera/CameraControllerSystem.h` | The pure steps the system runs, for tests |
| `RegisterCameraComponents` | `camera/ComponentRegistration.h` | Registers `CameraController` |

## Rules

**A switch never resets the camera.** Entering FreeFly takes yaw and pitch from
the current rotation (roll is dropped). Entering Follow chases from where the
camera is, ignoring `snapDistance` until it is back within it, so flying far
away and toggling back does not teleport.

**Root entities only.** `Transform` is parent-local on a child and the math is
absolute, so a parented controller logs once and does nothing.

**Input goes through `ForGame`.** An open game UI, such as the pause menu,
freezes free-fly. Mouse look reads the `Delta2D` action `CameraLook`, which is
the cursor's movement in pixels and is not clamped like the axis types.

## State

*As of 2026-10-15.* Follow and FreeFly work, with tests in
`modules/camera/tests/CameraController.tests.cpp` covering both steps, the
switch in each direction, pause, and the game-UI hold. HelloWorld's camera
follows the scripted `FollowRig`.

## Backlog

- Scroll to change free-fly speed, as `EditorCamera` does; scroll is not bindable yet.
- An entity picker for `target`; the Inspector shows `EntityRef` read-only.
- Cursor capture for look without holding a button.

## Changed

*2026-10-15* — **Module created.** Was: `CameraFollow` lived in `renderer`.
Now: `CameraController` here, with a free-fly mode. Because: issue #28's
follow-up, which reads input.
