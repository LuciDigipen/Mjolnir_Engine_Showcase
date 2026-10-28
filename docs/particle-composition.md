# Particle composition experiment history

The T0 renderer probe established the initial opaque-to-HDR composition contract
for raster, ray tracing, and Ray Reconstruction. T2a replaced it with the
production `particles_gpu` module and renderer `IFxSystem` registration boundary.

The probe API, renderer pass, shader, helper types, tests, and standalone fixture
were removed during T2a. This page remains only as a link target for the local
implementation plan's historical T0 record. Use [particles_gpu](modules/particles_gpu.md)
for current ownership and verification instructions.
