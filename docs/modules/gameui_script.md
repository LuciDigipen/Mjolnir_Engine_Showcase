# gameui_script

Lets a script draw game UI. A script that defines
`OnUI(self, world, entity, ui, dt)` is called once per game UI frame in Play,
paused or not, with a `ui` object over `gameui`'s widgets, layout helpers and
draw stacks. It is a bridge, so `gameui` never sees Lua and `script` never sees
`gameui`.

`mir::gameui_script` · depends `mir::gameui`, `mir::script`, `script_bindings` (public); `mir::assets`, `mir::core` (private) · API `modules/gameui_script/include/gameui_script/` · maintainer Quy Dong Ta · review pending

## Model

`GameUIScriptModule` registers one `UIScreenLayer::Game` screen with the
world's `GameUIModule`. Each frame that screen calls `DrawScriptScreens`, which
calls `OnUI` on every `ScriptRef` whose `OnStart` has run, through
`CallScriptCallback` from `script/BindingApi.h`. A script without `OnUI` costs a
table lookup. Nothing runs in Edit, since no script has started there.

There is no canvas object. The entity running the script stands in for one: its
UI exists while the entity does, `world:persist(entity)` keeps it across a
scene swap, and two entities make two UIs. `gameui` is immediate mode, so `OnUI`
draws the whole UI again every frame, and what it remembers lives in `self`. To
hide a panel, stop drawing it.

```lua
local Hud = {}

function Hud.NewInstanceData()
    return { shopOpen = false, volume = 0.5 }
end

function Hud.OnUI(self, world, entity, ui, dt)
    local area = ui:safe_area()

    local panel = ui:anchor(area, "top_left", 220, 120)
    ui:panel(panel)
    local column = ui:column(ui:inset(panel, 8), 4)
    ui:label("title", column:next(24), "Player")
    ui:bar("health", column:next(16), self.health / 100)

    if ui:button("shop", ui:anchor(area, "bottom_right", 120, 40), "Shop", { pointer_only = true }).clicked then
        self.shopOpen = true
    end

    if self.shopOpen then
        ui:hold_input()
        local shop = ui:anchor(area, "center", 300, 200)
        ui:panel(shop)
        local rows = ui:column(ui:inset(shop, 12), 6)
        self.volume = ui:slider("volume", rows:next(24), self.volume, 0, 1)
        if ui:button("close", rows:next(32), "Close").clicked or ui:cancel_pressed() then
            self.shopOpen = false
        end
    end
end

return Hud
```

Rects are tables `{ x, y, w, h }` in UI units: the area is laid out against
`gameui`'s 640 by 480 design size and scaled to the display, so a script never
sees pixels. A widget's name is mixed with the entity, so two entities running
one script never share a widget. A widget drawn in a loop takes
`{ index = i }`.

Text takes a game's own font and look per widget:

```lua
local font = ui:font("fonts/Rajdhani-SemiBold.ttf")
ui:label("speed", rect, string.format("%d", speed), {
    font = font, size = 26, align = "right", color = { 1, 0.3, 0.2, 1 },
    outline = { width = 1, color = { 0, 0, 0, 0.6 } }, shadow = true,
})
```

Shapes draw gauges, and the `with_*` calls turn and move what is inside them:

```lua
local cx, cy, r = 200, 300, 60
ui:arc(cx, cy, r, 135, 405, { width = 6, color = { 1, 1, 1, 0.2 } })
ui:arc(cx, cy, r, 135, 135 + 270 * speed / top, { width = 6, gradient = {
    from = { cx - r, cy }, to = { cx + r, cy }, from_color = { 0.3, 0.9, 0.5, 1 }, to_color = { 1, 0.3, 0.2, 1 } } })
ui:with_rotation(135 + 270 * speed / top, cx, cy, function()
    ui:line(cx, cy, cx + r - 12, cy, { width = 3, color = { 1, 0.2, 0.2, 1 } })
end)
ui:circle(cx, cy, r + 6, { width = 1, dash = { 4, 4 } })
```

Fades and slides come from the `actions` bridge's named values:
`ui:with_opacity(world:value("Hud.alpha", 1), function() ... end)`.
`games/HelloWorld/assets/scripts/ui_demo.lua` is a working example.

## API

| Type | Header | Role |
|---|---|---|
| `GameUIScriptModule` | `gameui_script/GameUIScriptModule.h` | The bindings and the Game screen; takes the `ScriptModule`, the world's `GameUIModule`, the assets and the resolver |
| `UIScriptFrame` | `gameui_script/UIBindings.h` | What `ui` wraps; the context is set only while an `OnUI` runs |
| `RegisterUIBindings`, `DrawScriptScreens` | `gameui_script/UIBindings.h` | The `UI`, `UIStack` and `UISprite` usertypes; one frame of every script's `OnUI` |

### Lua

Every call is a method, `ui:name(...)`. A widget call returns a result table
`{ clicked, hovered, held, focused, changed }`.

