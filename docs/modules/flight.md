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
step and uses the defaults when it is missing. The actuator stage consumes the queued commands and the held
controls; the aerodynamics stage evaluates the selected tier's profile into a body-frame `ForceMoment` and adds
it to the accumulator. The propulsion and gear stages are still empty. A tier without a profile in the config
adds nothing and warns once per aircraft config. L2 is the default playable model; `Aircraft::tier` defaults to
it.

`FlightInputSystem` runs in Update, after PreUpdate's `InputSystem` and before the frame's fixed steps. For
every `Aircraft` with `playerInput` it copies the pitch, roll, yaw, throttle and brake actions into
`AircraftRuntime::controls` and appends one command per fresh flap or trim press to
`AircraftRuntime::commands`. The actuator stage drains that queue on the first fixed step after the frame, so
a frame without a fixed step keeps its commands for the next one.

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
| `FlightModule` | `<flight/FlightModule.h>` | registers components, the input system, the config system and the step gate |
| `FlightStepGate` | `<flight/FlightStepGate.h>` | the before/after step pipeline |
| `FlightInput`, `AddFlightDefaultActions` | `<flight/FlightInput.h>` | the flight action names and their default keyboard and gamepad bindings |
| `FlightInputSystem` | `<flight/FlightInputSystem.h>` | Update system that reads the flight actions and queues discrete commands |
| `StepActuators` | `<flight/FlightActuators.h>` | the pure actuator stage over an `ActuatorState` |
| `FlightControls`, `FlightCommandQueue`, `ActuatorState` | `<flight/AircraftRuntime.h>` | held controls, the discrete-command FIFO and the actuator state |
| `FlightEnvironment`, `DensityModel`, `AtmosphereSample`, `SampleAtmosphere` | `<flight/FlightEnvironment.h>` | the World's air resource and the density/sound sample at an altitude |
| `AirData`, `ComputeAirData`, `kAirDataMinSpeed` | `<flight/AirData.h>` | body-frame velocity relative to the air and the quantities derived from it |
| `AeroInput`, `ForceMoment`, `AerodynamicProfile`, `L1Profile`, `L2Profile` | `<flight/AerodynamicModel.h>` | the model's input, its body-frame output and the per-tier coefficients |
| `IAerodynamicModel`, `AerodynamicModelFor`, `WindAxesToBody` | `<flight/AerodynamicModel.h>` | the stateless model interface, the tier lookup and the wind-axis conversion |
| `L2StallFraction`, `L2CriticalAngleOfAttack` | `<flight/AerodynamicModel.h>` | the post-stall blend weight and the positive critical angle of attack |
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
- The aerodynamic stage builds an `AeroInput` from the step's air data, body rates, actual actuator deflections
  and the config's reference area, span and chord, then evaluates `AerodynamicModelFor(Aircraft::tier)` and
  stores the result in `AircraftRuntime::aero`. It only adds force and moment to the accumulator; it never
  writes position, velocity, attitude or angular velocity.
- `WindAxesToBody` is the one wind-axis conversion: drag opposes the relative velocity, lift is perpendicular to
  it in the body X-Z plane (`alpha = 0` gives body -Z), side force is body +Y. All three are zero below
  `kAirDataMinSpeed`.
- L1 gains use the normalised deflection (actual over maximum), so `Ka`, `Ke` and `Kr` are moments per pascal
  and stay independent of surface travel. Damping gains act on the body rates and do not depend on airspeed.
- L2 blends the linear coefficients into the post-stall table with a C1-continuous smoothstep of width
  `blendDeg` after each critical alpha. `qbar` uses the true airspeed and the rate normalisation uses
  `max(V, minAirspeed)`, so the force is zero at rest and the damping stays bounded at low speed. Post-stall
  scales the control derivatives by `controlScale` and `Cmalpha`, `Clbeta`, `Cnbeta` by `stabilityScale`.
- A tier with no profile in the config adds no aerodynamic force and warns once per aircraft config; L3
  stays zero until its issue lands.
