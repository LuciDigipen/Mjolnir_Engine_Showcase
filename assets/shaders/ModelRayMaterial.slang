// Include after RaySceneCommon; use the existing RT tables, not raster set 4.
#define MIR_SURFACE_EXTERNAL_TABLES 1
#define gSurfaceImages gImages
#define gSurfaceSamplers gSamplers
#define gSurfaceRecords gMaterialRecords
#include "ModelMaterial.slang"

SurfaceOutput EvaluateRayModel(RtInstanceInfo info, uint3 indices, float3 bary, float4 baseColor, float footprint)
{
    const float3 objNormal = InterpolateFloat3(info, indices, bary, kNormalOffset);
    const float3 normal = SafeNormalize(info.normalCol0.xyz * objNormal.x + info.normalCol1.xyz * objNormal.y +
                                            info.normalCol2.xyz * objNormal.z,
                                        float3(0, 1, 0));
    float4 tangent = 0;
    float2 uv = 0;
    float3 positions[3];
    float2 uvs[3];
    for (uint i = 0; i < 3; ++i)
    {
        const uint offset = indices[i] * kVertexStrideFloats;
        tangent += float4(info.vertexAddress[offset + 12], info.vertexAddress[offset + 13],
                          info.vertexAddress[offset + 14], info.vertexAddress[offset + 15]) *
                   bary[i];
        uvs[i] = float2(info.vertexAddress[offset + kUvOffset], info.vertexAddress[offset + kUvOffset + 1]);
        uv += uvs[i] * bary[i];
        const float3 p = LoadFloat3(info, indices[i], 0);
        positions[i] = info.modelCol0.xyz * p.x + info.modelCol1.xyz * p.y + info.modelCol2.xyz * p.z;
    }
    tangent = TransformTangentFrame(info.modelCol0.xyz, info.modelCol1.xyz, info.modelCol2.xyz, tangent, normal);
    MaterialContext context;
    context.rayTraced = true;
    context.uvDx = 0;
    context.uvDy = 0;
    context.incidence = max(abs(dot(normal, WorldRayDirection())), .05);
    context.footprintMeters = footprint / context.incidence;
    context.uvDensity =
        SurfaceUvDensity(positions[1] - positions[0], positions[2] - positions[0], uvs[0], uvs[1], uvs[2]);
    bool front = HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE;
    if ((info.flags & kInstanceRasterFacingCorrection) != 0)
        front = !front;
    return EvaluateModelMaterial(info.materialRecord, baseColor, normal, tangent, uv,
                                 (info.flags & kInstanceDoubleSided) != 0 && !front, context);
}
