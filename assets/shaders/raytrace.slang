#include "AtmosphereCommon.slang"
#include "LightingCommon.slang"

[[vk::binding(0, 0)]]
RaytracingAccelerationStructure gTlas;

[[vk::binding(1, 0)]]
RWTexture2D<float4> gOutput;

[[vk::binding(3, 0)]]
RWTexture2D<float> gDistance;

[[vk::binding(4, 0)]]
RWTexture2D<float4> gVelocity;

struct RtInstanceInfo
{
    float *vertexAddress;
    uint *indexAddress;
    float4 tint;
    float4 normalCol0;
    float4 normalCol1;
    float4 normalCol2;
    uint unlit;
    uint pad0;
    uint pad1;
    uint pad2;
    float4x4 prevModel; // camera-relative to THIS frame's camera - see DrawItem::previousModel
};

[[vk::binding(2, 0)]]
StructuredBuffer<RtInstanceInfo> gInstances;

[[vk::binding(0, 1)]]
cbuffer CameraData
{
    float4x4 viewProj;
    float4 lightDirWS;
    float4 lightColor;
    float4 ambientColor;
    float4x4 invViewProj;
    float4 cameraPos; // the origin: the TLAS is built relative to this view's camera
    float4 fogColor;
    float4 fogRange;       // x = start distance, y = end distance
    float4 cameraWorldPos; // absolute camera position, for the atmosphere only
    float4 clipPlanes;     // x = near, y = far, or 0 for no far plane
    float4 atmosphereScattering;
    float4 atmosphereRadii;
    float4 atmosphereMisc;
    float4 lightingMeta; // x = this view's count in gLights, y = its first index
    float4x4 prevViewProj;
    float4x4 sunShadowViewProj; // unused here; declared to keep the prefix contiguous - see jitterAndPad
    float4 jitterAndPad;        // xy = this frame's sub-pixel jitter in pixels, Dlss anti-aliasing only
};

static const uint kAaModeOff = 0;
static const uint kAaModeMsaa = 2;
static const uint kAaModeDlss = 3;

// D3D's standard 8x rotated-grid sample pattern, 16ths of a pixel from center - the ray-traced
// multi-sample anti-aliasing fallback's spatial supersample offsets.
static const uint kMaxMsaaSamples = 8;
static const float2 kMsaaOffsets[kMaxMsaaSamples] = {
    float2(1.0, -3.0), float2(-1.0, 3.0), float2(5.0, 1.0), float2(-3.0, -5.0),
    float2(-5.0, 5.0), float2(-7.0, -1.0), float2(3.0, 7.0), float2(7.0, -7.0)
};

struct RtPushConstants
{
    uint2 outputOffset;
    uint aaMode;
    uint sampleCount;
};

[[vk::push_constant]]
RtPushConstants pushData;

struct RayPayload
{
    float3 color;
    float distance;
    float2 velocity; // NDC-space, current minus previous - see closestHitMain
};

struct ShadowPayload
{
    float visibility;
};

static const float3 kAlbedo = float3(0.5, 0.5, 0.5);
static const float kSpecularStrength = 0.04;
static const float kShininess = 32.0;
// pos(3) + color(3) + normal(3) + uv(2) + alpha(1) + tangent(4); keep in sync with mir::Vertex.
static const uint kVertexStrideFloats = 16;

// Self-shadowing bias: the origin is nudged along the surface normal before tracing.
static const float kShadowNormalBias = 0.01;
static const float kSunShadowDistance = 10000.0;

float TraceShadowRay(float3 origin, float3 direction, float tMax)
{
    ShadowPayload shadowPayload;
    shadowPayload.visibility = 0.0;

    RayDesc shadowRay;
    shadowRay.Origin = origin;
    shadowRay.Direction = direction;
    shadowRay.TMin = 0.001;
    shadowRay.TMax = max(tMax, shadowRay.TMin);

    TraceRay(gTlas,
             RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
             0xFF, 0, 0, 1, shadowRay, shadowPayload);

    return shadowPayload.visibility;
}