- `SampleAtmosphere` reports the true altitude `worldY - seaLevelY` and clamps it to the model's range only
  inside the ISA formula. ISA is the troposphere lapse up to the tropopause and an isothermal layer above;
  the constant model returns `constantDensity` and a fixed speed of sound at every altitude.
- Products of inertia in the file use the `RigidBody` convention: the file holds `Ixy, Ixz, Iyz` and the tensor
  holds their negatives. The parser rejects non-finite values and reports every error as `<json path>: <reason>`.
- `aero` and `aero.L1` are optional. When `aero.L1` is present every coefficient is required and `CD`, `Ka`,
  `Ke`, `Kr`, `Kp`, `Kq`, `Kn` must be `>= 0`; a negative or missing value is reported with its path.
- `FlightInputSystem` only writes aircraft with `playerInput`; input never reaches an AI or parked aircraft.
  A frame's analog controls are the latest values, and its fresh presses are appended to a 16-command FIFO in
  action-table order: `FlightFlapsUp`, `FlightFlapsDown`, `FlightTrimUp`, `FlightTrimDown`. Further presses that
  frame are dropped.
- The actuator stage drains the FIFO on the first fixed step after the frame and applies the commands in order:
  flaps move the detent index by -1/+1 inside its list, trim adds +/-`stepDeg` clamped to +/-`maxDeg`, then
  throttle changes by `(up - down) * throttleRate * dt` clamped to 0..1.
- A surface target is the command times `maxDeflectionDeg`; the elevator target adds trim and is clamped to its
  own maximum. The command passes a first-order lag with time constant `delay` (exact `1 - exp(-dt/delay)`,
  0 = pass through), then the actual deflection moves toward the lagged value by at most `rateDegPerSec * dt`.
  Flaps move toward `detentsDeg[flapDetent]` at their own rate.

## State

*As of 2026-11-18.* The configuration asset, parser, decoder, cached store, mass-property system, environment
resource, air data, actuator stage and the L1 and L2 aerodynamic models work. L2 is the default tier and includes
the stall blend and flap table. `FlightStepGate` is attached, reads state and air data, and adds the selected
tier's aerodynamic force and moment. The propulsion and gear stages are stubs; L3 yields zero with a warning
until its issue lands. There is no hot reload yet; the store is cleared only with the module.

Tests in `modules/flight/tests/`: `AircraftConfig` (full file, every error class, decoder tag, store caching,
mass-property rotation, an aircraft through one fixed step), `AirData` (wind and updraft, crosswind, rotated
aircraft, rest and reverse flow, ISA and constant density, the additional wind through a fixed step),
`FlightFrames` (axis mapping, round trip, gravity, air angles), `FlightInput` (action names and types, default
bindings, idempotent merge, a rebound action kept), `FlightActuators` (rate limit and settle, lag, trim and flap
commands in order, throttle, surface signs, a frame with no fixed step, input through the physics fixture),
`Aerodynamics` (wind-axis directions, determinism, dynamic-pressure scaling, control and damping signs, sources,
missing profiles, an L1 aircraft through the physics fixture), `AerodynamicsL2` (stall continuity, flap lift,
trim and control signs, adverse yaw, post-stall scaling, the low-speed bound, zero-airspeed rest, determinism,
`aero.L2` parsing and its error paths) and `FlightStepGate` (control-moment signs, state from the step, detach).

## Flight input

| Action | Type | Gamepad | Keyboard |
|---|---|---|---|
| `FlightPitch` | Axis1D | Left stick Y | S +1, W -1 |
| `FlightRoll` | Axis1D | Left stick X | D +1, A -1 |
| `FlightYaw` | Axis1D | Right trigger +1, left trigger -1 | E +1, Q -1 |
| `FlightThrottleUp` | Button | A | R |
| `FlightThrottleDown` | Button | B | F |
| `FlightBrake` | Button | X | B |
| `FlightFlapsUp` | Button | D-pad up | Left bracket |
| `FlightFlapsDown` | Button | D-pad down | Right bracket |
| `FlightTrimUp` | Button | D-pad left | T |
| `FlightTrimDown` | Button | D-pad right | G |
| `FlightCameraToggle` | Button | Y | V |

