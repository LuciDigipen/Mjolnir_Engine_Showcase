// Surface material framework shared by the raster and ray-traced passes.
//
// Coordinate spaces:
//   positionRS  camera-relative render space (world axes, camera at the origin)
//   normalWS / tangentWS  the same render space, not world-double space
//   localPos    object space of the drawn mesh
//   uv0         frontend-defined: artist UV for MeshFrontend
//
// Materials only read SurfaceInput and sample through MaterialContext, so the
// same material source compiles into the raster and RT passes without knowing
// which set carries the shared image/sampler/record rows.

#ifndef MIR_SURFACE_COMMON
#define MIR_SURFACE_COMMON

#include "LightingCommon.slang"
#include "TangentFrame.slang"

#ifndef MIR_SURFACE_SET
#define MIR_SURFACE_SET 4
#endif
#ifndef MIR_SURFACE_IMAGES_BINDING
#define MIR_SURFACE_IMAGES_BINDING 1
#endif
#ifndef MIR_SURFACE_SAMPLERS_BINDING
#define MIR_SURFACE_SAMPLERS_BINDING 2
#endif
#ifndef MIR_SURFACE_RECORDS_BINDING
#define MIR_SURFACE_RECORDS_BINDING 0
#endif
#ifndef MIR_DRAW_RECORDS_SET
#define MIR_DRAW_RECORDS_SET 5
#endif
#ifndef MIR_DRAW_RECORDS_BINDING
#define MIR_DRAW_RECORDS_BINDING 0
#endif

static const uint kMaxSurfaceImages = 1024;
static const uint kMaxSurfaceSamplers = 256;

[[vk::binding(MIR_SURFACE_IMAGES_BINDING, MIR_SURFACE_SET)]]
Texture2D<float4> gSurfaceImages[kMaxSurfaceImages];

[[vk::binding(MIR_SURFACE_SAMPLERS_BINDING, MIR_SURFACE_SET)]]
SamplerState gSurfaceSamplers[kMaxSurfaceSamplers];

[[vk::binding(MIR_SURFACE_RECORDS_BINDING, MIR_SURFACE_SET)]]
ByteAddressBuffer gSurfaceRecords;

// Stable image and sampler rows, filled by the CPU material loader.
struct TextureRef
{
    uint image;
    uint sampler;
};

// Must match DrawRecord in modules/renderer/include/renderer/DrawRecord.h.
struct DrawRecord
{
    uint materialOffset;
    uint materialSize;
    uint flags;
    uint reserved;
    uint terrain[4];
};

// Mirrors mir::TerrainWeightUv and mir::AccumulateTerrainLayerWeights in
// modules/renderer/include/renderer/TerrainDrawData.h.
float2 TerrainWeightUv(float3 localPos, float invFootprint, uint side)
{
    const float span = float(side - 1);
    return (localPos.xz * invFootprint * span + 0.5) / float(side);
}

// Word 0 is the weight image row plus one, 1 the sampler row, 2 the material
// layer of each R..A channel (0xFF unused) and 3 the reciprocal tile size.
float4 TerrainLayerWeights(DrawRecord record, float3 localPos)
{
    if (record.terrain[0] == 0)
        return float4(1.0, 0.0, 0.0, 0.0);
    const uint image = record.terrain[0] - 1;
    const uint sampler = record.terrain[1];
    const float invFootprint = asfloat(record.terrain[3]);
    uint width, height;
    gSurfaceImages[NonUniformResourceIndex(image)].GetDimensions(width, height);
    if (width == 0 || !(invFootprint > 0.0) || !isfinite(invFootprint))
        return float4(1.0, 0.0, 0.0, 0.0);
    const float2 uv = TerrainWeightUv(localPos, invFootprint, width);
    const float4 channels = gSurfaceImages[NonUniformResourceIndex(image)].SampleLevel(
        gSurfaceSamplers[NonUniformResourceIndex(sampler)], uv, 0.0);
    const float channelWeights[4] = { channels.x, channels.y, channels.z, channels.w };
    float layers[4] = { 0.0, 0.0, 0.0, 0.0 };
    for (uint channel = 0; channel < 4; ++channel)
    {
        const uint layer = (record.terrain[2] >> (8 * channel)) & 0xFFu;
        if (layer != 0xFFu && layer < 4u)
            layers[layer] += channelWeights[channel];
    }
    const float sum = layers[0] + layers[1] + layers[2] + layers[3];
    if (!(sum > 0.0))
        return float4(1.0, 0.0, 0.0, 0.0);
    return float4(layers[0], layers[1], layers[2], layers[3]) / sum;
}

