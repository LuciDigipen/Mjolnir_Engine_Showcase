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

#endif
