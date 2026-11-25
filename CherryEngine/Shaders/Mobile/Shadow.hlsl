// Depth-only sun shadow map (the instances that cast shadows).
#include "Common.hlsli"

float4 VSMain([[vk::location(0)]] float3 position : POSITION, uint instanceId : SV_InstanceID) : SV_Position
{
	const InstanceGpu instance = gInstances[gDraw.firstInstance + instanceId];
	return mul(mul(float4(position, 1.0), instance.world), gFrame.shadowViewProj);
}