[shader("raygeneration")]
void rayGenMain()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dims = DispatchRaysDimensions().xy;

    const uint sampleCount = pushData.aaMode == kAaModeMsaa
                                 ? clamp(pushData.sampleCount, 1u, kMaxMsaaSamples)
                                 : 1u;

    float3 colorSum = float3(0.0, 0.0, 0.0);
    float centerDistance = 0.0;
    float2 centerVelocity = float2(0.0, 0.0);

    for (uint i = 0; i < sampleCount; i++)
    {
        const float2 msaaOffset = pushData.aaMode == kAaModeMsaa ? kMsaaOffsets[i] / 16.0 : float2(0.0, 0.0);
        const float2 pixelOffset = msaaOffset + jitterAndPad.xy;

        const float2 ndc = (float2(pixel) + 0.5 + pixelOffset) / float2(dims) * 2.0 - 1.0;

        const float4 onNearPlane = mul(invViewProj, float4(ndc, 1.0, 1.0));
        const float3 toNearPlane = onNearPlane.xyz / onNearPlane.w - cameraPos.xyz;
        const float3 rayDir = normalize(toNearPlane);

        const float toFarPlane = clipPlanes.y * length(toNearPlane) / clipPlanes.x;

        RayDesc ray;
        ray.Origin = cameraPos.xyz;
        ray.Direction = rayDir;
        ray.TMin = 0.001;
        ray.TMax = clipPlanes.y > 0.0 ? toFarPlane : 1.0e30;

        RayPayload payload;
        payload.color = float3(0.0, 0.0, 0.0);
        payload.distance = clipPlanes.y > 0.0 ? toFarPlane : 1.0e8;
        payload.velocity = float2(0.0, 0.0); // a miss (sky) is treated as stationary

        TraceRay(gTlas, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFF, 0, 0, 0, ray, payload);

        colorSum += payload.color;
        if (i == 0)
        {
            centerDistance = payload.distance;
            centerVelocity = payload.velocity;
        }
    }

    gOutput[pixel + pushData.outputOffset] = float4(colorSum / float(sampleCount), 1.0);
    gDistance[pixel + pushData.outputOffset] = centerDistance;
    gVelocity[pixel + pushData.outputOffset] = float4(centerVelocity, 0.0, 0.0);
}

[shader("miss")]
void missMain(inout RayPayload payload)
{
    AtmosphereParams params;
    params.rayleighCoeff = atmosphereScattering.xyz;
    params.mieCoeff = atmosphereScattering.w;
    params.planetRadius = atmosphereRadii.x;
    params.atmosphereRadius = atmosphereRadii.y;
    params.rayleighScaleHeight = atmosphereRadii.z;
    params.mieScaleHeight = atmosphereRadii.w;
    params.mieG = atmosphereMisc.x;
    params.sunIntensity = atmosphereMisc.y;

    const float3 rayOrigin = float3(cameraWorldPos.x, params.planetRadius + cameraWorldPos.y, cameraWorldPos.z);
    payload.color = Atmosphere(WorldRayDirection(), rayOrigin, normalize(lightDirWS.xyz), params);
}

[shader("miss")]
void shadowMissMain(inout ShadowPayload payload)
{
    payload.visibility = 1.0;
}