Triggers are remapped in the input module to 0 released and 1 fully pressed before the deadzone, and their
`scale` applies, so one signed yaw axis can use both. Values are positive for stick back, stick right and
right rudder.

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

`throttleRate` and `actuators` are required. Angles are degrees here and radians inside the code.

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

Optional. Every key is required when `aero.L1` is present; `CD` and the four `K` gains must be `>= 0`.

| Key | Type | Meaning |
|---|---|---|
| `CL` | number | lift coefficient |
| `CD` | number >= 0 | drag coefficient |
| `Ka` | number >= 0 | aileron control moment per pascal and unit normalised deflection |
| `Ke` | number >= 0 | elevator control moment per pascal and unit normalised deflection |
| `Kr` | number >= 0 | rudder control moment per pascal and unit normalised deflection |
| `Kp` | number >= 0 | roll damping moment per rad/s |
| `Kq` | number >= 0 | pitch damping moment per rad/s |
| `Kn` | number >= 0 | yaw damping moment per rad/s |

```json
{ "CL": 0.4, "CD": 0.04, "Ka": 3.0, "Ke": 4.0, "Kr": 2.0, "Kp": 2000, "Kq": 4000, "Kn": 2000 }
```

Lift `L = q S CL` acts along the wind-axis lift direction and drag `D = q S CD` along the relative velocity;
both act at the centre of mass, so L1 has no stability moment. The control and damping moments are

- `Mx = q Ka da - Kp p`
- `My = q Ke de - Kq q`
- `Mz = q Kr dr - Kn r`

with the normalised deflection `d* = actual deflection / max deflection` in -1..1 and `p, q, r` the body angular
velocity. The control terms use the sign convention in `actuators` above, so a positive elevator, aileron or
rudder gives a positive moment about its body axis. A tier without a profile in the file yields no force or
moment and the gate warns once.

### `aero.L2` (#453)

Optional. The default playable model (`Aircraft::tier` defaults to L2). Every key is required when `aero.L2`
is present; `CD0` and `k` must be `>= 0`, `minAirspeed` must be `> 0`, and angles in the file are degrees.

| Key | Type | Meaning |
|---|---|---|
| `CL0` | number | lift coefficient at zero alpha |
| `CLalpha` | number | lift slope per radian |
| `CD0` | number >= 0 | parasite drag |
| `k` | number >= 0 | induced drag factor, `CD = CD0 + k CL^2` |
| `Cm0` | number | pitching moment at zero alpha |
| `Cmalpha` | number | pitch stiffness per radian, negative is stable |
| `Cmq` | number | pitch damping per normalised pitch rate, negative is stable |
| `Cmde` | number | elevator control moment per radian |
| `CYbeta` | number | side force per radian of beta |
| `CYdr` | number | side force per radian of rudder |
| `Clbeta` | number | roll moment from beta per radian, negative is stable |
| `Clp` | number | roll damping per normalised roll rate, negative is stable |
| `Clda` | number | aileron control moment per radian |
| `Cnbeta` | number | yaw moment from beta per radian, positive is stable |
| `Cnr` | number | yaw damping per normalised yaw rate, negative is stable |
| `Cndr` | number | rudder control moment per radian |
| `Cnda` | number | yaw moment from aileron per radian; negative is adverse yaw |
| `minAirspeed` | number > 0 | m/s, lower bound of the rate normalisation |
| `stall.positiveDeg` | number > 0 | positive critical alpha before the flap shift |
| `stall.negativeDeg` | number < 0 | negative critical alpha |
| `stall.blendDeg` | number > 0 | width of the smooth transition after each critical alpha |
| `stall.controlScale` | number 0..1 | control derivative multiplier when fully stalled |
| `stall.stabilityScale` | number 0..1 | multiplier for `Cmalpha`, `Clbeta` and `Cnbeta` when fully stalled |
| `stall.postStall` | array | `[alphaDeg, CL, CD, Cm]` rows, at least 2, strictly increasing alpha; must cover `[negativeDeg, positiveDeg + blendDeg]` |
| `flaps` | array | `[flapDeg, dCL, dCD, dCm, stallShiftDeg]` rows, at least 1, strictly increasing flap angle; the first row may be 0 |

