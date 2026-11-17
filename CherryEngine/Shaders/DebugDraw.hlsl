// World-space debug lines. Vertices are pulled from a bindless structured buffer
// with SV_VertexID, so no input layout is needed.
#include "Common.hlsli"

struct VertexOut
{
	float4 position : SV_POSITION;
	float4 color : COLOR;
};

VertexOut VSMain(uint vertexId : SV_VertexID)
{
	StructuredBuffer<DebugVertex> vertices = ResourceDescriptorHeap[gFrame.debugVertexBufferIndex];
	DebugVertex v = vertices[vertexId];

	VertexOut vout;
	vout.position = mul(float4(v.position, 1.0), gView.viewProj);
	vout.color = UnpackRGBA8(v.color);
	return vout;
}

float4 PSMain(VertexOut pin) : SV_Target
{
	return pin.color;
}
