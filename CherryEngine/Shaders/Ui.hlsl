// RmlUi's geometry (Renderer/UiRenderer.cpp), drawn over the LDR view after the tonemap and
// anti-aliasing. Colours and textures are premultiplied by alpha, and the pipeline blends
// that way (GraphicsPipelineDesc::premultipliedBlend).
#include "Common.hlsli"

CHERRY_PUSH_CONSTANTS ConstantBuffer<UiConstants> gDraw : register(b0);

struct VertexIn
{
	CHERRY_LOCATION(0) float2 position : POSITION;
	CHERRY_LOCATION(1) float4 color    : COLOR;
	CHERRY_LOCATION(2) float2 uv       : TEXCOORD;
};

struct VertexOut
{
	float4 position : SV_POSITION;
	float4 color    : COLOR;
	float2 uv       : TEXCOORD;
};

VertexOut VSMain(VertexIn vin)
{
	// Projection x transform for column vectors, stored column-major as RmlUi's Matrix4f keeps
	// it. Shaders are compiled row-major (-Zpr), so this reads as its transpose: multiplying
	// the position from the left gives the same result.
	StructuredBuffer<float4x4> transforms = ResourceDescriptorHeap[gDraw.transformBufferIndex];
	const float4x4 transform = transforms[gDraw.transformIndex];
	VertexOut vout;
	vout.position = mul(float4(vin.position + gDraw.translation, 0.0f, 1.0f), transform);
	// The projection maps depth to -1..1 (OpenGL's convention); D3D and Vulkan clip at 0..1.
	vout.position.z = vout.position.z * 0.5f + vout.position.w * 0.5f;
	vout.color = vin.color;
	vout.uv = vin.uv;
	return vout;
}

float4 PSMain(VertexOut pin) : SV_Target
{
	float4 color = pin.color;
	if (gDraw.textureIndex != CHERRY_INVALID_INDEX)
	{
		Texture2D<float4> texture = ResourceDescriptorHeap[gDraw.textureIndex];
		color *= texture.Sample(gLinearClamp, pin.uv);
	}
	return color;
}