// World-space UV gradients on a triangle. The maximum singular value of the
// 2x3 Jacobian is the largest UV change per meter, including rank-one UVs.
float3 SurfaceUvGradient(float3 e1, float3 e2, float du1, float du2)
{
    const float a = dot(e1, e1);
    const float b = dot(e1, e2);
    const float c = dot(e2, e2);
    const float determinant = a * c - b * b;
    if (!(abs(determinant) > 1e-20) || !isfinite(determinant))
        return float3(0.0, 0.0, 0.0);
    return ((c * du1 - b * du2) * e1 + (a * du2 - b * du1) * e2) / determinant;
}

float SurfaceUvDensity(float3 e1, float3 e2, float2 uv0, float2 uv1, float2 uv2)
{
    const float2 duv1 = uv1 - uv0;
    const float2 duv2 = uv2 - uv0;
    const float3 gradientU = SurfaceUvGradient(e1, e2, duv1.x, duv2.x);
    const float3 gradientV = SurfaceUvGradient(e1, e2, duv1.y, duv2.y);
    const float uu = dot(gradientU, gradientU);
    const float vv = dot(gradientV, gradientV);
    const float uv = dot(gradientU, gradientV);
    const float trace = uu + vv;
    const float difference = uu - vv;
    const float largest = 0.5 * (trace + sqrt(max(difference * difference + 4.0 * uv * uv, 0.0)));
    if (!(largest > 0.0) || !isfinite(largest))
        return 0.0;
    return sqrt(largest);
}

struct SurfaceInput
{
    float3 positionRS;
    float3 normalWS;
    float3 tangentWS;
    float bitangentSign;
    float2 uv0;
    float3 localPos;
    float3 viewDirWS;
    float viewDistance;
    float4 vertexColor;
    bool frontFace;
    // UV units per meter at this point; ray tracing uses it for the mip level.
    float uvDensity;
    // Weights of material layers 0..3, always summing to one. A material that
    // does not read it keeps drawing as a single layer.
    float4 layerWeights;
};

// Every field lerps, and height lets layered materials blend by height later.
struct SurfaceOutput
{
    float3 albedo;
    float3 normalWS;
    float roughness;
    float f0;
    float ao;
    float height;
    float3 emissive;
};

SurfaceOutput ApplySurfaceTint(SurfaceOutput surface, float4 tint)
{
    surface.albedo *= tint.rgb;
    surface.emissive *= tint.rgb;
    return surface;
}

struct MaterialContext
{
    bool rayTraced;
    float2 uvDx;
    float2 uvDy;
    // The ray cone's world footprint at the hit and its incidence cosine.
    float footprintMeters;
    float incidence;
    // UV units per meter for the coordinates the material samples.
    float uvDensity;
    // Byte offset of the material record a layered material loads its children from.
    uint recordOffset;

    float4 Sample(TextureRef texture, float2 uv)
    {
        const uint image = NonUniformResourceIndex(texture.image);
        const uint sampler = NonUniformResourceIndex(texture.sampler);
        if (rayTraced)
        {
            uint width, height;
            gSurfaceImages[image].GetDimensions(width, height);
            const float footprint = max(uvDensity * footprintMeters * max(width, height), 0.0);
            return gSurfaceImages[image].SampleLevel(gSurfaceSamplers[sampler], uv, log2(max(footprint, 1.0)));
        }
        return gSurfaceImages[image].SampleGrad(gSurfaceSamplers[sampler], uv, uvDx, uvDy);
    }

    float4 Sample(TextureRef texture, float2 uv, float2 dx, float2 dy)
    {
        const uint image = NonUniformResourceIndex(texture.image);
        const uint sampler = NonUniformResourceIndex(texture.sampler);
        if (rayTraced)
        {
            uint width, height;
            gSurfaceImages[image].GetDimensions(width, height);
            const float footprint = max(uvDensity * footprintMeters * max(width, height), 0.0);
            return gSurfaceImages[image].SampleLevel(gSurfaceSamplers[sampler], uv, log2(max(footprint, 1.0)));
        }
        return gSurfaceImages[image].SampleGrad(gSurfaceSamplers[sampler], uv, dx, dy);
    }
};

// Hue rotation around the neutral axis; zero degrees returns the input.
float3 SurfaceHueShift(float3 color, float degrees)
{
    if (!isfinite(degrees) || degrees == 0.0)
        return color;
    const float radians = degrees * 0.017453292519943295;
    const float3 axis = float3(0.5773502691896258, 0.5773502691896258, 0.5773502691896258);
    const float cosine = cos(radians);
    const float sine = sin(radians);
    const float3 rotated = color * cosine + cross(axis, color) * sine + axis * dot(axis, color) * (1.0 - cosine);
    return max(rotated, float3(0.0));
}

