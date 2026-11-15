/**
 * @file oceanWater.slang
 * @author Rahul Nair
 * @brief Procedural opaque ocean surface; mimics water, with no simulation behind it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
// Everything is computed here from world XZ, shader time and vertex color, so
// raster and ray tracing match. localPos.xz is world XZ because the ocean entity
// sits at the origin in X and Z. Vertex color is data from export_ocean.py:
// R = depth in meters, G = distance to shore in meters, B = shelter (0 cove, 1 open).

static const float kOceanTau = 6.28318531;
static const float kOceanGravity = 9.81;
static const float kOceanGoldenAngle = 2.39996323;
static const float kOceanTimeWrap = 3600.0; // jitterAndPad.z wraps here; see LightingCommon
static const int kOceanOctaves = 16;
static const float kOceanDrag = 0.35;
static const float kOceanBaseRoughness = 0.11;
static const float kOceanGustStrength = 0.45;
static const float kOceanShelterDamping = 0.65;
static const float kOceanSurfWavelength = 22.0; // meters
static const float kOceanSurfPeriod = 8.0;      // seconds per band
static const float kOceanSurfReach = 42.0;      // meters from shore
static const float3 kOceanFoamColor = float3(0.86, 0.90, 0.90);
static const float3 kOceanHaze =
    float3(0.56, 0.52, 0.42); // the sky's color at the horizon, so the far surface meets it

float OceanHash(float2 p)
{
    const float3 q = frac(float3(p.xyx) * 0.1031);
    const float3 r = q + dot(q, q.yzx + 33.33);
    return frac((r.x + r.y) * r.z);
}

float OceanNoise(float2 p)
{
    const float2 cell = floor(p);
    const float2 f = p - cell;
    const float2 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(OceanHash(cell), OceanHash(cell + float2(1, 0)), u.x),
                lerp(OceanHash(cell + float2(0, 1)), OceanHash(cell + float2(1, 1)), u.x), u.y);
}

// Two octaves is enough for masks.
float OceanNoise2(float2 p)
{
    return OceanNoise(p) * 0.65 + OceanNoise(p * 2.13 + 17.3) * 0.35;
}

struct OceanWaves
{
    float2 slope;       // d(height)/dx, d(height)/dz of the resolved octaves
    float crest;        // 0..1, resolved crest height
    float lostVariance; // slope variance of the octaves too small for this pixel
};

// Sum of sharpened directional waves. Each one drags the sample point along its
// slope, which bunches the crests of the next, smaller one. The sharpening keeps
// crests narrow and troughs wide, which a plain sine sum does not.
OceanWaves EvaluateOceanWaves(float2 position, float time, float windAngle, float spread, float longest,
                              float wavelengthRatio, float steepness, float steepnessRatio, float drag, float footprint)
{
    OceanWaves waves;
    waves.slope = float2(0.0, 0.0);
    waves.crest = 0.0;
    waves.lostVariance = 0.0;

    float wavelength = longest;
    float slopeScale = steepness;
    float crestWeight = 0.0;
    // Meander the whole field, or the long waves read as straight parallel lines from altitude.
    float2 warped =
        position + 70.0 * (float2(OceanNoise2(position / 340.0), OceanNoise2(position / 340.0 + 41.7)) - 0.5);
    for (int i = 0; i < kOceanOctaves; ++i)
    {
        // Slope peaks in the middle of the spectrum: long swell is gentle, and strong swell shows as bands.
        const float swell = 1.0 - exp(-float(i + 1) / 3.0);
        const float octaveSlope = slopeScale * swell;
        const float resolve = smoothstep(2.0, 6.0, wavelength / max(footprint, 1e-4));
        // Filtered as variance, so a distant surface turns rough rather than shimmering.
        waves.lostVariance += (1.0 - resolve * resolve) * octaveSlope * octaveSlope * 0.5;
        if (resolve > 0.0)
        {
            const float angle = windAngle + spread * sin(float(i) * kOceanGoldenAngle);
            const float2 direction = float2(cos(angle), sin(angle));
            const float k = kOceanTau / wavelength;
            // Deep-water dispersion, rounded to whole cycles per wrap so the wrap does not show.
            const float cycles = round(sqrt(kOceanGravity * k) * kOceanTimeWrap / kOceanTau);
            // Breaks each wave into finite groups; endless crests cross into a woven lattice.
            const float group = OceanNoise(warped / (wavelength * 5.0) + float(i) * 13.7);
            const float phase = dot(direction, warped) * k + cycles * kOceanTau * (time / kOceanTimeWrap) + group * 1.5;
            // Long waves stay round: a sharp crest that long flips the sun's glint on and off along a straight line.
            const float sharpness = lerp(0.25, 1.0, saturate(float(i) / 8.0));
            const float wave = exp(sharpness * (sin(phase) - 1.0));
            const float derivative = sharpness * wave * cos(phase);
            const float height = lerp(0.35, 1.4, group);

            waves.slope += direction * derivative * octaveSlope * resolve * height;
            waves.crest += wave * octaveSlope * resolve * height;
            crestWeight += octaveSlope;
            warped -= direction * derivative * wavelength * octaveSlope * drag;
        }
        wavelength *= wavelengthRatio;
        slopeScale *= steepnessRatio;
    }
    waves.crest = crestWeight > 0.0 ? waves.crest / crestWeight : 0.0;
    return waves;
}

// Light lines on the seabed where wave focus would gather it.
float OceanCaustics(float2 position, float time)
{
    const float2 p = position * 0.55;
    const float a = abs(sin(p.x + 1.7 * sin(p.y * 0.73 + time * 0.8) + time * 0.31));
    const float b = abs(sin(p.y * 1.13 + 1.9 * sin(p.x * 0.67 - time * 0.63) - time * 0.27));
    return pow(saturate(1.0 - min(a, b)), 6.0);
}

struct OceanWaterMaterial : IMaterial
{
    // Eight float4s is the most a record can hold: slangc crashes on Load<M> past 128 bytes.
    float4 deepColor;    // rgb = open water; a = depth in meters where the seabed detail is gone
    float4 sandColor;    // rgb = seabed where water is thin; a = whitecap amount
    float4 absorption;   // rgb = per meter, per channel; a = scatter strength
    float4 skyZenith;    // rgb; a = meters where the far fade starts
    float4 skyHorizon;   // rgb; a = meters where the far fade ends
    float4 scatterColor; // rgb = light through crests; a = surf amount
    float4 wind;         // x = direction in radians, y = spread in radians, z = gust scale in meters, w = gust speed
    float4 waveShape;    // x = longest wavelength, y = wavelength ratio, z = steepness, w = steepness ratio

    SurfaceOutput Evaluate(SurfaceInput input, MaterialContext context)
    {
        const float time = jitterAndPad.z;
        const float2 position = input.localPos.xz;
        const float depth = input.vertexColor.r;
        const float shore = input.vertexColor.g;
        const float shelter = input.vertexColor.b;
        const float3 V = input.viewDirWS;

        const float grazing = max(V.y, 0.05);
        const float footprint = input.viewDistance * rayCone.x / grazing;

        // Patches of stronger wind, which keep the surface from reading as uniform from altitude.
        const float2 gustPoint = position / wind.z + wind.w * time * float2(cos(wind.x), sin(wind.x)) / wind.z;
        const float gust = OceanNoise2(gustPoint);
        const float windy = lerp(1.0 - kOceanGustStrength, 1.0 + kOceanGustStrength, gust);
        const float calm = lerp(1.0 - kOceanShelterDamping, 1.0, shelter);

        const OceanWaves waves = EvaluateOceanWaves(position, time, wind.x, wind.y, waveShape.x, waveShape.y,
                                                    waveShape.z * windy * calm, waveShape.w, kOceanDrag, footprint);

        float3 N = normalize(float3(-waves.slope.x, 1.0, -waves.slope.y));
        const float variance = waves.lostVariance * windy * windy * calm * calm;
        // GGX alpha is about sqrt(2 * slope variance), and roughness is sqrt(alpha).
        float roughness = sqrt(max(kOceanBaseRoughness * kOceanBaseRoughness, sqrt(2.0 * variance)));

        // Foam, whose sharp shapes fade to their average once a pixel is wider than they are.
        const float foamDetail = 1.0 - smoothstep(3.0, 16.0, footprint);
        const float foamNoise = OceanNoise2(position * 0.09 + float2(time * 0.04, -time * 0.03));
        const float fineNoise = OceanNoise2(position * 0.7 - float2(time * 0.11, time * 0.07));

        const float bandPhase = (shore + foamNoise * 9.0) / kOceanSurfWavelength + time / kOceanSurfPeriod;
        const float band = pow(saturate(0.5 + 0.5 * sin(bandPhase * kOceanTau)), 5.0);
        const float surfReach = 1.0 - smoothstep(kOceanSurfReach * 0.3, kOceanSurfReach, shore);
        const float surfMask = smoothstep(0.3, 0.7, foamNoise * 0.6 + fineNoise * 0.4 + 0.15);
        const float surfFoam = lerp(0.18, band * surfMask, foamDetail) * surfReach * scatterColor.a;

        const float lap = 0.5 + 0.5 * sin(time * 0.6 + foamNoise * 9.0);
        const float edgeFoam =
            (1.0 - smoothstep(0.0, 2.5 + lap * 4.0, shore)) * lerp(0.5, smoothstep(0.25, 0.6, fineNoise), foamDetail);

        const float capShape = smoothstep(0.62, 0.86, waves.crest + (gust - 0.5) * 0.25);
        const float capMask = smoothstep(0.45, 0.8, fineNoise);
        const float whitecaps =
            lerp(smoothstep(0.55, 0.85, gust) * 0.05, capShape * capMask, foamDetail) * sandColor.a * shelter;

        const float foam = saturate(surfFoam + edgeFoam + whitecaps);

        N = normalize(lerp(N, float3(0.0, 1.0, 0.0), foam * 0.7));
        roughness = lerp(roughness, 0.75, foam);

        const float NoV = saturate(dot(N, V));
        const float fresnel = 0.02 + 0.98 * pow(1.0 - NoV, 5.0);
        // A rough surface sees many slopes, so it never reaches a mirror's full grazing Fresnel.
        const float effectiveFresnel = lerp(fresnel, min(fresnel, 0.55), saturate(roughness * 2.0));

        float3 R = reflect(-V, N);
        R.y = abs(R.y);
        const float3 sky = lerp(skyHorizon.rgb, skyZenith.rgb, pow(saturate(R.y), 0.45));

        // Thin water shows the seabed, and each channel dies off with depth at its own rate.
        // Patches of weed and rock, so a flat shelf does not read as one even band from altitude.
        const float bedPatches = OceanNoise2(position / 95.0 + 7.3);
        const float clearDepth = max(depth, 0.0) + bedPatches * bedPatches * 3.5;
        const float3 transmitted = exp(-absorption.rgb * clearDepth);
        const float2 refracted = position + waves.slope * clearDepth * 1.5;
        const float seabedDetail = 1.0 - smoothstep(0.5, 4.0, footprint);
        const float ripples = lerp(0.5, OceanNoise(refracted * 0.45), seabedDetail);
        const float caustics =
            OceanCaustics(refracted, time) * seabedDetail * (1.0 - smoothstep(0.0, deepColor.a, clearDepth));
        const float3 seabed = sandColor.rgb * (0.82 + ripples * 0.28 + caustics * 0.3);
        const float3 body = lerp(deepColor.rgb, seabed, transmitted);

        const float3 L = normalize(lightDirWS.xyz);
        const float backlit = pow(saturate(dot(V, normalize(N * 0.4 - L))), 4.0);
        const float3 scatter = scatterColor.rgb * absorption.a * backlit * saturate(waves.crest * 1.5) * lightColor.rgb;

        SurfaceOutput output = (SurfaceOutput)0;
        output.f90 = float3(1);
        output.dielectricF90 = 1;
        output.albedo = lerp(body * (1.0 - effectiveFresnel), kOceanFoamColor, foam);
        output.normalWS = N;
        output.roughness = saturate(roughness);
        output.dielectricF0 = lerp(0.012, 0.04, foam);
        output.f0 = output.dielectricF0; // only the sun reads this; the sky uses its own Fresnel
        output.ao = 1.0;
        output.height = 0.0;
        output.emissive = (sky * effectiveFresnel + scatter) * (1.0 - foam);

        // The mesh ends in the distance; this hides where.
        const float far = smoothstep(skyZenith.a, skyHorizon.a, input.viewDistance);
        output.albedo *= 1.0 - far;
        output.emissive = lerp(output.emissive, kOceanHaze, far);
        output.normalWS = normalize(lerp(output.normalWS, float3(0.0, 1.0, 0.0), far));
        output.roughness = lerp(output.roughness, 1.0, far);
        return output;
    }
};