| Call | Returns | Effect |
|---|---|---|
| `bounds()`, `safe_area()`, `scale()` | rect, rect, number | The area, the area inside the safe-area insets, and pixels per UI unit |
| `anchor(rect, where, w, h)` | rect | A `w` by `h` rect at `where` in `rect`: `top_left`, `top`, `top_right`, `left`, `center`, `right`, `bottom_left`, `bottom`, `bottom_right` |
| `inset(rect, all)`, `inset(rect, left, top, right, bottom)` | rect | `rect` shrunk on each side |
| `row(rect[, gap])`, `column(rect[, gap])` | stack | A stack with `:next(size)`, `:last(size)`, `:rest()` and `:remaining()`, `gameui`'s `UIStack` |
| `panel(rect)` | nothing | A panel in the style's colour or sprite |
| `label(name, rect, text[, options])` | result | Text in a widget's rect |
| `button(name, rect, text[, options])` | result | A button; `clicked` on release over it, or confirm while focused |
| `image(name, rect, sprite[, options])` | result | A sprite from `ui:sprite` |
| `bar(name, rect, fraction[, options])` | result | A bar filled from 0 to 1 |
| `toggle(name, rect, text, value[, options])` | value, result | A checkbox; returns the new value |
| `slider(name, rect, value, min, max[, options])` | value, result | A slider; returns the new value |
| `sprite(path[, slice])` | sprite or `nil` | A texture from the game's assets, nine-sliced by `slice`; loaded once, and `nil` with one error if it does not load |
| `font(path)` | font or `nil` | A `.ttf` or `.otf` from the game's assets, for the `font` option; loaded once, and `nil` with one error if it does not load. Unlike `ui`, a font can be kept in `self` |
| `with_opacity(alpha, fn)`, `with_clip(rect, fn)` | nothing | Runs `fn` faded, or clipped to `rect` |
| `with_offset(x, y, fn)` | nothing | Runs `fn` moved by `x`, `y` |
| `with_rotation(degrees, fn)`, `with_rotation(degrees, px, py, fn)` | nothing | Runs `fn` turned clockwise about the origin or about `px`, `py` |
| `with_transform(transform, fn)` | nothing | Runs `fn` scaled, then turned, both about `pivot`, then moved: `{ offset = { x, y }, rotate = degrees, scale = s or { x, y }, pivot = { x, y } }`, each optional |
| `line(x1, y1, x2, y2[, options])` | nothing | A line |
| `lines(points[, options])` | nothing | A line through `{ x1, y1, x2, y2, ... }`, round to the start with `closed = true` |
| `polygon(points[, options])` | nothing | `points` filled, convex or not; at least three |
| `circle(x, y, radius[, options])` | nothing | A filled circle, or its outline with `width` or `dash` |
| `arc(x, y, radius, from, to[, options])` | nothing | Part of a circle's outline, from `from` to `to` degrees |
| `pie(x, y, radius, from, to[, options])` | nothing | A filled slice, as for a cooldown |
| `rect(rect[, options])` | nothing | A filled rect, or its outline with `width` or `dash` |
| `hold_input()` | nothing | Gameplay reads every action at rest this frame, as an open menu wants |
| `hide_cursor()` | nothing | The mouse cursor is hidden this frame |
| `cancel_pressed()`, `pause_pressed()`, `focus_visible()` | bool | `UICancel` or `UIPause` this frame; whether the focus highlight shows |
| `set_focus(name[, index])` | bool | Moves focus to a widget; `false` while the shell's pause menu holds the input |
| `measure_text(text[, font[, size]])` | width, height | Text size in UI units, in the style's font and size unless given; `nil` keeps the style's |
| `prompt(action)` | label or `nil`, gamepad | The binding's short name on the active device |

`options` is optional. Every widget takes `index`, `disabled`, `hidden`,
`pointer_only` and `links` (`{ up, down, left, right }`, each a name or
`{ name, index }`). `label`, `image` and `bar` also take `focusable`, `image`
takes `tint` (`{ r, g, b, a }`), and `slider` takes `step`. An unknown key is an
error that lists the valid ones.

`label`, `button` and `toggle` also take text options, which change that
widget's text only:

| Option | Value |
|---|---|
| `font` | What `ui:font` returned |
| `size` | Font size in UI units, more than 0 |
| `color` | The text colour |
| `align` | `"left"`, `"center"` or `"right"`, inside the widget's padding |
| `shadow` | `true` for black, 1 unit down and right; `false` for none; or `{ color, x, y }` |
| `outline` | `true` for black, 1 unit wide; `false` for none; or `{ color, width }` |

A colour is `{ r, g, b, a }` by name or by position, a missing channel 1.

Shapes take their own options, and an unknown one is an error that lists the
valid ones for that call:

