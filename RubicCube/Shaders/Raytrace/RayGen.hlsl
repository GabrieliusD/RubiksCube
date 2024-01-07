#include "Common.hlsl"

// Raytracing output texture, accessed as a UAV
RWTexture2D< float4 > gOutput : register(u0);

// Raytracing acceleration structure, accessed as a SRV
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
};

[shader("raygeneration")] 
void RayGen() {
  // Initialize the ray payload
  HitInfo payload;
  payload.colorAndDistance = float4(0.0, 0.0, 0.0, 0.0);

  // Get the location within the dispatched 2D grid of work items
  // (often maps to pixels, so this could represent a pixel coordinate).
  uint2 launchIndex = DispatchRaysIndex().xy;
  float2 dims = float2(DispatchRaysDimensions().xy);
  float2 d = (((launchIndex.xy + 0.5f) / dims.xy) * 2.0f - 1.0f);
    
    d.y = -d.y;
    float aspectRatio = dims.x / dims.y;
    
    float4 world = mul(float4(d, 0, 1), gInvViewProj);
    world.xyz /= world.w;
    
    RayDesc ray;
    ray.Origin = gEyePosW;
    ray.Direction = normalize(world.xyz - ray.Origin);
    ray.TMin = 0;
    ray.TMax = 100000;
    
    TraceRay(SceneBVH, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

  gOutput[launchIndex] = float4(payload.colorAndDistance.rgb, 1.f);
}
