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

Two additional CPU emitters leave 45-second wind-driven contrails at the wing
attachments. Samples appear every 2 metres or 0.25 seconds, widen and fade with
age, and remain in world coordinates as the aircraft crosses tiles. Each trail
keeps at most 1,024 points, evicting its oldest samples on overflow. Set emission
scale to zero to stop adding samples while existing trails drift and fade.

The renderer settings now use velocity-aligned additive exhaust, camera-position
facing, soft opaque intersections and distance fog. Contrails use alpha blending
and sun/ambient lighting. These values live in the emitter JSON renderer objects,
recook assets after changing them. Particle shading follows opaque DoF/motion blur
and RR and precedes bloom/tonemapping. Glass interleaving and RT secondary rays
remain outside this composition stage.
