// Full-screen passes: the tonemap (HDR -> gamma-encoded LDR, as Shaders/Tonemap.hlsl without
// bloom, AO, grading and eye adaptation), then FXAA (Shaders/Fxaa.hlsl) or a plain copy into
// the swapchain.
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D gSource;
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState gSourceSampler;

struct PostConstants
{
	float2 texel;       // 1 / source size
	float exposure;
	uint toneCurve;     // ACES (1) or clip (0)
};
[[vk::push_constant]] PostConstants gPost;

struct VertexOut
{
	float4 positionH : SV_Position;
	[[vk::location(0)]] float2 uv : TEXCOORD0;
};

VertexOut VSMain(uint vertexId : SV_VertexID)
{
	VertexOut vout;
	vout.uv = float2((vertexId << 1) & 2, vertexId & 2);
	vout.positionH = float4(vout.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	return vout;
}

float3 LinearToSrgb(float3 c)
{
	c = saturate(c);
	return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055);
}

float3 ACESFilm(float3 x)
{
	return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

float4 TonemapPS(VertexOut pin) : SV_Target
{
	float3 color = gSource.SampleLevel(gSourceSampler, pin.uv, 0).rgb * gPost.exposure;
	color = gPost.toneCurve != 0 ? ACESFilm(color) : saturate(color);
	return float4(LinearToSrgb(color), 1.0);
}

float4 CopyPS(VertexOut pin) : SV_Target
{
	return float4(gSource.SampleLevel(gSourceSampler, pin.uv, 0).rgb, 1.0);
}

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float3 Tap(float2 uv) { return gSource.SampleLevel(gSourceSampler, uv, 0).rgb; }

float4 FxaaPS(VertexOut pin) : SV_Target
{
	const float2 texel = gPost.texel;
	const float2 uv = pin.uv;
	const float3 rgbM = Tap(uv);
	const float lumaNW = Luma(Tap(uv + float2(-1, -1) * texel));
	const float lumaNE = Luma(Tap(uv + float2(1, -1) * texel));
	const float lumaSW = Luma(Tap(uv + float2(-1, 1) * texel));
	const float lumaSE = Luma(Tap(uv + float2(1, 1) * texel));
	const float lumaM = Luma(rgbM);
	const float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
	const float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
	if (lumaMax - lumaMin < max(0.0312, lumaMax * 0.125))
		return float4(rgbM, 1.0);
	float2 dir = float2(-((lumaNW + lumaNE) - (lumaSW + lumaSE)), (lumaNW + lumaSW) - (lumaNE + lumaSE));
	const float reduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.25 * (1.0 / 8.0), 1.0 / 128.0);
	const float scale = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
	dir = clamp(dir * scale, -8.0, 8.0) * texel;
	const float3 rgbA = 0.5 * (Tap(uv + dir * (1.0 / 3.0 - 0.5)) + Tap(uv + dir * (2.0 / 3.0 - 0.5)));
	const float3 rgbB = rgbA * 0.5 + 0.25 * (Tap(uv - dir * 0.5) + Tap(uv + dir * 0.5));
	const float lumaB = Luma(rgbB);
	return float4(lumaB < lumaMin || lumaB > lumaMax ? rgbA : rgbB, 1.0);
}
