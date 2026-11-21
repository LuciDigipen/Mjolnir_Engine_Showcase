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
| `with_opacity(alpha, fn)`, `with_clip(rect, fn)` | nothing | Runs `fn` faded, or clipped to `rect` |
| `hold_input()` | nothing | Gameplay reads every action at rest this frame, as an open menu wants |
| `hide_cursor()` | nothing | The mouse cursor is hidden this frame |
| `cancel_pressed()`, `pause_pressed()`, `focus_visible()` | bool | `UICancel` or `UIPause` this frame; whether the focus highlight shows |
| `set_focus(name[, index])` | nothing | Moves focus to a widget |
| `measure_text(text)` | width, height | Text size in UI units at the style's font size |
| `prompt(action)` | label or `nil`, gamepad | The binding's short name on the active device |

`options` is optional. Every widget takes `index`, `disabled`, `hidden`,
`pointer_only` and `links` (`{ up, down, left, right }`, each a name or
`{ name, index }`). `label`, `image` and `bar` also take `focusable`, `image`
takes `tint` (`{ r, g, b, a }`), and `slider` takes `step`. An unknown key is an
error that lists the valid ones.

## Rules

**A HUD's widgets are `pointer_only`.** In play the stick, the arrow keys and
confirm belong to the game. A focusable widget lets them land on it and click
it. A screen that holds the input, a menu, can be focusable.

**`ui` is valid only inside the `OnUI` it was given to.** A script that keeps it
and calls it later gets an error, not a crash.

**Opacity and clip are scoped.** There is no push or pop in Lua, only
`with_opacity` and `with_clip`, which pop whether `fn` returns or fails. After
each `OnUI` the bridge also pops anything left pushed, so a broken script never
trips `EndFrame`'s balance asserts.

**A bad argument fails that `OnUI`.** The error names the call and the argument,
and counts as a script error like any other, so in the editor it ends Play. The
next script's `OnUI` still runs.

**Under the shell's pause menu, the widgets are disabled.** They draw greyed and
take no press or focus, because the menu is a `Shell` screen that holds the
input. See [gameui.md](gameui.md).

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
`HelloWorld`'s default scene has the sample.

## Backlog

1. **Order between scripts,** a `ui_order` the bridge sorts by.
2. **Style from Lua.** Screens use the shared `UIStyle`.
3. **A preview in Edit.** Scripts only run in Play, so their UI only shows there.
4. **Layouts from data,** on top of the same calls: a Lua table is already one.
5. **Audio and controls options from Lua,** for a game's own settings screen,
   through `audio_script` and `input_script`.

## Changed

*2026-11-20* — **Added.** Was: every game UI was C++, and the shell's HUD and
pause menu the only one. Now: a script's `OnUI` draws game UI. Because:
`gameui` backlog item 3, a game authoring its own UI.
