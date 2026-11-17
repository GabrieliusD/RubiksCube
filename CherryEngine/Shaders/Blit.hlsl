// Fullscreen-triangle copy of an already gamma-encoded texture to the back buffer.
#include "Common.hlsli"

ConstantBuffer<BlitConstants> gDraw : register(b0);

struct VertexOut
{
	float4 position : SV_POSITION;
	float2 uv : TEXCOORD;
};

VertexOut VSMain(uint vertexId : SV_VertexID)
{
	VertexOut vout;
	vout.uv = float2((vertexId << 1) & 2, vertexId & 2);
	vout.position = float4(vout.uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return vout;
}

float4 PSMain(VertexOut pin) : SV_Target
{
	Texture2D<float4> source = ResourceDescriptorHeap[gDraw.sourceTextureIndex];
	return source.SampleLevel(gLinearClamp, pin.uv, 0);
}