| Option | Value | Calls |
|---|---|---|
| `color` | The shape's colour, white unless given | all |
| `width` | Line width in UI units, more than 0, 1 unless given | `line`, `lines`, `circle`, `arc`, `rect` |
| `dash` | `{ length, gap, offset }` by position or by name; the gap is the length unless given, and `offset` moves the dashes along | `line`, `lines`, `circle`, `arc`, `rect` |
| `gradient` | `{ from = { x, y }, to = { x, y }, from_color, to_color }`, multiplying the colour, each end white unless given | all |
| `closed` | `true` to join the last point to the first | `lines` |
| `rounding` | Corner radius | `rect` |

Angles are degrees, 0 to the right and clockwise, the way y grows down, for the
shapes and `with_rotation` alike. A shape takes no pointer and has no name.
Points are a flat list, at most 4096 to a call.

## Rules

**A HUD's widgets are `pointer_only`.** In play the stick, the arrow keys and
confirm belong to the game. A focusable widget lets them land on it and click
it. A screen that holds the input, a menu, can be focusable.

**`ui` is valid only inside the `OnUI` it was given to.** A script that keeps it
and calls it later gets an error, not a crash.

**Opacity, clip and transform are scoped.** There is no push or pop in Lua,
only `with_opacity`, `with_clip`, `with_offset`, `with_rotation` and
`with_transform`, which pop whether `fn` returns or fails. After
each `OnUI` the bridge also pops anything left pushed, so a broken script never
trips `EndFrame`'s balance asserts.

**A bad argument fails that `OnUI`.** The error names the call and the argument,
and counts as a script error like any other, so in the editor it ends Play. The
next script's `OnUI` still runs.

**Under the shell's pause menu, the widgets are disabled.** They draw greyed and
take no press or focus, because the menu is a `Shell` screen that holds the
input. A slider or button held when the menu opens lets go, and `ui:set_focus`
returns `false` and leaves focus on the menu. See [gameui.md](gameui.md).

**Text options last one widget.** They are set on the style while that widget
draws and put back after it, error or not, so the next widget and the shell's
pause menu draw in the shared style.

**Scripts draw in query order.** Two entities' UIs overlap in whatever order the
ECS walks them. Keep a game's screens on one entity, or apart on screen, until
there is an order field.

## State

*As of 2026-11-20.* Widgets, layout, sprites, scoped opacity and clip, input
holding, focus, prompts and text measurement work. `tests/UIBindings.tests.cpp`
drives `DrawScriptScreens` through a real `UIContext` with no backend: a click
on a Lua button, only after `OnStart` and only in Play (paused included), two
entities' widgets kept apart, toggle and slider values and layout rects, a
failing `OnUI` leaving the stacks balanced while the next script still draws,
and a bad argument, an unknown option and a kept `ui` each failing cleanly.
Text options: a label's font, size, colour, alignment, shadow and outline in its
draw commands and gone from the next label and the style, `measure_text` with a
size, `ui:font` `nil` for a font that does not load, and a bad `align`, a text
option on a slider and a string for `font` each failing the `OnUI`. Shapes:
each call's commands, points, widths, angles in radians and gradient, under
`with_rotation` and `with_transform`, and a button moved by `with_offset`
clicked where it is drawn; an odd or short point list, an option a call does
not take, a gradient with one point, a zero dash, a negative radius, a wrong
`with_rotation` call and an unknown transform key each failing the `OnUI`, and
an error inside nested `with_*` calls leaving the transforms balanced.
`HelloWorld`'s default scene has the sample.

## Backlog

1. **Order between scripts,** a `ui_order` the bridge sorts by.
2. **Style from Lua.** Screens use the shared `UIStyle`; text options are
   per widget, so a script repeats them or keeps them in a table.
3. **A preview in Edit.** Scripts only run in Play, so their UI only shows there.
4. **Layouts from data,** on top of the same calls: a Lua table is already one.
5. **Audio and controls options from Lua,** for a game's own settings screen,
   through `audio_script` and `input_script`.

## Changed

*2026-11-21* — **Shapes and transforms.** Was: a script drew only through
widgets and panels, axis-aligned. Now: `line`, `lines`, `polygon`, `circle`,
`arc`, `pie` and `rect`, with `width`, `dash` and `gradient`, and
`with_offset`, `with_rotation` and `with_transform`, the pointer following the
transform. Because: dials, compasses and HUD markings from Lua.

*2026-11-21* — **Fonts and text options.** Was: text was in the shared style's
font, size and colour only. Now: `ui:font` loads a game's font, and `label`,
`button` and `toggle` take `font`, `size`, `color`, `align`, `shadow` and
`outline`; `measure_text` takes a font and size. Because: HUD text in a game's
own type.

*2026-11-21* — **Under the pause menu, a held widget lets go and `set_focus` is
refused.** Was: a slider dragged when the menu opened kept changing behind it,
and a script setting focus every frame took it from the menu's Resume. Now:
both are blocked in `gameui`, and `ui:set_focus` returns whether it took.
Because: review.

*2026-11-20* — **Added.** Was: every game UI was C++, and the shell's HUD and
pause menu the only one. Now: a script's `OnUI` draws game UI. Because:
`gameui` backlog item 3, a game authoring its own UI.
