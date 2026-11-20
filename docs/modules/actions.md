# actions

Eases an entity's position, rotation and scale toward a goal over time, and
eases named values that have no entity, such as a menu's fade. One World
resource holds every action, so any system, screen or script reaches it through
the World, and no object needs an action list of its own or a reference to one.
It does not run physics, animate models or draw anything.

`mir::actions` · depends `mir::core` (public) · API `modules/actions/include/actions/` · maintainer Quy Dong Ta · review pending

## Model

`ActionScheduler` is a World resource that `ActionsModule` emplaces. A call
such as `MoveTo(entity, goal, params)` appends one action and returns an
`ActionHandle`. Actions are plain values in one vector, run in the order they
were added. Nothing is allocated per action and nothing is virtual.

Every action runs in a **lane**. An entity's actions run in that entity's lane
(`ActionLane(entity)`), and a value's tweens run in the lane named after its key
(`ActionLane("Pause.alpha")`), unless `ActionParams::mLane` names another. A
**blocking** action holds back every action after it in its lane until it ends,
during its delay as well. Actions in other lanes keep running. That gives one
entity a sequence while a second entity, or a named lane such as a cutscene,
runs alongside it.

There are two **clocks**. `ActionClock::Game` actions advance in
`ActionSystem`, in `SystemPhase::Update`, only while `IsSimulating`. They stop
in Edit and while the game is paused. `ActionClock::UI` actions advance at
`FrameSlot::FrameBegin` with the frame's `dt`, in every mode, so a pause menu
keeps animating while the game is paused.

An action reads its start value when its delay ends, not when it is added, so a
queued move starts from wherever the entity is by then. A goal is absolute. A
target that dies, or loses its `Transform`, ends its action. When it ends,
nothing is written.

**Values** are four floats per key. `TweenValue` eases the first component and
`TweenVec4` eases all four. A key never set holds 0. Game UI is immediate mode
and keeps nothing between frames, so a screen reads its animated numbers back
each frame:

```cpp
ActionScheduler &actions = world.Resource<ActionScheduler>();

// when the screen opens
actions.SetValue("Pause.alpha", 0.0f);
actions.TweenValue("Pause.alpha", 1.0f, { .mDuration = 0.25f, .mEase = Ease::Out, .mClock = ActionClock::UI });
actions.SetValue("Pause.slide", -40.0f);
actions.TweenValue("Pause.slide", 0.0f, { .mDuration = 0.25f, .mEase = Ease::Out, .mClock = ActionClock::UI });

// every frame
ui.DrawList().PushOpacity(actions.Value("Pause.alpha", 1.0f));
UIRect resume = layout.mResume;
resume.x += actions.Value("Pause.slide");
Button(ui, UIId("Resume"), resume, "Resume");
ui.DrawList().PopOpacity();
```

Offset a widget by changing the rect you pass it, never by moving only what is
drawn. Hit testing reads the same rect, so the click follows the widget.

## API

| Type | Header | Role |
|---|---|---|
| `ActionScheduler` | `actions/ActionScheduler.h` | The resource: entity actions, value tweens, lanes, clocks, cancelling |
| `ActionParams`, `ActionHandle`, `ActionClock`, `ActionLane` | `actions/ActionScheduler.h` | Duration, delay, ease, clock, blocking and lane of one action, its handle, and the lane ids |
| `Ease`, `ApplyEase`, `EaseName`, `EaseFromName` | `actions/Ease.h` | The curves and their names |
| `ActionSystem` | `actions/ActionSystem.h` | Ticks the Game clock in `Update` |
| `ActionsModule` | `actions/ActionsModule.h` | The resource, the system, the UI clock at `FrameBegin`, and the clear at `PlayEnd` |

`In` starts slow and `Out` ends slow. `StrongIn` and `StrongOut` are the same
curves, steeper. `Back` overshoots past the goal and settles, and `Bounce`
bounces into it. Every curve is 0 at the start and 1 at the end.

### Lua

`actions_script` adds these to the `World` usertype. Handles are integers. A
call that is refused logs one error and returns `nil` (or `false`).