float SurfaceParallaxFade(float distance, float fadeStartMeters, float fadeEndMeters)
{
    if (!isfinite(distance) || !isfinite(fadeStartMeters) || !isfinite(fadeEndMeters) ||
        !(fadeEndMeters > fadeStartMeters))
        return 0.0;
    return 1.0 - smoothstep(fadeStartMeters, fadeEndMeters, distance);
}

// The macro sample is in its own repeat, so raster gradients and the RT
// footprint must use the macro period rather than the detail-tile period.
MaterialContext SurfaceMacroContext(MaterialContext context, float tileSizeMeters, float macroPeriodMeters)
{
    const float scale = tileSizeMeters / max(macroPeriodMeters, 0.001);
    context.uvDx *= scale;
    context.uvDy *= scale;
    context.uvDensity *= scale;
    return context;
}

static const float kParallaxMinOffsetPixels = 0.5;
static const uint kParallaxMinSteps = 8;
// Sine of the angle between the UV derivatives below which the footprint is degenerate.
static const float kParallaxMinFootprintSine = 1e-3;

// Marches the height texture in tangent space before any surface sampling. The
// ray is centered on the authored reference height, so a constant height above
// or below it yields the opposite signed intersection without quantization.
// The raster path reuses the caller's uvDx/uvDy, so SampleGrad keeps the
// original derivatives; the RT path uses MaterialContext's explicit level of
// detail. Fade scales the found offset continuously, and maxDistance clamps it.
float2 SurfaceParallaxUv(SurfaceInput input, MaterialContext context, TextureRef heightTexture,
    float2 uv, float tileSizeMeters, float heightMeters, float reference, float maxDistanceMeters,
    float fadeStartMeters, float fadeEndMeters, uint maxSteps, bool rayTracedEnabled)
{
    if (maxSteps == 0 || !(tileSizeMeters > 0.001) || !isfinite(tileSizeMeters) || !(heightMeters > 0.0) ||
        !isfinite(heightMeters) || !isfinite(reference) || !isfinite(maxDistanceMeters) ||
        !(maxDistanceMeters > 0.0))
        return uv;
    if (context.rayTraced && !rayTracedEnabled)
        return uv;

    const float fade = SurfaceParallaxFade(input.viewDistance, fadeStartMeters, fadeEndMeters);
    if (!(fade > 0.0))
        return uv;

    const float3 normal = SafeNormalize(input.normalWS, float3(0.0, 1.0, 0.0));
    const float3 tangent = SafeNormalize(input.tangentWS, float3(1.0, 0.0, 0.0));
    const float3 bitangent = input.bitangentSign * cross(normal, tangent);
    const float3 viewDirection = SafeNormalize(input.viewDirWS, float3(0.0, 0.0, -1.0));
    const float3 viewTangent = float3(dot(viewDirection, tangent), dot(viewDirection, bitangent),
        dot(viewDirection, normal));
    if (!(viewTangent.z > 0.0))
        return uv;

    // Fade the offset to zero as the view reaches the horizon instead of
    // switching at a threshold; the search direction stays bounded below.
    const float incidenceFade = smoothstep(0.0, 0.05, viewTangent.z);
    if (!(incidenceFade > 0.0))
        return uv;

    // Bounds the horizontal offset at grazing angles instead of letting it
    // diverge, which keeps every step finite and the march stable.
    float2 rayDirection = viewTangent.xy / max(viewTangent.z, 0.15);
    float rayLength = length(rayDirection);
    if (!isfinite(rayLength))
        return uv;
    if (rayLength > 8.0)
    {
        rayDirection *= 8.0 / rayLength;
        rayLength = 8.0;
    }

    const float uvPerMeter = 1.0 / tileSizeMeters;
    const float clampedReference = saturate(reference);
    const float maximumOffsetUv = maxDistanceMeters * uvPerMeter;

    // Ray tracing and hosts without usable derivatives keep the material's step count.
    const float2 uvDx = context.uvDx;
    const float2 uvDy = context.uvDy;
    const float determinant = uvDx.x * uvDy.y - uvDy.x * uvDx.y;
    const bool footprintKnown = !context.rayTraced && all(isfinite(uvDx)) && all(isfinite(uvDy)) &&
        isfinite(determinant) && abs(determinant) > kParallaxMinFootprintSine * length(uvDx) * length(uvDy);
    const uint stepCap = min(maxSteps, 128u);
    uint steps = stepCap;
    if (footprintKnown)
    {
        // Screen pixels per unit of ray direction, through the inverse of [uvDx uvDy].
        const float2 rayPixels = float2(uvDy.y * rayDirection.x - uvDy.x * rayDirection.y,
                                     uvDx.x * rayDirection.y - uvDx.y * rayDirection.x) / determinant;
        const float rayPixelLength = length(rayPixels);
        const float pixelsPerUv = rayLength > 0.0 ? rayPixelLength / rayLength : 0.0;
        const float visibleOffsetUv = min(rayLength * heightMeters * fade * uvPerMeter, maximumOffsetUv);
        if (visibleOffsetUv * incidenceFade * pixelsPerUv < kParallaxMinOffsetPixels)
            return uv;
        const float spanPixels = 2.0 * heightMeters * fade * uvPerMeter * rayPixelLength;
        steps = uint(min(max(ceil(spanPixels), float(kParallaxMinSteps)), float(stepCap)));
    }

    // A signed height s in meters is the ray's height relative to the reference
    // plane: the surface at uv + dir*s*fade is at (packed - reference)*H. Searching
    // from +H down to -H visits the highest/visible surface first; the full range
    // always contains a crossing because packed is in [0, 1] and the reference is
    // clamped to that range.
    float previousS = heightMeters;
    float2 previousUv = uv + rayDirection * ((previousS * fade) * uvPerMeter);
    float previousSurface = (context.Sample(heightTexture, previousUv, context.uvDx, context.uvDy).g -
                                clampedReference) * heightMeters;
    float previousGap = previousSurface - previousS;

    for (uint index = 1; index <= steps; ++index)
    {
        const float currentS = heightMeters - 2.0 * heightMeters * float(index) / float(steps);
        const float2 currentUv = uv + rayDirection * ((currentS * fade) * uvPerMeter);
        const float currentSurface = (context.Sample(heightTexture, currentUv, context.uvDx, context.uvDy).g -
                                         clampedReference) * heightMeters;
        const float currentGap = currentSurface - currentS;
        if (!isfinite(currentGap))
            return uv;
        if (previousGap <= 0.0 && currentGap >= 0.0)
        {
            const float denominator = currentGap - previousGap;
            const float fraction = denominator > 1e-12 ? saturate(-previousGap / denominator) : 0.0;
            float intersection = lerp(previousS, currentS, fraction);
            if (abs(intersection) < 1e-7)
                intersection = 0.0;
            float2 offsetUv = rayDirection * (intersection * fade * uvPerMeter);
            const float offsetLength = length(offsetUv);
            if (offsetLength > maximumOffsetUv)
                offsetUv *= maximumOffsetUv / offsetLength;
            return uv + offsetUv * incidenceFade;
        }
        previousS = currentS;
        previousGap = currentGap;
    }

    return uv;
}

