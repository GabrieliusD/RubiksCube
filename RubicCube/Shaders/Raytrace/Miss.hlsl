#include "Common.hlsl"

TextureCube gCubemap : register(t0);
SamplerState g_s0 : register(s0);

[shader("miss")]
void Miss(inout HitInfo payload : SV_RayPayload)
{
    float4 color = gCubemap.SampleLevel(g_s0, WorldRayDirection(), 0);
    
    payload.colorAndDistance = float4(color.rgb, -1.f);
}