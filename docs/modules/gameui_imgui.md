# gameui_imgui

Draws the game UI through ImGui: gameui's backend over ImGui's background draw
list and its 1.92 user textures, and the drawer that runs gameui's frame. It is
a bridge, so gameui never sees ImGui and imguiruntime never sees gameui.

`mir::gameui_imgui` · depends `mir::gameui` (public); `imguiruntime`, `window`, `core`, `imgui` (private) · API `modules/gameui_imgui/include/gameui_imgui/` · maintainer Quy Dong Ta · reviewed 2026-10-24

## Model

`GameUIImGuiModule` installs an `ImGuiUIBackend` on the game UI's context and
registers a drawer in the `Game` group. Each frame the drawer takes the area
from `GameUIArea`, or the whole window, the pointer from the window's raw input,
and calls `GameUIModule::RunFrame`.

`ImGuiUIBackend` replays a draw list into ImGui's background list, its UI units
times the frame's scale, font size and rounding included. ImGui renders that list
before every window, and `VulkanRenderer` draws ImGui after the 3D
views, so the game UI lands over the scene and under Hierarchy, Inspector and
Output. A texture's pixels go into an `ImTextureData` in `WantCreate`,
registered as a user texture; `ImGui_ImplVulkan` creates, uploads and frees it,
so the renderer has no part in it.

## API

| Type | Header | Role |
|---|---|---|
| `GameUIImGuiModule` | `gameui_imgui/GameUIImGuiModule.h` | Installs the backend and runs gameui's frame in the `Game` drawer group |
| `ImGuiUIBackend` | `gameui_imgui/ImGuiUIBackend.h` | `IUIBackend` over ImGui's background list and user textures |

## Rules

**Only `ImGuiUIBackend.cpp` includes `<imgui.h>`.** It also includes
`<imgui_internal.h>`, for `RegisterUserTexture`. No public header names an
ImGui type.

**No `ImGui::Begin`.** A window would sit among the editor's panels and dock
like one. The background list has no window.

**The game UI outlives ImGui's shutdown.** The module only stops new copies when
it goes; each texture copy releases itself when the game UI goes, which `App`
puts after `ImGuiModule` (see [gameui.md](gameui.md)).

## State

*As of 2026-10-24.* Moved out of `gameui` as is. `tests/ImGuiUIBackend.tests.cpp`
hosts a headless ImGui context and covers a texture reaching ImGui's list in
`WantCreate` with its pixels, a texture made when the backend is installed,
loading through an `AssetCache`, teardown before and after the context, and
`EndFrame` drawing a sprite with the `ImTextureData` as its texture.

## Backlog

None yet.

## Changed

*2026-10-24* — **Added.** Was: `UIBackendImGui.cpp` in `gameui`, and `App`
drew the game UI in its own ImGui drawer. Now: this bridge. Because: gameui must
not link `imguiruntime` or ImGui.
