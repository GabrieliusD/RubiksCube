// ImGui on the Vulkan renderer (Renderer/Vulkan). Compiled to SPIR-V at build time
// (CherryEngine/CMakeLists.txt, cherry_mobile_shader). Vulkan clip space: y points down,
// like ImGui's screen coordinates.
struct ImGuiConstants
{
	float2 scale;
	float2 translate;
};
[[vk::push_constant]] ImGuiConstants gImGui;

[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D gTexture;
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState gSampler;

struct VSInput
{
	[[vk::location(0)]] float2 position : POSITION;
	[[vk::location(1)]] float2 uv : TEXCOORD0;
	[[vk::location(2)]] float4 color : COLOR0;
};

struct PSInput
{
	float4 position : SV_Position;
	[[vk::location(0)]] float4 color : COLOR0;
	[[vk::location(1)]] float2 uv : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
	PSInput output;
	output.position = float4(input.position * gImGui.scale + gImGui.translate, 0.0f, 1.0f);
	output.color = input.color;
	output.uv = input.uv;
	return output;
}

float4 PSMain(PSInput input) : SV_Target
{
	return input.color * gTexture.Sample(gSampler, input.uv);
}
