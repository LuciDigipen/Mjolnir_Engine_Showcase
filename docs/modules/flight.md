# flight

The shared fixed-wing flight model: the `Aircraft` component, the `*.aircraft.json` configuration asset and the per-step pipeline that turns controls into forces on a Jolt body. Games use it through a Prefab, a configuration file and Lua; it owns no rendering or UI.

`mir::flight` · depends `mir::core`, `mir::physics`, `mir::input`, `mir::assets` · API `modules/flight/include/flight/` · maintainer Jackey Cen · reviewed 2026-11-18

## Model

An entity becomes an aircraft when it carries `Aircraft` and a `RigidBody`. `Aircraft::config` names a
`*.aircraft.json` under the game's assets root; `FlightModule` wires an `AircraftConfigSystem` into FixedUpdate
at `kEarly`, before `PhysicsSystem`'s edit sync, so the mass properties the config supplies reach Jolt on the
same step. `AircraftConfigStore` loads through `AssetResolver` and `AssetCache` synchronously and decodes each
path once. The store keeps the decoded configs for the module's lifetime, and `AircraftRuntime::config` points
into it.

`FlightStepGate` is the per-step pipeline. It reads the body state before the physics step and skips any
aircraft whose config did not load.

`FlightEnvironment` is a World resource that `FlightModule` emplaces when absent: the sea level, the density
model (ISA by default, or a constant density) and the global wind. The environment stage samples the
atmosphere at the body position, adds the aircraft's `additionalWind` to the global wind and writes the
result into `AircraftRuntime::airData`, then clears `additionalWind`. The gate reads the resource once per
step and uses the defaults when it is missing. The later pipeline stage functions are still empty.

```cpp
mir::FlightModule flight(context, mir::MakeAircraftConfigSource(assets.Cache(), resolver));
```

## API

| Type | Header | Role |
|---|---|---|
| `Aircraft`, `AircraftRuntime`, `AerodynamicTier` | `<flight/components/Aircraft.h>` | component and its transient per-step state |
| `AircraftConfig`, `ParseAircraftConfig`, `AircraftConfigDecoder` | `<flight/AircraftConfig.h>` | the file's values, its parser and the raw-blob decoder |
| `AircraftConfigStore`, `MakeAircraftConfigSource` | `<flight/AircraftConfigStore.h>` | cached decode by path, fed by the asset cache |
| `AircraftConfigSystem`, `WriteMassProperties` | `<flight/AircraftConfigSystem.h>` | FixedUpdate system that writes mass properties |
| `FlightModule` | `<flight/FlightModule.h>` | registers components, the config system and the step gate |
| `FlightStepGate` | `<flight/FlightStepGate.h>` | the before/after step pipeline |
| `FlightEnvironment`, `DensityModel`, `AtmosphereSample`, `SampleAtmosphere` | `<flight/FlightEnvironment.h>` | the World's air resource and the density/sound sample at an altitude |
| `AirData`, `ComputeAirData`, `kAirDataMinSpeed` | `<flight/AirData.h>` | body-frame velocity relative to the air and the quantities derived from it |
| `BodyToLocal`, `LocalToBody`, `InertiaBodyToLocal` | `<flight/FlightFrames.h>` | the one body/local conversion |
| `AngleOfAttack`, `Sideslip` | `<flight/FlightFrames.h>` | air-data angles from body velocity |

## Rules

- Every body-frame to engine-local conversion goes through `FlightFrames.h`. The axes are fixed: body X nose,
  Y right wing, Z belly; engine local X right, Y up, Z back.
- A config path is relative to the assets root and stays a fixed `char[kFieldStringCapacity]` in the component,
  so `Aircraft` remains trivially copyable and saveable; `runtime` is transient.
- The config system runs only while simulating, so the derived mass properties never reach the authored scene;
  the Play snapshot restores the authored `RigidBody` on Stop.
- The config system only writes a `RigidBody` when the path differs from `AircraftRuntime::appliedConfig`.
  A failed load is cached, logged once, leaves the body untouched and sets `runtime.config` to null; the gate
  then skips that aircraft.
- `WriteMassProperties` never changes the motion type. It sets mass, the rotated custom inertia tensor, the
  rotated custom centre of mass, zero damping, the gyroscopic force and `LinearCast`.
- `FlightEnvironment::wind` and `AircraftRuntime::additionalWind` are the velocity of the air in the world
  frame, not the direction it blows from. The gate computes with their sum and clears `additionalWind` after
  reading it, so a script or system must write it again every fixed step.
- Below `kAirDataMinSpeed` the flow direction is undefined, so `angleOfAttack` and `sideslip` read 0 while
  `trueAirspeed`, `dynamicPressure` and `mach` still follow the speed.
- `SampleAtmosphere` reports the true altitude `worldY - seaLevelY` and clamps it to the model's range only
  inside the ISA formula. ISA is the troposphere lapse up to the tropopause and an isothermal layer above;
  the constant model returns `constantDensity` and a fixed speed of sound at every altitude.
- Products of inertia in the file use the `RigidBody` convention: the file holds `Ixy, Ixz, Iyz` and the tensor
  holds their negatives. The parser rejects non-finite values and reports every error as `<json path>: <reason>`.

## State

*As of 2026-11-18.* The configuration asset, parser, decoder, cached store, mass-property system, environment
resource and air data work. `FlightStepGate` is attached and reads state and air data, but the actuator,
propulsion, gear and aerodynamic stages are stubs. There is no hot reload yet; the store is cleared only with
the module.

