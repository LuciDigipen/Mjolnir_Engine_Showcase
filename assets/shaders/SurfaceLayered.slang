/**
 * @file SurfaceLayered.slang
 * @author Jackey Cen
 * @brief Blend combinators for layered terrain surface materials.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#ifndef MIR_SURFACE_LAYERED
#define MIR_SURFACE_LAYERED

static const uint kTerrainBlendWeight = 0;
static const uint kTerrainBlendHeight = 1;
static const uint kTerrainBlendAlpha = 2;

float4 SurfaceLayeredWeights(float w0, float w1, float w2, float w3, uint count)
{
    float values[4] = { w0, w1, w2, w3 };
    float sum = 0.0;
    for (uint index = 0; index < count; ++index)
    {
        values[index] = max(values[index], 0.0);
        sum += values[index];
    }
    if (!(sum > 0.0))
        return float4(1.0, 0.0, 0.0, 0.0);
    return float4(values[0], values[1], values[2], values[3]) / sum;
}

float4 SurfaceLayeredFactors(float4 weights, float4 heights, uint count, uint blendMode, float contrast)
{
    const float w[4] = { weights.x, weights.y, weights.z, weights.w };
    const float h[4] = { heights.x, heights.y, heights.z, heights.w };
    float f[4] = { 0.0, 0.0, 0.0, 0.0 };

    if (blendMode == kTerrainBlendHeight)
    {
        const float c = max(contrast, 1e-4);
        float top = -1e30;
        for (uint index = 0; index < count; ++index)
            if (w[index] > 0.0)
                top = max(top, w[index] + h[index]);
        float total = 0.0;
        for (uint index = 0; index < count; ++index)
        {
            if (w[index] > 0.0)
            {
                f[index] = w[index] * max(w[index] + h[index] - (top - c), 0.0);
                total += f[index];
            }
        }
        if (total > 0.0)
            for (uint index = 0; index < count; ++index)
                f[index] /= total;
        return float4(f[0], f[1], f[2], f[3]);
    }

    if (blendMode == kTerrainBlendAlpha)
    {
        float coverage = 1.0;
        for (int index = int(count) - 1; index >= 1; --index)
        {
            f[index] = w[index] * coverage;
            coverage *= max(1.0 - w[index], 0.0);
        }
        f[0] = coverage;
        return float4(f[0], f[1], f[2], f[3]);
    }

    for (uint index = 0; index < count; ++index)
        f[index] = w[index];
    return float4(f[0], f[1], f[2], f[3]);
}

// Height blending needs every weighted layer's height; the other modes only the layers they keep, which in
// alpha mode can include a zero-weight layer 0.
float4 SurfaceLayeredNeeded(float4 weights, uint count, uint blendMode)
{
    if (blendMode == kTerrainBlendHeight)
        return weights;
    return SurfaceLayeredFactors(weights, float4(0.0, 0.0, 0.0, 0.0), count, blendMode, 1.0);
}

SurfaceOutput SurfaceLayeredEmpty()
{
    return (SurfaceOutput)0;
}

SurfaceOutput SurfaceLayeredCombine(const SurfaceOutput outputs[4], float4 factors, uint count)
{
    const float f[4] = { factors.x, factors.y, factors.z, factors.w };
    SurfaceOutput blended = SurfaceLayeredEmpty();
    float3 normal = float3(0.0, 0.0, 0.0);
    float3 coatNormal = float3(0.0, 0.0, 0.0);
    float unlitWeight = 0.0;
    for (uint index = 0; index < count; ++index)
    {
        blended.albedo += outputs[index].albedo * f[index];
        normal += outputs[index].normalWS * f[index];
        blended.roughness += outputs[index].roughness * f[index];
        blended.f0 += outputs[index].f0 * f[index];
        blended.f90 += outputs[index].f90 * f[index];
        blended.dielectricF0 += outputs[index].dielectricF0 * f[index];
        blended.dielectricF90 += outputs[index].dielectricF90 * f[index];
        blended.metallic += outputs[index].metallic * f[index];
        blended.clearcoat += outputs[index].clearcoat * f[index];
        blended.clearcoatRoughness += outputs[index].clearcoatRoughness * f[index];
        coatNormal += outputs[index].clearcoatNormalWS * (outputs[index].clearcoat * f[index]);
        if (outputs[index].unlit)
            unlitWeight += f[index];
        blended.ao += outputs[index].ao * f[index];
        blended.height += outputs[index].height * f[index];
        blended.emissive += outputs[index].emissive * f[index];
    }
    blended.normalWS = dot(normal, normal) > 0.0 ? normalize(normal) : outputs[0].normalWS;
    blended.clearcoatNormalWS = dot(coatNormal, coatNormal) > 0.0 ? normalize(coatNormal) : blended.normalWS;
    blended.unlit = unlitWeight > 0.5;
    return blended;
}

struct Layered2<A : IMaterial, B : IMaterial> : IMaterial
{
    uint blendMode;
    float heightContrast;
    uint layerOffset0;
    uint layerOffset1;

    SurfaceOutput Evaluate(SurfaceInput input, MaterialContext context)
    {
        const float4 weights = SurfaceLayeredWeights(input.layerWeights.x, input.layerWeights.y, 0.0, 0.0, 2);
        const float4 needed = SurfaceLayeredNeeded(weights, 2, blendMode);
        SurfaceOutput out0 = SurfaceLayeredEmpty();
        SurfaceOutput out1 = SurfaceLayeredEmpty();
        if (needed.x > 0.0)
        {
            const A layer = gSurfaceRecords.Load<A>(context.recordOffset + layerOffset0);
            out0 = layer.Evaluate(input, context);
        }
        if (needed.y > 0.0)
        {
            const B layer = gSurfaceRecords.Load<B>(context.recordOffset + layerOffset1);
            out1 = layer.Evaluate(input, context);
        }
        const float4 heights = float4(out0.height, out1.height, 0.0, 0.0);
        const float4 factors = SurfaceLayeredFactors(weights, heights, 2, blendMode, heightContrast);
        const SurfaceOutput outputs[4] = { out0, out1, SurfaceLayeredEmpty(), SurfaceLayeredEmpty() };
        return SurfaceLayeredCombine(outputs, factors, 2);
    }
};

struct Layered3<A : IMaterial, B : IMaterial, C : IMaterial> : IMaterial
{
    uint blendMode;
    float heightContrast;
    uint layerOffset0;
    uint layerOffset1;
    uint layerOffset2;

    SurfaceOutput Evaluate(SurfaceInput input, MaterialContext context)
    {
        const float4 weights =
            SurfaceLayeredWeights(input.layerWeights.x, input.layerWeights.y, input.layerWeights.z, 0.0, 3);
        const float4 needed = SurfaceLayeredNeeded(weights, 3, blendMode);
        SurfaceOutput out0 = SurfaceLayeredEmpty();
        SurfaceOutput out1 = SurfaceLayeredEmpty();
        SurfaceOutput out2 = SurfaceLayeredEmpty();
        if (needed.x > 0.0)
        {
            const A layer = gSurfaceRecords.Load<A>(context.recordOffset + layerOffset0);
            out0 = layer.Evaluate(input, context);
        }
        if (needed.y > 0.0)
        {
            const B layer = gSurfaceRecords.Load<B>(context.recordOffset + layerOffset1);
            out1 = layer.Evaluate(input, context);
        }
        if (needed.z > 0.0)
        {
            const C layer = gSurfaceRecords.Load<C>(context.recordOffset + layerOffset2);
            out2 = layer.Evaluate(input, context);
        }
        const float4 heights = float4(out0.height, out1.height, out2.height, 0.0);
        const float4 factors = SurfaceLayeredFactors(weights, heights, 3, blendMode, heightContrast);
        const SurfaceOutput outputs[4] = { out0, out1, out2, SurfaceLayeredEmpty() };
        return SurfaceLayeredCombine(outputs, factors, 3);
    }
};

struct Layered4<A : IMaterial, B : IMaterial, C : IMaterial, D : IMaterial> : IMaterial
{
    uint blendMode;
    float heightContrast;
    uint layerOffset0;
    uint layerOffset1;
    uint layerOffset2;
    uint layerOffset3;

    SurfaceOutput Evaluate(SurfaceInput input, MaterialContext context)
    {
        const float4 weights = SurfaceLayeredWeights(
            input.layerWeights.x, input.layerWeights.y, input.layerWeights.z, input.layerWeights.w, 4);
        const float4 needed = SurfaceLayeredNeeded(weights, 4, blendMode);
        SurfaceOutput out0 = SurfaceLayeredEmpty();
        SurfaceOutput out1 = SurfaceLayeredEmpty();
        SurfaceOutput out2 = SurfaceLayeredEmpty();
        SurfaceOutput out3 = SurfaceLayeredEmpty();
        if (needed.x > 0.0)
        {
            const A layer = gSurfaceRecords.Load<A>(context.recordOffset + layerOffset0);
            out0 = layer.Evaluate(input, context);
        }
        if (needed.y > 0.0)
        {
            const B layer = gSurfaceRecords.Load<B>(context.recordOffset + layerOffset1);
            out1 = layer.Evaluate(input, context);
        }
        if (needed.z > 0.0)
        {
            const C layer = gSurfaceRecords.Load<C>(context.recordOffset + layerOffset2);
            out2 = layer.Evaluate(input, context);
        }
        if (needed.w > 0.0)
        {
            const D layer = gSurfaceRecords.Load<D>(context.recordOffset + layerOffset3);
            out3 = layer.Evaluate(input, context);
        }
        const float4 heights = float4(out0.height, out1.height, out2.height, out3.height);
        const float4 factors = SurfaceLayeredFactors(weights, heights, 4, blendMode, heightContrast);
        const SurfaceOutput outputs[4] = { out0, out1, out2, out3 };
        return SurfaceLayeredCombine(outputs, factors, 4);
    }
};

#endif
