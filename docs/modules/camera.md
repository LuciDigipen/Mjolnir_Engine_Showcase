# camera

Gameplay camera control: moves an entity's `Transform` by following a target or
by free-fly input. It does not own the lens or draw anything; `Camera` and
`RenderSystem` stay in `renderer`.

`mir::camera` · depends `mir::core`, `mir::input` (private) · API `modules/camera/include/camera/` · maintainer Quy Dong Ta · reviewed 2026-10-15

## Model

A `CameraController` sits on the same root entity as a `Camera`.
`CameraModule` registers `CameraControllerSystem` in `LateUpdate`, after
`Propagate`, so a follow target's `WorldTransform` is this frame's, including a `RigidBody` that `PhysicsInterpolationSystem` blended just
before. After writing the camera's `Transform` it calls `ResolveWorld` on the
camera, because propagation has already run and `RenderSystem` reads the cache.
It runs at `kEarly`, before the other `LateUpdate` systems that read that cache,
such as `AudioListenerSystem`.

`mode` picks one of two behaviours:

- **Follow** chases `target` at `offset`, closing `1 - e^(-stiffness * dt)` of the
  gap each frame, and optionally looks at the target. Runs only while simulating.
- **FreeFly** moves from the `Camera*` input actions in `input/DefaultInputMap.h`
  at `moveSpeed` (times `fastMultiplier` while `CameraFast` is held), turning by
  `lookSensitivity` degrees per pixel while `CameraLookHold` is held and by
  `stickLookRate` degrees per second from the right stick. Runs during Play,
  paused or not, so a frozen frame can be inspected.

`offsetInTargetSpace` turns the offset with the target's rotation. With
`offsetHeadingOnly` the offset follows only the target's yaw, so a rolling or
pitching target does not swing the camera around it; it applies together with
`offsetInTargetSpace`.

`ToggleCameraMode` (F2, gamepad Back) flips every controller between the two.
Setting `mode` from the Inspector or Lua works the same.

Free-fly's look direction is the hidden `yawDegrees` and `pitchDegrees` fields,
so a script can aim a free-fly camera by writing them; pitch is clamped to
±`kFreeFlyMaxPitchDegrees`. Writing the `Transform`'s rotation instead is
overwritten on the next frame.

## API

| Type | Header | Role |
|---|---|---|
| `CameraController`, `CameraMode`, `kFreeFlyMaxPitchDegrees` | `camera/components/CameraController.h` | Mode, follow and free-fly settings, plus runtime state; of that, only free-fly yaw and pitch are fields |
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

*2026-10-25* — **Free-fly yaw and pitch are fields (#219).** Was: they were
runtime state with no field, so a script's write to the `Transform`'s rotation
was overwritten next frame and nothing could aim a free-fly camera. Now:
`yawDegrees` and `pitchDegrees` are hidden fields, pitch clamped to the same
limit the step uses. Because: scripted cutscene hand-offs and respawn look-ats.
Entering FreeFly still re-seeds both from the rotation.

*2026-10-15* — **Module created.** Was: `CameraFollow` lived in `renderer`.
Now: `CameraController` here, with a free-fly mode. Because: issue #28's
follow-up, which reads input.