interface IMaterial
{
    SurfaceOutput Evaluate(SurfaceInput input, MaterialContext context);
};

struct MeshFrontend
{
    static SurfaceInput Build(float3 positionRS, float3 normalWS, float4 tangentWS, float2 uv0,
        float3 localPos, float4 vertexColor, bool frontFace)
    {
        SurfaceInput input;
        input.positionRS = positionRS;
        input.normalWS = normalWS;
        input.tangentWS = tangentWS.xyz;
        input.bitangentSign = tangentWS.w < 0.0 ? -1.0 : 1.0;
        input.uv0 = uv0;
        input.localPos = localPos;
        input.viewDirWS = normalize(cameraPos.xyz - positionRS);
        input.viewDistance = length(cameraPos.xyz - positionRS);
        input.vertexColor = vertexColor;
        input.frontFace = frontFace;
        input.uvDensity = 0.0;
        input.layerWeights = float4(1.0, 0.0, 0.0, 0.0);
        return input;
    }
};

#include "SurfaceLayered.slang"

struct TerrainFrontend
{
    static SurfaceInput Build(float3 positionRS, float3 macroNormalWS, float2 uv0, float3 localPos,
        float4 vertexColor, bool frontFace, float uvDensity)
    {
        SurfaceInput input;
        input.positionRS = positionRS;
        input.normalWS = SafeNormalize(macroNormalWS, float3(0.0, 1.0, 0.0));
        const float4 tangent = TerrainTangentFrame(input.normalWS);
        input.tangentWS = tangent.xyz;
        input.bitangentSign = tangent.w;
        input.uv0 = uv0;
        input.localPos = localPos;
        input.viewDirWS = normalize(cameraPos.xyz - positionRS);
        input.viewDistance = length(cameraPos.xyz - positionRS);
        input.vertexColor = vertexColor;
        input.frontFace = frontFace;
        input.uvDensity = uvDensity;
        input.layerWeights = float4(1.0, 0.0, 0.0, 0.0);
        return input;
    }
};

#endif
