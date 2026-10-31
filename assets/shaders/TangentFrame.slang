#ifndef MIR_TANGENT_FRAME
#define MIR_TANGENT_FRAME

// c0/c1/c2 are the model's linear columns, not rows. Explicit column sums keep
// the transform independent of Slang matrix-constructor layout.
float4 TransformTangentFrame(float3 c0, float3 c1, float3 c2, float4 tangentOS, float3 normalWS)
{
    const float3 n = normalize(normalWS);
    float3 tangent = c0 * tangentOS.x + c1 * tangentOS.y + c2 * tangentOS.z;
    tangent -= n * dot(n, tangent);
    if (dot(tangent, tangent) < 1e-12)
        tangent = cross(n, abs(n.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0));
    tangent = normalize(tangent);

    float handedness = tangentOS.w < 0.0 ? -1.0 : 1.0;
    if (dot(c0, cross(c1, c2)) < 0.0)
        handedness = -handedness;
    return float4(tangent, handedness);
}

// Finite fallback for a zero, NaN or infinite direction.
float3 SafeNormalize(float3 value, float3 fallback)
{
    const float lengthSq = dot(value, value);
    if (!(lengthSq > 1e-20) || !isfinite(lengthSq))
        return fallback;
    return value * rsqrt(lengthSq);
}

// The terrain UV +U direction projected into the tangent plane. For a Y-up
// slope (n.y, -n.x, 0); a normal parallel to Z falls back to world X.
float4 TerrainTangentFrame(float3 macroNormalWS)
{
    const float3 n = SafeNormalize(macroNormalWS, float3(0.0, 1.0, 0.0));
    float3 tangent = float3(n.y, -n.x, 0.0);
    if (!(dot(tangent, tangent) > 1e-20))
        tangent = float3(1.0, 0.0, 0.0) - n * n.x;
    if (!(dot(tangent, tangent) > 1e-20))
        tangent = float3(0.0, 0.0, 1.0) - n * n.z;
    tangent = SafeNormalize(tangent, float3(1.0, 0.0, 0.0));
    const float handedness = dot(cross(n, tangent), float3(0.0, 0.0, 1.0)) < 0.0 ? -1.0 : 1.0;
    return float4(tangent, handedness);
}

#endif
