# gameshell

The screens every game gets from the engine: a HUD while it plays and a pause
menu over it, driven by the gamepad, the mouse or the keyboard. It is built on
`gameui`'s public API alone, as a game's own screens would be, and decides
nothing about the game: it reports what the player asked, and its module pushes it
out.

`mir::gameshell` · depends `mir::gameui` (public) · API `modules/gameshell/include/gameshell/` · maintainer Quy Dong Ta · reviewed 2026-10-02

## Model

`GameShell` is drawn every frame inside the game UI's frame, and returns a
`GameShellAction`: `Pause`, `Resume`, `Restart`, `Quit` or nothing. Its module
turns those into requests, which `App` applies at the top of the next frame, the
way it handles the editor's toolbar.

Outside Play it draws nothing and an open menu closes. In Play it draws the
HUD, then the menu over it.

**The HUD** sits in the top-right corner of the safe area: the play time and a
Pause button carrying the `UIPause` prompt for the active device, `Start` on a
pad and `Escape` on the keyboard. The button is `mPointerOnly`. While the game
runs, the stick and the confirm button are the game's, so no `UINavigate` press
can land on it; the mouse alone still pauses by clicking it. The clock stops
while the menu is open and starts from zero on Restart and on each Play.

**The pause menu** opens on `UIPause` (Escape, Start) or the HUD's button, and
closes on `UIPause`, `UICancel` (Escape, B, the right mouse button) or Resume.
It dims the whole area, centres a panel in the safe area, and puts focus on
Resume. Resume, Restart, Audio and Quit are a column linked up and down with
wrap, with Select and Back prompts under them. Quit leaves Play in the editor and
closes the window in a game build.

**The Audio screen** replaces the menu's panel when Audio is chosen, with focus
on its first control: the music volume slider, a Mute music toggle, the SFX
slider, Mute SFX, Restore defaults and Back, again a wrapping column. A slider
steps by a tenth on left and right, or follows the mouse while held, and each
change goes to the audio at once, so the player hears it while adjusting.
`UICancel` and Back go back a step to the menu, onto Audio; `UIPause` alone
(Start) resumes from either screen. Escape is both, so it backs out a step at a
time. The menu always opens on its main screen.

**The Controls screen** works the same way, with focus on its first control:
an Invert mouse Y toggle, an Invert gamepad Y toggle, Restore defaults and Back.
It reads and writes through `IControlsOptions`; `App` sets them with
`GameShellModule::SetControlsOptions` through `GameControlsOptions`, which hands
each change to `InputModule::SetControls`. The input module applies it from its
next update and saves it to the same per-user settings file as the audio, under
`controls`. With no options set there is no Controls entry. Its entry sits
between Audio and Quit.

**The cursor** is the game's while it plays. `GameShell` calls
`UIContext::HideCursor()` each frame in Play with the menu closed, unless the
mouse is the active device and has moved within the last
`GameShell::kCursorIdleSeconds` (2 seconds). It stays hidden while
`CameraLookHold` is down, since a mouse turning the camera is not pointing. So
a player on the gamepad or keyboard never sees it, a player who reaches for
the mouse sees it at once and can click the HUD's Pause button, and it is
always shown in the menu and outside Play. A pointer outside the game UI's
area is never hidden, so in the editor the panels round the viewport keep
their cursor.

The screen reads and writes through `IAudioOptions`, never the audio module:
`gameshell` builds without Wwise, and with no options set there is no Audio
entry. `App` sets them with `GameShellModule::SetAudioOptions` once both are
built, through `GameAudioOptions`, which calls the public audio API and restores
`AudioModule::Defaults()`. The audio module writes every change to the per-user
settings file itself, so the screen never saves.

While it is open it calls `UIContext::HoldInput()`, so gameplay reads every
action at rest through `InputState::ForGame`. The menu also asks `App` to
pause the simulation, but that is not what keeps the game's input from it:
`gameui`'s own state does, and the same call keeps a menu that pauses nothing,
an inventory say, from leaking input to the game.

Each device does the whole round trip alone:

| | Open | Navigate | Adjust a slider | Close |
|---|---|---|---|---|
| Gamepad | Start | D-pad, left stick | Left and right on either | B a step back, Start, or A on Resume |
| Keyboard | Escape | Arrow keys | Left and right arrows | Escape a step back, or Enter on Resume |
| Mouse | Click the HUD's Pause | Hover | Drag | The right button a step back, or click Resume |

### The module

`GameShellModule` registers one screen with `GameUIModule`: the HUD and the
pause menu, or `UIDemoScreen` while the `GameUIDemo` resource asks for it. `--ui-demo`
sets it at startup and the editor's **Debug > Game UI Widget Demo** switches it
at any time; the demo's art loads the first time it shows, and the style it
swaps is put back when the menus return. It turns a `GameShellAction`
into requests on the `RequestQueue`: Pause, Resume, Restart as Stop then Play,
and Quit as Stop in the editor or Quit in a game build.