[shader("closesthit")]
void closestHitMain(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    const RtInstanceInfo info = gInstances[InstanceID()];

    const float3 barycentrics = float3(
        1.0 - attribs.barycentrics.x - attribs.barycentrics.y,
        attribs.barycentrics.x,
        attribs.barycentrics.y);

    const uint primBase = PrimitiveIndex() * 3;
    const uint i0 = info.indexAddress[primBase + 0];
    const uint i1 = info.indexAddress[primBase + 1];
    const uint i2 = info.indexAddress[primBase + 2];

    // Object-space hit position (pos is floats 0-2 of each vertex) - needed
    // by both branches below, to reproject through info.prevModel for velocity.
    const uint pb0 = i0 * kVertexStrideFloats;
    const uint pb1 = i1 * kVertexStrideFloats;
    const uint pb2 = i2 * kVertexStrideFloats;
    const float3 op0 = float3(info.vertexAddress[pb0], info.vertexAddress[pb0 + 1], info.vertexAddress[pb0 + 2]);
    const float3 op1 = float3(info.vertexAddress[pb1], info.vertexAddress[pb1 + 1], info.vertexAddress[pb1 + 2]);
    const float3 op2 = float3(info.vertexAddress[pb2], info.vertexAddress[pb2 + 1], info.vertexAddress[pb2 + 2]);
    const float3 objectPos = op0 * barycentrics.x + op1 * barycentrics.y + op2 * barycentrics.z;
    const float3 hitWorldPos = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();

    float3 color;

    if (info.unlit != 0)
    {
        const uint b0 = i0 * kVertexStrideFloats + 3;
        const uint b1 = i1 * kVertexStrideFloats + 3;
        const uint b2 = i2 * kVertexStrideFloats + 3;
        const float3 c0 = float3(info.vertexAddress[b0], info.vertexAddress[b0 + 1], info.vertexAddress[b0 + 2]);
        const float3 c1 = float3(info.vertexAddress[b1], info.vertexAddress[b1 + 1], info.vertexAddress[b1 + 2]);
        const float3 c2 = float3(info.vertexAddress[b2], info.vertexAddress[b2 + 1], info.vertexAddress[b2 + 2]);
        const float3 vertexColor = c0 * barycentrics.x + c1 * barycentrics.y + c2 * barycentrics.z;
        color = vertexColor * info.tint.rgb;
    }
    else
    {
        const uint b0 = i0 * kVertexStrideFloats + 6;
        const uint b1 = i1 * kVertexStrideFloats + 6;
        const uint b2 = i2 * kVertexStrideFloats + 6;
        const float3 n0 = float3(info.vertexAddress[b0], info.vertexAddress[b0 + 1], info.vertexAddress[b0 + 2]);
        const float3 n1 = float3(info.vertexAddress[b1], info.vertexAddress[b1 + 1], info.vertexAddress[b1 + 2]);
        const float3 n2 = float3(info.vertexAddress[b2], info.vertexAddress[b2 + 1], info.vertexAddress[b2 + 2]);
        const float3 objNormal = n0 * barycentrics.x + n1 * barycentrics.y + n2 * barycentrics.z;

        const float3 N = normalize(
            info.normalCol0.xyz * objNormal.x +
            info.normalCol1.xyz * objNormal.y +
            info.normalCol2.xyz * objNormal.z);

        const float3 shadowOrigin = hitWorldPos + N * kShadowNormalBias;
        const float3 L = normalize(lightDirWS.xyz);
        const float3 V = normalize(cameraPos.xyz - hitWorldPos);
        const float3 H = normalize(L + V);

        const float diffuse = max(dot(N, L), 0.0);
        const float specular = (diffuse > 0.0) ? pow(max(dot(N, H), 0.0), kShininess) : 0.0;

        const float sunVisibility = TraceShadowRay(shadowOrigin, L, kSunShadowDistance);

        const float3 albedo = kAlbedo * info.tint.rgb;
        color = albedo * ambientColor.rgb +
                (albedo * lightColor.rgb * diffuse + lightColor.rgb * kSpecularStrength * specular) * sunVisibility;

        const int firstLight = int(lightingMeta.y);
        const int lightCount = int(lightingMeta.x);
        for (int i = 0; i < lightCount; i++)
        {
            GpuLight light = gLights[firstLight + i];
            const float3 toLight = light.positionAndRange.xyz - hitWorldPos;
            const float dist = length(toLight);
            const float range = light.positionAndRange.w;
            const float3 Lp = toLight / max(dist, 1e-5);

            float atten = clamp(1.0 - dist / max(range, 1e-5), 0.0, 1.0);
            const float cosAngle = dot(-Lp, light.directionAndSpotCos.xyz);
            atten *= (cosAngle >= light.directionAndSpotCos.w) ? 1.0 : 0.0;

            const float diffuseP = max(dot(N, Lp), 0.0);

            const float visibility = light.color.w >= 0.0 ? TraceShadowRay(shadowOrigin, Lp, dist) : 1.0;

            color += albedo * light.color.rgb * atten * diffuseP * visibility;
        }
    }

    const float clampedDist = clamp(RayTCurrent(), fogRange.x, fogRange.y);
    const float fogFactor = clamp(1.0 - exp(-fogColor.a * (clampedDist - fogRange.x)), 0.0, 1.0);
    payload.color = lerp(color, fogColor.rgb, fogFactor);
    payload.distance = RayTCurrent();

    // hitWorldPos is already this frame's camera-relative space (cameraPos is
    // its origin); info.prevModel puts objectPos in that SAME space (rebased
    // to this frame's camera, not last frame's - see DrawItem::previousModel),
    // so viewProj/prevViewProj alone carry the camera's own motion.
    const float4 currClip = mul(viewProj, float4(hitWorldPos, 1.0));
    const float4 prevClip = mul(prevViewProj, mul(info.prevModel, float4(objectPos, 1.0)));
    payload.velocity = (currClip.xy / currClip.w) - (prevClip.xy / prevClip.w);
}
