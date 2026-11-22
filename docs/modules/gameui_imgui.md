# gameui_imgui

Draws the game UI through ImGui: gameui's backend over ImGui's background draw
list and its 1.92 user textures, and the drawer that runs gameui's frame. It is
a bridge, so gameui never sees ImGui and imguiruntime never sees gameui.

`mir::gameui_imgui` · depends `mir::gameui` (public); `imguiruntime`, `window`, `core`, `imgui` (private) · API `modules/gameui_imgui/include/gameui_imgui/` · maintainer Quy Dong Ta · reviewed 2026-10-24

## Model

`GameUIImGuiModule` installs an `ImGuiUIBackend` on the game UI's context and
registers a drawer in the `Game` group. Each frame the drawer takes the area
from `GameUIArea`, or the whole window, the pointer from the window's raw input,
and calls `GameUIModule::RunFrame`. The pointer is withheld while ImGui wants
the mouse or `GameUIArea::mToolsHavePointer` is set, and a withheld pointer
leaves the cursor shown. See gameui.md's Rules.

`ImGuiUIBackend` replays a draw list into ImGui's background list, its UI units
times the frame's scale, font size and rounding included. ImGui renders that list
before every window, and `VulkanRenderer` draws ImGui after the 3D
views, so the game UI lands over the scene and under Hierarchy, Inspector and
Output. A texture's pixels go into an `ImTextureData` in `WantCreate`,
registered as a user texture; `ImGui_ImplVulkan` creates, uploads and frees it,
so the renderer has no part in it.

A font goes into ImGui's font atlas through `AddFontFromMemoryTTF`, the atlas
owning a copy of its bytes. ImGui 1.92 bakes each size the first time text
asks for it, so one font serves every size. The font is removed from the atlas
when the game UI releases it, and a context destroyed first takes it down with
its atlas. Text with no font draws in `ImGui::GetFont()`, as before.

`MakeFont` first runs the checks ImGui's stb_truetype loader makes, finding the
font in the file and its required tables, and refuses a font that fails them,
since ImGui asserts on it in Debug and gives no font in Release. Text is drawn
and measured at its size in pixels rounded as ImGui rounds it, which asserts at
0, so both keep the size between 1 and ImGui's largest, as `PushFont` does: a
0.25 unit label is drawn and measured one pixel tall.

## API

| Type | Header | Role |
|---|---|---|
| `GameUIImGuiModule` | `gameui_imgui/GameUIImGuiModule.h` | Installs the backend and runs gameui's frame in the `Game` drawer group |
| `ImGuiUIBackend` | `gameui_imgui/ImGuiUIBackend.h` | `IUIBackend` over ImGui's background list, user textures and font atlas |

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
`EndFrame` drawing a sprite with the `ImTextureData` as its texture. It also
loads `fonts/Inter.ttf` from the source tree as a game font: into the atlas and
out with its `UIFonts`, measured with its own widths, drawn by a label, loaded
through an `AssetCache` once per id, and kept past its context's end. A damaged
copy, its tables renamed, and a cut-off copy are refused with the atlas
untouched, and text under a pixel draws and measures at one pixel.

## Backlog

None yet.

## Changed

*2026-11-21* — **Fonts.** Was: text always drew in `ImGui::GetFont()`. Now:
`MakeFont` adds a game font to ImGui's atlas, and text and measuring use the
command's font. Because: gameui fonts.

*2026-11-20* — **The pointer is withheld under the editor.** Was: the game UI
read the raw mouse even with an ImGui popup or the gizmo over it. Now: the
drawer sets `UIPointer::mWithheld` from `WantCaptureMouse` and
`GameUIArea::mToolsHavePointer`. Because: one click did two things.

*2026-10-24* — **Added.** Was: `UIBackendImGui.cpp` in `gameui`, and `App`
drew the game UI in its own ImGui drawer. Now: this bridge. Because: gameui must
not link `imguiruntime` or ImGui.
