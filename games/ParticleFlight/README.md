# ParticleFlight

Open ParticleFlight under Project > Open Project and press Play, or run:

```powershell
.\builds\windows-msvc-debug\MjolnirEngine.exe --game games/ParticleFlight --no-editor
```

A scripted aircraft flies a 96 m radius loop at about 24 m/s. Two offset exhaust
attachments rotate with it, share a system tile, and cross its 64 m boundary.
The follow camera keeps the aircraft in view. The emitters combine time and distance
spawning, a one-time burst, inherited world velocity, seeded variation, drag toward
world-space wind, lifetime size/alpha curves and a four-frame cooked flipbook.

Pause freezes simulation. Stop and Play resets the path and seeds. Change the
ParticleSystem wind or scale overrides in the inspector to tune emission. The
landmarks are spaced 64 m apart. This is a visual aircraft proxy, not flight physics.
