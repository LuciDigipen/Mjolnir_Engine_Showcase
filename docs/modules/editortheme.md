# editortheme

The editor colour palettes and spacing (Midnight, the default, and Slate), plus
DPI rescaling for fonts and icons. It
draws nothing and knows no engine type.

`mir::editortheme` · depends nothing engine-side; `imgui` (private) · API `modules/editortheme/include/editortheme/` · maintainer Sumin Park · reviewed 2026-09-06

## Model

One public header, one call. `editor` applies the theme once at startup, and
nothing else calls in. Each theme is its own source file (`MidnightTheme.cpp`,
`SlateTheme.cpp`) that writes spacing, rounding and colours over a
default-constructed `ImGuiStyle` and returns its status colours;
`EditorTheme.cpp` picks one by `EditorThemeKind`.

Midnight is modelled on Unreal Engine 5's dark editor, pushed close to black:
title, menu and tab strips (`#0A0A0A`), panels a step lighter (`#161616`),
recessed outlined inputs (`#0A0A0A`), grey category bars, blue row
selection and one blue accent (`#0070E0`). Corners are 4px, scrollbars are thin
pills, and the selected tab takes the panel's colour so it joins the panel below.
`ImGuiCol_Header` is row selection here, so category bars draw with
`ThemeColor::Category` instead (`widgets::CategoryHeader` in `editor`). Because frames carry a border, a transparent button that should look
flat pushes `ImGuiStyleVar_FrameBorderSize` to 0, as the title-bar window
controls in `editor` do. Slate is the earlier blue-grey, rounded theme.

It is a separate module rather than a file inside `editor` because it depends on
nothing engine-side. It can be read, edited or swapped without touching anything
that owns state, and a second theme is a second file here rather than a branch in
`editor`.

## API

| Type | Header | Role |
|---|---|---|
| `EditorTheme` | `editortheme/EditorTheme.h` | Palette and spacing, applied to the live `ImGuiStyle` |
| `ThemeColor` | `editortheme/EditorTheme.h` | Colours outside `ImGuiStyle` that `editor`'s widgets draw with: axis tags, category bars, selected-row hover, success, primary |
| `EditorThemeKind` | `editortheme/EditorTheme.h` | Which theme `Apply` writes; defaults to `Midnight` |

## Rules

**No engine dependency.** The moment this needs `mir::core`, it has stopped being
theme data and belongs in `editor`.

**Hex values are what the screen shows.** ImGui draws into the sRGB swapchain,
which encodes the shader's output again, so `FromHex` decodes each sRGB hex to
linear first. `Apply` takes `srgbTarget` (from `VulkanRenderer::SwapchainIsSrgb`
via `editor`); on a UNORM fallback swapchain the hex goes through unchanged. Colours written straight into `ImGuiStyle` or pushed as `ImVec4`
literals elsewhere skip this and show brighter than their numbers.

**Applied by `editor`, from the user's preference.** At startup, and again at the
start of the frame after the Preferences theme combo changes. `Apply` resets the
whole style, so it must run before anything is pushed that frame.

## State

*As of 2026-10-22.* Two themes, Midnight (default) and Slate. Colours, spacing
and DPI-aware font and icon sizing all work.

No tests. It is a table of constants written into `ImGuiStyle`.

## Backlog

1. A DPI change while running does not re-scale; `ScaleForDpi` runs only with
   `Apply`.