```json
"aero": { "L2": {
  "CL0": 0.25, "CLalpha": 5.0, "CD0": 0.03, "k": 0.05,
  "Cm0": 0.05, "Cmalpha": -0.6, "Cmq": -12, "Cmde": 1.2,
  "CYbeta": -0.4, "CYdr": -0.1, "Clbeta": -0.1, "Clp": -0.5, "Clda": 0.08,
  "Cnbeta": 0.12, "Cnr": -0.15, "Cndr": 0.08, "Cnda": -0.02,
  "minAirspeed": 10,
  "stall": {
    "positiveDeg": 15, "negativeDeg": -15, "blendDeg": 6,
    "controlScale": 0.4, "stabilityScale": 0.3,
    "postStall": [[-15, -0.9, 0.2, -0.15], [0, 0.25, 0.05, 0.05],
                  [15, 1.35, 0.16, -0.1], [21, 1.02, 0.24, -0.13]]
  },
  "flaps": [[0, 0, 0, 0, 0], [10, 0.25, 0.005, -0.01, -0.5],
            [20, 0.5, 0.012, -0.02, -1], [30, 0.7, 0.02, -0.03, -1]]
} }
```

The flap row is interpolated linearly at the actual flap angle and clamped at both ends. With the
interpolated `dCL`, `dCD`, `dCm` and `stallShift`:

- `CLlin = CL0 + CLalpha alpha + dCL`, `CDlin = CD0 + k CLlin^2 + dCD`,
  `Cmlin = Cm0 + Cmalpha_eff alpha + dCm`
- `alphaPos = positiveDeg + stallShift`, `alphaNeg = negativeDeg`
- `w = max(smoothstep(alphaPos, alphaPos + blend, alpha), smoothstep(-alphaNeg, -alphaNeg + blend, -alpha))`
- `CL = lerp(CLlin, CLpost(alpha), w)`, and the same blend for `CD` and the static `Cm`; the post-stall
  table is interpolated linearly in alpha and clamped at its ends
- `ctl = lerp(1, controlScale, w)`, and `Cmalpha`, `Clbeta` and `Cnbeta` use `lerp(1, stabilityScale, w)`

smoothstep is `t^2 (3 - 2t)` on the clamped `t = (x - edge0) / (edge1 - edge0)`, so `CL`, `CD` and `Cm`
are C1-continuous through the transition. `L2StallFraction` and `L2CriticalAngleOfAttack` expose `w` and
`alphaPos` for telemetry and the stall warning.

The forces and moments use `qbar = 0.5 rho V^2` with the true `V`; the rates are normalised with
`Veff = max(V, minAirspeed)`. The model yields nothing below `kAirDataMinSpeed`.

- `Y = qbar S (CYbeta beta + CYdr dr)`
- `Mx = qbar S b (Clbeta_eff beta + Clp p_hat + ctl Clda da)`
- `My = qbar S c (Cm_static + Cmq q_hat + ctl Cmde de)`
- `Mz = qbar S b (Cnbeta_eff beta + Cnr r_hat + ctl (Cndr dr + Cnda da))`

with `p_hat = p b / (2 Veff)`, `q_hat = q c / (2 Veff)`, `r_hat = r b / (2 Veff)` and `da`, `de`, `dr`
the actual deflections in radians. The wind-axis helper carries lift, drag and side force; the control
terms form `controlMoment`, the rate terms `dampingMoment` and the rest `stabilityMoment`.

Sign notes under the `actuators` convention: `Cmalpha < 0`, `Cnbeta > 0` and `Clbeta < 0` are stable;
`Cmde`, `Clda` and `Cndr` are positive; `Cnda < 0` is adverse yaw; `CYdr` is negative for a conventional
tail. `Clp`, `Cmq` and `Cnr` are negative for damping.

### `aero.L3` (#462)

Defined by its issue; it must add its section here.
