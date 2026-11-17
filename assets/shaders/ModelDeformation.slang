// The caller declares deformation as an indexable float4 buffer before including
// this file. Raster vertices and RT's compute pass use the same morph/skin kernel.
void DeformModelVertex(uint vertex, inout float3 p, inout float3 n, inout float3 t, inout float sign)
{
    uint count = uint(deformation[0].x), targets = uint(deformation[0].y);
    if (count == 0)
        return;
    for (uint j = 0; j < targets; ++j)
    {
        float w = deformation[uint(deformation[1].z) + j].x;
        uint offset = uint(deformation[1].y) + (j * count + vertex) * 3;
        p += w * deformation[offset].xyz;
        n += w * deformation[offset + 1].xyz;
        t += w * deformation[offset + 2].xyz;
    }
    float4 range = deformation[uint(deformation[1].x) + vertex];
    if (range.y > 0)
    {
        float4 c0 = 0, c1 = 0, c2 = 0, c3 = 0;
        for (uint j = 0; j < uint(range.y); ++j)
        {
            float4 influence = deformation[uint(deformation[0].w) + uint(range.x) + j];
            uint offset = uint(deformation[0].z) + uint(influence.x) * 4;
            c0 += influence.y * deformation[offset];
            c1 += influence.y * deformation[offset + 1];
            c2 += influence.y * deformation[offset + 2];
            c3 += influence.y * deformation[offset + 3];
        }
        p = (c0 * p.x + c1 * p.y + c2 * p.z + c3).xyz;
        float determinant = dot(c0.xyz, cross(c1.xyz, c2.xyz));
        if (abs(determinant) > 1e-10)
            n = (cross(c1.xyz, c2.xyz) * n.x + cross(c2.xyz, c0.xyz) * n.y + cross(c0.xyz, c1.xyz) * n.z) / determinant;
        t = c0.xyz * t.x + c1.xyz * t.y + c2.xyz * t.z;
        if (determinant < 0)
            sign = -sign;
    }
}
