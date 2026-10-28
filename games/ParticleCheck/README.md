# ParticleCheck

Open **Project > Open Project > ParticleCheck**, then press **Play**. Select the
Game viewport to see the authored camera. Particles begin immediately when running
with `--no-editor` or an editor-disabled build.

```powershell
.\builds\windows-msvc-debug\MjolnirEngine.exe --game games/ParticleCheck --no-editor
```

Left to right: orange exhaust using Initialize/Age/Integrate, a cyan arc using
Acceleration before Integrate, and a violet plume behind a narrow occlusion block.
Each emitter has 512 slots. Orange and violet emit 75 particles per second,
the cyan arc emits 18 smaller particles per second to show individual sprites. The scene has a
free-fly camera, a floor, and colored bases marking the emission points.

Pause and resume Play to check frozen playback, Stop and Play to check reset.
Select an emitter entity, enable ParticleSystem's overrideParameters, and adjust
spawnRateScale or sizeScale to test live overrides. Edit the corresponding files
under assets/particles to test cooking and reload, including update module order.

The scene starts in raster mode with fog disabled. Enable ray tracing in rendering
settings to compare the same scene over RT. These are the current radial GPU
sprites, textured flipbooks and persistent contrails are later particle tasks.
