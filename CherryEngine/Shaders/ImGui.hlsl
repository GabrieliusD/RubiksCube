// Dear ImGui's draw lists, drawn by the renderer on any backend (Renderer/ImGuiRenderer.cpp).
// A texture id is a bindless SRV index + 1.
#include "Common.hlsli"

struct ImGuiConstants
{
	float2 scale;
	float2 translate;
	uint textureIndex;
};
CHERRY_PUSH_CONSTANTS ConstantBuffer<ImGuiConstants> gDraw : register(b0);

struct VertexIn
{
	CHERRY_LOCATION(0) float2 position : POSITION;
	CHERRY_LOCATION(1) float2 uv       : TEXCOORD;
	CHERRY_LOCATION(2) float4 color    : COLOR;
};

struct VertexOut
{
	float4 position : SV_POSITION;
	float2 uv       : TEXCOORD;
	float4 color    : COLOR;
};

VertexOut VSMain(VertexIn vin)
{
	VertexOut vout;
	vout.position = float4(vin.position * gDraw.scale + gDraw.translate, 0.0f, 1.0f);
	vout.uv = vin.uv;
	vout.color = vin.color;
	return vout;
}

float4 PSMain(VertexOut pin) : SV_Target
{
	Texture2D<float4> texture = ResourceDescriptorHeap[gDraw.textureIndex];
	return pin.color * texture.Sample(gLinearWrap, pin.uv);
}