| Call | Returns | Effect |
|---|---|---|
| `move_to(entity, position, options)` | handle | Eases `Transform` position to `position` |
| `rotate_to(entity, rotation, options)` | handle | Eases rotation to `{ x, y, z, w }` |
| `scale_to(entity, scale, options)` | handle | Eases scale to `{ x, y, z }` |
| `tween_value(name, goal, options)` | handle | Eases the named value to `goal` |
| `set_value(name, value)`, `value(name[, fallback])` | bool, number | Sets or reads a named value, `fallback` (default 0) when unset |
| `action_running(handle)`, `cancel_action(handle)` | bool, nothing | Asks about or stops one action |
| `cancel_actions(entity)`, `cancel_value(name)`, `cancel_lane(lane)` | nothing | Stops everything on that target or in that lane |
| `lane_busy(lane)`, `set_lane_paused(lane, paused)`, `set_lane_time_scale(lane, scale)` | bool | A lane is a name, or an entity for its own lane |
| `set_action_time_scale(scale[, clock])` | bool | Scales a whole clock, `"game"` by default |

`options` is optional: `duration` (seconds, default 1), `delay` (seconds),
`ease` (a name from `Ease`, such as `"Out"`), `blocking` (boolean), `lane` (a
name or an entity) and `clock` (`"game"` or `"ui"`). An unknown key is refused
with an error that lists the valid ones.

```lua
function Door.OnStart(self, world, entity)
    world:move_to(entity, { x = 0, y = 3, z = 0 }, { duration = 0.5, ease = "InOut", blocking = true })
    world:scale_to(entity, { x = 1, y = 0.1, z = 1 }, { duration = 0.2 })   -- after the move
end
```

## Rules

**Blocking holds back only later actions in the same lane on the same clock.**
An action added before a blocking one keeps running. A Game action never blocks
a UI action, even in the same lane, because the two clocks tick at different
times in the frame.

**The next action in a lane starts in the tick the blocking one ends, with that
whole tick.** Time left over from the finished action is not handed on, so a
long chain drifts by up to a frame per step. When steps must line up exactly,
queue them without blocking and give each a delay instead.

**Write a widget's rect, not its drawing.** `gameui` hit tests the rect a
widget is given. Moving only the draw commands would leave the widget clickable
where it used to be.

**A faded widget still takes the pointer.** `PushOpacity(0)` hides a widget
but does not disable it. Skip the widget, or pass `UIWidgetOptions` that
disable it, while it is hidden.

**PlayEnd clears everything.** Actions, values, lane settings and time scales
all go when Play stops, so nothing queued in Play reaches Edit. A Play session
also has its own World, so its scheduler goes with it.

**Lanes are hashes.** `ActionLane(name)` is FNV-1a and `ActionLane(entity)`
mixes the packed entity, so a name and an entity can in principle collide. At
64 bits that is not a practical concern, but it is why lanes are not small
integers.

Errors: none fatal. A refused call returns an invalid handle and logs one error
naming the call and what was wrong.

## State

*As of 2026-11-19.* Entity moves, rotations and scales, scalar and four-float
value tweens, lanes with blocking, pause and time scale per lane and per clock,
and the Lua bindings all work. `ActionScheduler.tests.cpp`, `Ease.tests.cpp`
and `ActionsModule.tests.cpp` cover the scheduler, the curves and which clock
runs in which mode. `ActionBindings.tests.cpp` in `actions_script` covers the
Lua calls. Nothing in the engine's own screens uses it yet.

## Backlog

1. A completion callback, or an event a script can poll, so a sequence can
   start the next step without checking `action_running` every frame.
2. Relative goals (`move_by`) if scripts keep computing `position + offset`.
3. Rotation in `gameui`: a rotation on `Image` and `Sprite` commands, drawn as
   rotated quads by `gameui_imgui`.
4. A font scale stack in `gameui`, so text grows with a widget that is scaled.

## Changed

*2026-11-19* — **Added.** Was: no way to ease anything over time. Now: one
scheduler resource for entities and named values, with a Game and a UI clock.
Because: the pause menu and the HUD need fades and slides that run while the
game is paused, and gameplay needs moves that any script can start without
holding a list.
