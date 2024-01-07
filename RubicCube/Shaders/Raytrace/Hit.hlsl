#include "Common.hlsl"

struct Vertex
{
    float3 Pos;
    float4 Color;
    float3 Normal;
    float2 TexC;
};

StructuredBuffer<Vertex> CubeVertex : register(t0);
StructuredBuffer<int> indices : register(t1);
Texture2D<float4> localTexture : register(t2);
SamplerState g_s0 : register(s0);

[shader("closesthit")] 
void ClosestHit(inout HitInfo payload, Attributes attrib) 
{
    float3 barycentrics =
    float3(1.f - attrib.bary.x - attrib.bary.y, attrib.bary.x, attrib.bary.y);

    const float3 A = float3(1, 0, 0);
    const float3 B = float3(0, 1, 0);
    const float3 C = float3(0, 0, 1);

    uint vertId = 3 * PrimitiveIndex();
    float3 hitColor = CubeVertex[indices[vertId + 0]].Color * barycentrics.x +
                  CubeVertex[indices[vertId + 1]].Color * barycentrics.y +
                  CubeVertex[indices[vertId + 2]].Color * barycentrics.z;
    
    float2 uv = CubeVertex[indices[vertId + 0]].TexC * barycentrics.x +
                  CubeVertex[indices[vertId + 1]].TexC * barycentrics.y +
                  CubeVertex[indices[vertId + 2]].TexC * barycentrics.z;
    const float3 diffuseColor = localTexture.SampleGrad(g_s0, uv, float2(0.f, 0.f), float2(0.f, 0.f)).rgb;
    
    payload.colorAndDistance = float4(diffuseColor, RayTCurrent());
}
