// Film grain, the last step of the film look. Shared by tonemap.slang and fxaa.slang: grain goes
// on after anti-aliasing, so with FXAA active the tonemap pass skips it (grain = 0) and FXAA adds it.

// Cheap per-pixel-per-frame white noise - varies with seed so grain
// animates instead of sitting as a static pattern.
float FilmGrainNoise(float2 pixel, float seed)
{
    return frac(sin(dot(pixel + seed, float2(12.9898, 78.233))) * 43758.5453);
}

float3 ApplyFilmGrain(float3 ldr, float2 pixel, float grain, float seed)
{
    return ldr + (FilmGrainNoise(pixel, seed) - 0.5) * grain;
}