## API

| Type | Header | Role |
|---|---|---|
| `GameShell`, `GameShellAction` | `gameshell/GameShell.h` | The HUD and the menu together, the play clock, and what the player asked this frame |
| `PauseMenu`, `PauseMenuAction` | `gameshell/PauseMenu.h` | Opens, closes, holds the input, and the Audio screen; `Open` for a button elsewhere |
| `IAudioOptions`, `AudioChannel` | `gameshell/AudioOptions.h` | What the Audio screen reads and changes: each channel's volume and mute, and Restore defaults |
| `GameShellModule::SetAudioOptions` | `gameshell/GameShellModule.h` | Gives the menu its Audio entry; null leaves it out |
| `IControlsOptions`, `LookDevice` | `gameshell/ControlsOptions.h` | What the Controls screen reads and changes: each device's inverted Y, and Restore defaults |
| `GameShellModule::SetControlsOptions` | `gameshell/GameShellModule.h` | Gives the menu its Controls entry; null leaves it out |
| `GameHud` | `gameshell/GameHud.h` | The clock and the pointer-only Pause button; true on the frame it is clicked |

`src/gameshell/ShellPrompts.h` draws a prompt, a key cap or pad pill with the
binding's label and then what it does, for both screens.

## Rules

**Use `gameui`'s public API and nothing else.** No ImGui, no `UIState`
internals and no device: the screens read `UIPause`, `UICancel` and the rest by
name through `UIContext`, and the device appears only in tests, which feed raw
input through `InputSystem`. This module is the proof that `gameui` serves
someone building a game, so reaching past its API would prove nothing.

**Report, do not act.** `EngineModeState` is `App`'s to change, through
`PlayScene`, `StopScene` and `TogglePause`. The shell returns what the player
asked and leaves the game alone, which is also what lets it be tested without
a world.

**Close on the frame after the action.** The frame the menu closes still holds
the input, so the press that closed it never reaches the game as well.

## State

*As of 2026-11-05.* Its module draws it every frame unless the widget demo is on. It
works in the editor's scene viewport, in Play, and in a game build, which with
`--no-editor`, or in a build without the editor at all, starts in Play.

`tests/GameShell.tests.cpp` feeds raw keys, pad buttons and the mouse through
`InputSystem` and the default bindings, as the engine does, and covers the menu opened, navigated and closed by each device alone
(the mouse in an offset area, as in the editor's viewport), game input held
while it is open without the capture flags, the stick and confirm not reaching
the HUD in play, nothing drawn outside Play and Stop closing the menu, both
screens inside the viewport and fullscreen at any safe area, a viewport too short
for the menu scaling it down whole with every button still under its text and
clickable, and the clock. The
Audio screen is covered the same way against a stand-in `IAudioOptions`: no entry
without one, each device alone opening it on its first control, stepping and
dragging the sliders, ten steps from full to silent, the mutes, Restore
defaults, and backing out a step at a time.
`smoke.game-mode` runs a game build for 120 frames on the GPU. Whether it looks
right is checked by running a game: Play in the editor, or `--no-editor`.

## Backlog

1. **Content from the game.** The shell is the engine's. A game will want its
   own HUD and its own options in the menu, through Lua or data, which is
   `gameui`'s content backlog item.
2. **Settings.** Audio is the one options screen. Bindings and the safe-area
   inset would join it.
3. **Confirm before Quit.** Quit and Restart act at once; a game with progress
   to lose wants a yes or no first, which wants a modal over the menu and
   `gameui`'s focus scopes.
4. **The pause press itself.** The game UI runs before `InputSystem` in a frame,
   so the game reads the Escape or Start that opens the menu on the frame
   before it opens. A game that binds those keys for itself sees them once.

## Changed

*2026-11-05* — **The widget demo switches at runtime.** Was: `--ui-demo`
picked the demo or the menus once at startup. Now: `GameUIDemo` picks it each
frame, set from `--ui-demo` and toggled from the editor's Debug menu.

*2026-11-04* — **The Controls screen and the hidden cursor.** Was: the menu
had no controls options, and the OS cursor showed over the game in Play. Now: a
Controls entry with Invert mouse Y and Invert gamepad Y, saved per user, and
the cursor hidden in Play unless the mouse is in use. Because: the rubric asks
for a vertical-look inversion option per device that sticks between sessions,
and for the cursor to be invisible while it is not used for gameplay.

*2026-10-28* — **The Audio screen.** Was: the menu had Resume, Restart and Quit
only. Now: an Audio entry opens music and SFX volume and mute, Restore defaults
and Back. Because: players change volume and mute from inside the game (#305).

*2026-10-24* — **`GameShellModule`.** Was: `App` drew the shell or the demo and
carried out each action. Now: the module registers the screen and pushes
requests. Because: `App` no longer wires features, and the demo-or-menus choice
belongs with the shell.

*2026-10-02* — **Created.** The pause menu and the HUD, the first screens
built on `gameui` from outside it.
