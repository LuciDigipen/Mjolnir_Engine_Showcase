# ADR 0001: Shared material tables for raster and ray tracing

- Status: Accepted
- Date: 2026-10-26
- Issue: #330

## Context

Raster materials each own a small descriptor set with combined base-color and
normal samplers. Ray tracing maps the frame's distinct image/sampler pairs into
a per-frame combined array, so a `TextureHandle` is not the traced index and the
same image with a different glTF sampler cannot share a row. Material parameters
also have no common GPU location: the push constant range is full, and RT has no
per-material record.

The terrain material work needs one data contract shared by both paths before any
material type or PBR migration exists.

## Decision

1. A `TextureHandle` index is its stable row in the shared image table. Releasing
   a row advances its generation, and the row is reused only with that new
   generation.
2. Samplers have their own stable rows, keyed by the effective sampler state.
   Materials reference an image row and a sampler row separately, preserving the
   same image with different glTF sampler configurations.
3. Material parameters are opaque, aligned raw bytes. `MaterialRecordTable` owns
   creation, update, release, capacity and alignment, including splitting a free
   span around the requested base alignment; the field layout belongs to the
   later material-type reflection work.
4. Publication is frame-safe. Staged bytes are copied into a record copy that no
   in-flight frame can still read, a copy is reclaimed only after the frames that
   may have read it complete, and uploaded write lists are consumed once so a later
   creation cannot replay an in-flight range.
5. Raster finds the record through set 0's dynamic record-reference rows, with one
   frame slice refreshed after that frame slot's wait. Ray tracing finds the same
   offset through `RtInstanceInfo`. `RasterPushConstants` stays at its 128-byte
   guaranteed size and instance data does not use `firstInstance`.
6. Descriptor indexing, partially bound descriptors and non-uniform sampled
   image indexing are detected separately from ray tracing. A capable raster-only
   device enables and uses the shared tables; an incapable device keeps the
   legacy material pipeline, set layout and shaders, with one warning. Ray
   tracing still requires those features.
7. Anisotropy is optional. The requested value clamps to the device limit and
   the effective value participates in the sampler cache key.

## Consequences

- The shared table costs one set layout, one buffer and one descriptor set per
  frame in flight, plus bounded image/sampler descriptor arrays.
- Raster and RT now agree on image slots, but existing legacy set 0 bindings stay
  until the material framework migration.
- Material updates are copied and retired across frames; an update can be
  deferred when no free record copy exists.
- M2 can define reflected records without changing the renderer's table, slot or
  publication contract.
