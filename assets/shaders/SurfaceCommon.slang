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

// Set 0 binding 2 in the raster pipeline; see MaterialStore::RecordRefDynamicOffset.
struct MaterialRecordRef
{
    uint offset;
    uint size;
    uint flags;
    uint reserved;
};

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
        return input;
    }
};

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
        return input;
    }
};

#endif
