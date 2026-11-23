# imguiruntime

The ImGui context's lifetime: the context itself, the fonts, the GLFW and Vulkan
backends, and the `NewFrame`/`Render` bracket around each frame. It draws no UI
and holds no theme, docking or layout; those belong to whoever draws.

`mir::imguiruntime` · depends `mir::renderer`, `mir::window` (public); `mir::core`, `imgui`, `iconfontcppheaders` (private) · API `modules/imguiruntime/include/imguiruntime/` · maintainer Quy Dong Ta · reviewed 2026-09-26

## Model

One `ImGuiRuntime` per process, held by `App`, sitting between the renderer and
every UI module. The UI modules (`editor` today, `gameui` next) draw into the
context it owns, and none of them owns any part of that context's lifetime.

```cpp
mImGui.Initialize(*mWindow, mRenderer);                 // after the renderer
mEditor.Initialize(mImGui, *mWindow, projectSettingsPath, mWorld, *mSceneOps);

// each frame
mImGui.BeginFrame();                                    // the NewFrame trio
mEditor.DrawLayout(...);                                // every UI module draws here
mRenderer.SetImGuiDrawData(mImGui.EndFrame());          // ImGui::Render, GetDrawData

// teardown, reverse order
mEditor.Shutdown();
mImGui.Shutdown(mRenderer);                             // before mRenderer.Shutdown()
```

It is a module of its own because neither obvious owner is allowed to hold it.
`ImGui_ImplGlfw_InitForVulkan` needs a `GLFWwindow *`, and `renderer` never sees
`window`. `App.cpp` makes no ImGui call. `editor` was the owner until 2026-09-26,
which made an editor-free build impossible, since a game UI had nowhere else to
get a context from.

## API

| Type | Header | Role |
|---|---|---|
| `ImGuiRuntime` | `imguiruntime/ImGuiRuntime.h` | `Initialize`/`Shutdown` the context and both backends; `BeginFrame`/`EndFrame` bracket a frame |
| `ImGuiModule` | `imguiruntime/ImGuiModule.h` | Owns the runtime; `AddDrawer`, `BeginUI`, `EndUI`, `RunUI` and `EndUIOn` |

One ImGui frame is split around the frame's systems. `BeginUI`, before them,
calls `BeginFrame`, runs every `Tools` drawer, then hands the frame's keyboard
and mouse capture to `InputState`, for this frame's systems. `NewFrame` sets
ImGui's capture, so it is final there. `EndUI` runs every `Game` drawer, then
every `Late` drawer, `EndFrame`, then the draw data to the renderer. Each group
runs in registration order.

`EndUI` runs in the `BeforeRender` slot of whichever scheduler updates, after
`LateUpdate`, so the game UI reads the world the frame renders. The module
registers it on its own scheduler, and `EndUIOn` on another, as `App` does for
each Play session. `RunUI` is `BeginUI` then `EndUI`, for a frame no systems
update, such as a scene's loading wait. A `BeginUI` that finds the previous
frame still open closes it first, and an `EndUI` with no frame open does
nothing.

`VulkanRenderer::InitImGuiVulkanBackend` and `ShutdownImGuiVulkanBackend` stay in
`renderer`, because they need the device, queue and swapchain format. This module
is their only caller.

## Rules

**This module calls `NewFrame` and `Render`, and nobody else does.** Every UI
module draws between `BeginFrame` and `EndFrame`. A second `NewFrame` in a frame
asserts inside ImGui, and a UI module that brackets its own frame would lock every
other UI module out of that frame.

**It sets no `ConfigFlags` and leaves `IniFilename` null.** Docking and
`imgui.ini` are editor choices, so `editor` turns them on in its own `Initialize`.
A build without `editor` persists nothing and docks nothing. ImGui reads the ini
file lazily on the first `NewFrame`, which is why a UI module can set the path
after this module's `Initialize` returns.

**Whoever points `IniFilename` at a string owns detaching it.** `DestroyContext`
saves through that pointer, so a UI module that sets it must save and null it in
its own `Shutdown`, which runs first. `Editor::Shutdown` does.

**Order in `App` is load-bearing.** `App.h` declares `mImGui` after `mRenderer`
and before `mEditor`: the runtime initialises after the renderer and before any
UI module, and shuts down after every UI module and before the renderer, so the
Vulkan backend is gone before the device is and the GLFW backend before the
window.

**The Vulkan backend frees `gameui`'s textures.** `gameui` registers each UI
texture with the context as an ImGui user texture, and
`ImGui_ImplVulkan_Shutdown` frees every registered texture's image and
descriptor, as it does the font atlas's. So `App`'s `UIContext` outlives
`Shutdown` and goes with `App`, and the order above still holds for everything
it draws. See [gameui.md](gameui.md).

**`~ImGuiRuntime()` is a no-op if `Shutdown()` was skipped.** It leaks the context
rather than reach into a `VulkanRenderer&` it was never given.

**The fonts are shared.** Inter at 16px with Font Awesome 6 merged into the same
atlas. Every UI module gets the icon glyphs; DPI scaling is still each module's
own style decision (`EditorTheme::ScaleForDpi` in the editor's case).

## State

*As of 2026-09-27.* Owns the full lifetime and the frame bracket. `App` calls it
directly; `editor` and `gameui` are its UI clients.

No tests. It needs a device and a display. The `smoke.*` tests cover startup,
every frame and shutdown, and `smoke.switch` covers the restart that reloads
`imgui.ini`.

## Backlog

1. ~~**`gameui`**, the second client~~, landed 2026-09-27; it draws into the
   background list between the same `BeginFrame` and `EndFrame`. See
   [gameui.md](gameui.md).
2. ~~**Input capture.**~~ Landed 2026-10-24: `RunUI` hands ImGui's own capture
   to `InputState`, and `App` no longer asks the editor. Which of the editor
   and the game UI owns a click is gameui's pointer rule, landed 2026-11-20.
   See [gameui.md](gameui.md).
3. **Font choice per build.** The icon font is loaded unconditionally; a game
   that wants its own fonts has no hook yet.

## Changed

*2026-11-22* — **The frame is split around the systems.** Was: `RunUI` ran
every drawer group before the scheduler. Now: `BeginUI` runs `Tools` before it,
and `EndUI` runs `Game` and `Late` in its `BeforeRender` slot. Because: the game
UI read the previous frame's transforms and camera, so a marker over a moving
target trailed it by a frame.

*2026-11-20* — **A `Late` drawer group.** Was: `Tools`, then `Game`. Now: a
third group after `Game`. Because: the editor's scene pick needs the game UI's
hit test of the same frame, and the game UI needs the editor's viewport first.

*2026-10-24* — **`ImGuiModule` runs registered drawers.** Was: `App` called
`BeginFrame`, the editor, the game UI and `EndFrame` itself, and set the UI
capture from the editor. Now: modules register drawers, `RunUI` runs them, and
the capture comes from ImGui's own flags. Because: `App` no longer knows who
draws.

*2026-10-02* — **The editor-free build exists.** `MJOLNIR_WITH_EDITOR` OFF
builds without `editor`, and this module is then the context's only owner and
`gameui` its only client. Nothing here changed for it: no `ConfigFlags`, no
`imgui.ini`, and the shared fonts, as the rules above already required. See
[build.md](../build.md#a-build-without-the-editor).

*2026-09-26* — **Created from `Editor::Initialize`/`Shutdown`/`BeginFrame`/`EndFrame`.**
Was: `editor` created the context, loaded the fonts, initialised both backends and
bracketed the frame. Now: this module does, so `editor` and a future `gameui` both
draw into a context neither owns.
