// ShadowPass.hlsl
// Depth-only drawing of meshes into a sun shadow map. gView.viewProj is the cascade's light
// matrix; instances come from the shadow instance buffer, instanced like the mesh pass.
// Surface shaders don't run here: vertex offsets and masks don't change shadows (yet).
#include "Common.hlsli"

CHERRY_PUSH_CONSTANTS ConstantBuffer<MeshDrawConstants> gDraw : register(b0);

struct VertexIn
{
	CHERRY_LOCATION(0) float3 position : POSITION;
	CHERRY_LOCATION(1) float3 normal   : NORMAL;
	CHERRY_LOCATION(2) float2 uv       : TEXCOORD;
	CHERRY_LOCATION(3) float4 color    : COLOR;
};

float4 VSMain(VertexIn vin, uint instanceId : SV_InstanceID) : SV_POSITION
{
	StructuredBuffer<InstanceData> instances = ResourceDescriptorHeap[gFrame.instanceBufferIndex];
	const InstanceData instance = instances[gDraw.instanceIndex + instanceId];
	const float4 positionW = mul(float4(vin.position, 1.0), instance.world);
	return mul(positionW, gView.viewProj);
}

// Not used by the pipeline (depth only); here so offline compile checks find an entry point.
void PSMain() {}