Tests in `modules/flight/tests/`: `AircraftConfig` (full file, every error class, decoder tag, store caching,
mass-property rotation, an aircraft through one fixed step), `AirData` (wind and updraft, crosswind, rotated
aircraft, rest and reverse flow, ISA and constant density, the additional wind through a fixed step),
`FlightFrames` (axis mapping, round trip, gravity, air angles) and `FlightStepGate` (control-moment signs,
state from the step, detach).

## Configuration format

### Conventions

- Body frame: X forward (nose), Y right wing, Z down (belly). Right-handed. All positions/directions in the file
  are body frame, metres, measured from the entity origin (the model origin).
- Engine local axes: -Z forward, +X right, +Y up. Body (X,Y,Z) -> local (-Z, +X, -Y). One adapter in
  `modules/flight` (`BodyToLocal` / `LocalToBody`) does every conversion.
- SI units. Angles in the file are degrees and the key ends in `Deg` (or `DegPerSec`); the loader converts to
  radians. Everything inside the code is radians.
- Curves are arrays of `[x, y]` pairs with strictly increasing x, linearly interpolated, clamped at the ends.
- Validation errors name the JSON path and the reason, e.g. `engines[0].rpmTau: must be > 0`.

### Top level

| Key | Type | Owner | Meaning |
|---|---|---|---|
| `version` | int | #449 | must be 1 |
| `mass` | number > 0 | #449 | kg |
| `inertia.diagonal` | [Ixx, Iyy, Izz] > 0 | #449 | kg m^2, body frame about the centre of mass |
| `inertia.products` | [Ixy, Ixz, Iyz] | #449 | kg m^2, body frame; optional, default 0; products of inertia as `RigidBody` takes them (the tensor holds -Ixy, -Ixz, -Iyz) |
| `centerOfMass` | vec3 | #449 | body frame |
| `reference.area` / `span` / `chord` | number > 0 | #449 | S (m^2), b (m), mean chord c̄ (m) |
| `throttleRate` | number > 0 | #451 | throttle change per second while held |
| `actuators` | object | #451 | see below |
| `engines` | array (may be empty) | #454 | empty = glider |
| `gear` | array (may be empty) | #323 | raycast gear points |
| `camera.eye` | vec3 | #449 (read by #457) | cockpit eye point |
| `camera.chaseDistance` / `chaseHeight` | number >= 0 | #449 (read by #457) | chase camera offset behind/above |
| `stallWarningMarginDeg` | number >= 0 | #455 | warn when alpha is within this margin of the critical alpha |
| `nodes` | object string -> string | #449 | component name -> model node name; recorded only |
| `aero.L1` / `aero.L2` / `aero.L3` | object | #452 / #453 / #462 | per-tier aerodynamic profiles |

### `actuators` (#451)

```json
"actuators": {
  "elevator": { "maxDeflectionDeg": 25, "rateDegPerSec": 60, "delay": 0.05 },
  "aileron":  { "maxDeflectionDeg": 20, "rateDegPerSec": 80, "delay": 0.05 },
  "rudder":   { "maxDeflectionDeg": 25, "rateDegPerSec": 60, "delay": 0.05 },
  "flaps":    { "detentsDeg": [0, 10, 20, 30], "rateDegPerSec": 5 },
  "trim":     { "stepDeg": 0.5, "maxDeg": 10 }
}
```
- `delay` (s) is a first-order lag time constant on the command before the rate limit; 0 = none.
- Trim adds to the elevator command (as a deflection), clamped to +-maxDeg; total stays within maxDeflection.
- Sign convention (simplified for the demo, differs from the NASA elevator/rudder convention): a positive
  deflection produces a positive body moment by the right-hand rule. Positive elevator = nose up (+My about +Y),
  positive aileron = right wing down (+Mx about +X), positive rudder = nose right (+Mz about +Z down).
  So every control gain/derivative (Ke, Cm_de, Cl_da, Cn_dr) is positive. Stick back = positive pitch command,
  stick right = positive roll command, right rudder (R2 / E) = positive yaw command.

### `engines[]` (#454)

```json
{ "position": [1.5, 0, 0], "direction": [1, 0, 0], "idleRpm": 700, "maxRpm": 2700, "rpmTau": 0.8,
  "staticThrust": [[0, 0], [1, 3200]], "speedFactor": [[0, 1.0], [60, 0.8], [120, 0.45]] }
```
- Target RPM = idleRpm + throttle * (maxRpm - idleRpm); RPM follows with time constant rpmTau.
- Thrust = staticThrust(rpm / maxRpm) * speedFactor(axial airspeed m/s) * (rho / 1.225), along `direction`
  (normalized) at `position`.

### `gear[]` (#323)

```json
{ "name": "nose", "mount": [2.0, 0, 0.6], "restLength": 0.6, "stiffness": 30000, "damping": 3000,
  "rollingFriction": 0.03, "sideFriction": 0.9, "brake": 0.0, "maxSteerDeg": 30 }
```
- Ray from `mount` along body +Z (down) of length `restLength` (+ a small margin).
- `brake` is a friction coefficient applied while Brake is held (0 = no brake on this wheel).
- `maxSteerDeg` > 0 makes it steerable by the rudder command.

### `aero.L1` (#452)

```json
{ "CL": 0.4, "CD": 0.04, "Ka": 3.0, "Ke": 4.0, "Kr": 2.0, "Kp": 2000, "Kq": 4000, "Kr_damp": 2000 }
```
Owner issue may refine the key names; it must update this file.

### `aero.L2` (#453) and `aero.L3` (#462)

Defined by their issues; each must add its section here.
