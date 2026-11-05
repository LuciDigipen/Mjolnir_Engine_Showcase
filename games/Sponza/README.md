# Sponza graphics and physics tour

Build the development preset after adding or changing assets. Open Sponza and press **Play**, or launch directly with `MjolnirEngine.exe --game games/Sponza --no-editor`. The camera starts at rest. **Left click** advances to the next presentation cue along the original route at the original speed, then holds for narration. **Right click** follows the same route back to the previous cue; triggered events are never undone or repeated. Physics, particles, model animation and activated sun/focus transitions continue while the camera holds. In the editor, click inside the game viewport; UI and pause-menu clicks are ignored. **Stop** and **Play** replay everything from the authored starting state.

The stop times are `0, 7, 17, 28, 33, 37, 40, 46, 54, 63, 65, 69, 74, 79, 82, 95, 98, 108` on the original timeline. Stationary narration spans are skipped when navigating; moving segments keep their original duration and easing. A next event at the same camera position fires immediately. The 95-second stop brings the camera into the Fox close-up after Walk starts at 91 seconds. Repeated clicks select further stops without teleporting or skipping intermediate events. The table below uses original route times, excluding manual pauses and backward travel. Set `AUTO_PLAY = true` in the script to restore the uninterrupted 108-second tour.

The shot list and timed actions are in `assets/scripts/showcase.lua`. Each action starts after its camera arrives, and the engine log records the tour time and frame for each cue.

| Time | Exhibit | What to look for |
| --- | --- | --- |
| 0–16 s | GFX 1, 4, 7 | Directional light first, then local lights and their shadows on the courtyard and the imported Sponza model. |
| 17–37 s | GFX 2, 5, 6, 7 | Solid-color primitives, paired glossy and matte spheres, a mipmapped swatch behind two alpha-blended GGX glass spheres at 28 s, and a fixed-view directional sun sweep across the courtyard from 33 to 37 s. |
| 39–59 s | PHY 1 | At 40 s two spheres drop onto the tray and rebound differently. At 46 s the capsule tips the stack. At 54 s matched ramps show the effect of friction. |
| 63–79 s | GFX 6, 11 | Fixed-view baseline, GPU sparks at 65 s, Bloom at 69 s, and a near-to-far focus pull from 74 s. The fountain is configured for approximately 1,860 live billboards at steady state. |
| 82–91 s | GFX 9, 12 | 104 scene entities move on the platform with scripted affine transforms. |
| 91–108 s | GFX 12 | The engine's glTF clip player plays Fox's authored Walk and Run clips, selected directly by the tour script, followed by a reverse view of the atrium. |

For a raster run, disable `rendering.rayTracing` and select FXAA. For a ray-traced run, enable `rendering.rayTracing` and select DLSS when available, or select `msaa` and set `rtSampleCount` to 2. Keep the output size the same and compare the 7–13 s lighting hold or the 28–31 s glass close-up. Ray tracing blends up to eight glass layers over the opaque background. Shadow rays skip blended surfaces. The frame-loop timings in the log include the fountain off/on windows and the 104-entity window. Use a lab-machine profiler and presentation checks for graded frame-rate, particle count and tearing evidence.

## Asset credits

Fox is included unmodified from the Khronos glTF Sample Assets. PixelMannen made the model under CC0 1.0. Tomkranis rigged and animated it, and AsoboStudio and scurest converted it to glTF under CC BY 4.0. [Fox sample](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Fox) · [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) · [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).
