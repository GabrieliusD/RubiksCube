#include "Common.hlsl"

struct Vertex
{
    float3 Pos;
    float4 Color;
    float3 Normal;
    float2 TexC;
};

RaytracingAccelerationStructure SceneBVH : register(t0);

cbuffer cbPass : register(b0)
{
    float4x4 gView;
    float4x4 gInvView;
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    float3 gEyePosW;
    float cbPerObjectPad1;
    float2 gRenderTargetSize;
    float2 gInvRenderTargetSize;
    float gNearZ;
    float gFarZ;
    float gTotalTime;
    float gDeltaTime;
    float4 gAmbientLight;
    Light gLights[16];
};

cbuffer cbPerObject : register(b1)
{
    float4x4 gWorld;
    float4x4 gTexTransform;
};

StructuredBuffer<Vertex> CubeVertex : register(t1);
StructuredBuffer<int> indices : register(t2);
Texture2D<float4> localTexture : register(t3);
SamplerState g_s0 : register(s0);
float3 HitWorldPosition()
{
    return WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
}
struct Ray
{
    float3 origin;
    float3 direction;
};

float4 TraceRadianceRay(in Ray ray, in uint currentRayRecursionDepth)
{
    if (currentRayRecursionDepth >= 1)
    {
        return float4(0, 0, 0, RayTCurrent());
    }

    // Set the ray's extents.
    RayDesc rayDesc;
    rayDesc.Origin = ray.origin;
    rayDesc.Direction = ray.direction;
    // Set TMin to a zero value to avoid aliasing artifacts along contact areas.
    // Note: make sure to enable face culling so as to avoid surface face fighting.
    rayDesc.TMin = 0.1;
    rayDesc.TMax = 10000;
    HitInfo rayPayload = { float4(0, 0, 0, RayTCurrent()), currentRayRecursionDepth + 1 };
    TraceRay(SceneBVH, 0, 0xFF, 0, 0, 0, rayDesc, rayPayload);


    return rayPayload.colorAndDistance;
}

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
    
    float3 normal = CubeVertex[indices[vertId + 0]].Normal * barycentrics.x +
                  CubeVertex[indices[vertId + 1]].Normal * barycentrics.y +
                  CubeVertex[indices[vertId + 2]].Normal * barycentrics.z;
    float3 normalW = mul(normal, (float3x3) gWorld);
    
    const float3 diffuseColor = localTexture.SampleGrad(g_s0, uv, float2(0.f, 1.f), float2(0.f, 0.f)).rgb;
    
    float ndotl = ComputeNDotL(gLights[0], normalW);
    float3 ambient = gAmbientLight.rgb * diffuseColor;
    
    Ray reflectionRay = { HitWorldPosition(), reflect(WorldRayDirection(), normalW) };

    float4 reflectionColor = float4(0, 0, 0, 0); 
    reflectionColor = TraceRadianceRay(reflectionRay, payload.depth);
    float3 normalColor = ambient + (diffuseColor * ndotl) + reflectionColor.rgb;
    payload.colorAndDistance = float4(normalColor.rgb, RayTCurrent());

}

