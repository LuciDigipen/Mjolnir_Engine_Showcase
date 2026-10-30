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

    float4 Sample(TextureRef texture, float2 uv)
    {
        const uint image = NonUniformResourceIndex(texture.image);
        const uint sampler = NonUniformResourceIndex(texture.sampler);
        if (rayTraced)
            return gSurfaceImages[image].SampleLevel(gSurfaceSamplers[sampler], uv, 0.0);
        return gSurfaceImages[image].SampleGrad(gSurfaceSamplers[sampler], uv, uvDx, uvDy);
    }

    float4 Sample(TextureRef texture, float2 uv, float2 dx, float2 dy)
    {
        const uint image = NonUniformResourceIndex(texture.image);
        const uint sampler = NonUniformResourceIndex(texture.sampler);
        if (rayTraced)
            return gSurfaceImages[image].SampleLevel(gSurfaceSamplers[sampler], uv, 0.0);
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
        return input;
    }
};

#endif
