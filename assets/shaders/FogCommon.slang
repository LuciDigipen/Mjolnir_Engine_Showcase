#ifndef MIR_FOG_COMMON_SLANG
#define MIR_FOG_COMMON_SLANG
float DistanceFogTransmittance(float distance, float density, float2 range)
{
    return saturate(exp(-density * (clamp(distance, range.x, range.y) - range.x)));
}
#endif
