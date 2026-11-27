// RmlUi's rendering (Renderer/UiRenderer.cpp), drawn over the LDR view after the tonemap and
// anti-aliasing. Colours and textures are premultiplied by alpha, and so is blending.
//
// Geometry (VSMain): PSMain (colour x texture), PSGradient (CSS gradients), PSMaskWrite /
// PSMaskAdd (into the clip mask).
// Full-view passes (VSFullscreen, a triangle over the viewport): PSCopy (copies and
// compositing, times gDraw.color), PSColorMatrix, PSBlendMask, PSBlur, PSDropShadow; these
// follow RmlUi's OpenGL 3 reference renderer.
//
// The clip mask is a count per pixel (an R16F texture): a pixel is drawn where the count equals
// gDraw.clipRequired, as the reference renderer does with a stencil buffer.
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

float4 Data(uint index)
{
	StructuredBuffer<float4> data = ResourceDescriptorHeap[gDraw.dataIndex];
	return data[index];
}

void ClipTest(float4 position)
{
	if (gDraw.clipMaskIndex != CHERRY_INVALID_INDEX)
	{
		Texture2D<float> mask = ResourceDescriptorHeap[gDraw.clipMaskIndex];
		if (abs(mask.Load(int3(position.xy, 0)) - gDraw.clipRequired) > 0.5f) discard;
	}
}

float4 SampleSource(float2 uv)
{
	Texture2D<float4> source = ResourceDescriptorHeap[gDraw.textureIndex];
	return source.SampleLevel(gLinearClamp, uv, 0);
}

// ---- Geometry ------------------------------------------------------------------------------

VertexOut VSMain(VertexIn vin)
{
	// Projection x transform for column vectors, as RmlUi's Matrix4f stores it: four columns.
	// float4x4(c0, c1, c2, c3) makes them rows, so the position multiplies from the left.
	const uint t = gDraw.transformOffset;
	const float4x4 transform = float4x4(Data(t), Data(t + 1), Data(t + 2), Data(t + 3));
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
	ClipTest(pin.position);
	float4 color = pin.color;
	if (gDraw.textureIndex != CHERRY_INVALID_INDEX)
	{
		Texture2D<float4> texture = ResourceDescriptorHeap[gDraw.textureIndex];
		color *= texture.Sample(gLinearClamp, pin.uv);
	}
	return color;
}

// CSS gradients. Parameters at paramOffset: (function, stop count), (p, v), 4 x 4 stop positions,
// then the stop colours (premultiplied). p and v: linear: start and the vector to the end;
// radial: centre and 1 / radius; conic: centre and the angle's unit vector.
#define GRADIENT_LINEAR 0
#define GRADIENT_RADIAL 1
#define GRADIENT_CONIC 2
#define GRADIENT_REPEATING_LINEAR 3
#define GRADIENT_REPEATING_RADIAL 4
#define GRADIENT_REPEATING_CONIC 5

float StopPosition(uint i)
{
	const float4 four = Data(gDraw.paramOffset + 2 + i / 4);
	return four[i % 4];
}

float4 PSGradient(VertexOut pin) : SV_Target
{
	ClipTest(pin.position);
	const float4 header = Data(gDraw.paramOffset);
	const float4 pv = Data(gDraw.paramOffset + 1);
	const int function = (int)header.x;
	const uint stops = (uint)header.y;
	const float2 p = pv.xy, v = pv.zw;
	float t = 0.0f;
	if (function == GRADIENT_LINEAR || function == GRADIENT_REPEATING_LINEAR)
	{
		t = dot(v, pin.uv - p) / dot(v, v);
	}
	else if (function == GRADIENT_RADIAL || function == GRADIENT_REPEATING_RADIAL)
	{
		t = length(v * (pin.uv - p));
	}
	else
	{
		const float2 d = pin.uv - p;
		const float2 r = float2(v.x * d.x + v.y * d.y, -v.y * d.x + v.x * d.y);   // GLSL's mat2(v.x, -v.y, v.y, v.x) * d
		t = 0.5f + atan2(-r.x, r.y) / (2.0f * 3.14159265f);
	}
	if (function >= GRADIENT_REPEATING_LINEAR && stops > 1)
	{
		const float t0 = StopPosition(0), t1 = StopPosition(stops - 1);
		const float span = t1 - t0;
		if (span > 0.0f) t = t0 + (t - t0) - span * floor((t - t0) / span);
	}
	float4 color = Data(gDraw.paramOffset + 6);
	for (uint i = 1; i < stops; ++i)
		color = lerp(color, Data(gDraw.paramOffset + 6 + i), smoothstep(StopPosition(i - 1), StopPosition(i), t));
	return pin.color * color;
}

// Clip mask: write gDraw.color.x (no blending) or add one (additive blending).
float4 PSMaskWrite(VertexOut pin) : SV_Target
{
	return float4(gDraw.color.x, 0.0f, 0.0f, 1.0f);
}

float4 PSMaskAdd(VertexOut pin) : SV_Target
{
	return float4(1.0f, 0.0f, 0.0f, 1.0f);
}

// ---- Full-view passes ------------------------------------------------------------------------

VertexOut VSFullscreen(uint vertexId : SV_VertexID)
{
	VertexOut vout;
	vout.uv = float2((vertexId << 1) & 2, vertexId & 2);   // 0..1 over the viewport (y down)
	vout.position = float4(vout.uv * float2(2, -2) + float2(-1, 1), 0, 1);
	vout.color = 1.0f;
	return vout;
}

// Copy, composite (with the clip mask) or fade: the source at uv x uvScale + uvOffset, times color.
float4 PSCopy(VertexOut pin) : SV_Target
{
	ClipTest(pin.position);
	return SampleSource(pin.uv * gDraw.uvScale + gDraw.uvOffset) * gDraw.color;
}

// The view into the supersampled base layer: nearest, so that averaging it back down gives the
// view's pixels back exactly where no UI was drawn.
float4 PSCopyPoint(VertexOut pin) : SV_Target
{
	Texture2D<float4> source = ResourceDescriptorHeap[gDraw.textureIndex];
	return source.SampleLevel(gPointClamp, pin.uv, 0);
}

// brightness, contrast, invert, grayscale, sepia, hue-rotate, saturate: a matrix's four columns
// at paramOffset, applied to the premultiplied colour (the reference renderer's shortcut:
// alpha is left alone, so the constant column is scaled by alpha).
float4 PSColorMatrix(VertexOut pin) : SV_Target
{
	const float4 c = SampleSource(pin.uv);
	const uint m = gDraw.paramOffset;
	const float4 transformed = Data(m) * c.r + Data(m + 1) * c.g + Data(m + 2) * c.b + Data(m + 3) * c.a;
	return float4(transformed.rgb, c.a);
}

// mask-image: the source times the mask's alpha (the mask texture's index is paramOffset).
float4 PSBlendMask(VertexOut pin) : SV_Target
{
	Texture2D<float4> mask = ResourceDescriptorHeap[gDraw.paramOffset];
	return SampleSource(pin.uv) * mask.SampleLevel(gLinearClamp, pin.uv, 0).a;
}

// One direction of a separable Gaussian: 17 taps gDraw.uvOffset apart. At paramOffset: the 9
// weights (centre first), then the region (min uv, max uv) outside which the source counts as
// transparent.
float4 PSBlur(VertexOut pin) : SV_Target
{
	const float4 limits = Data(gDraw.paramOffset + 3);
	float4 color = 0.0f;
	[unroll] for (int i = -8; i <= 8; ++i)
	{
		const float2 uv = pin.uv + i * gDraw.uvOffset;
		const float2 inside = step(limits.xy, uv) * step(uv, limits.zw);
		const uint k = (uint)abs(i);
		const float weight = Data(gDraw.paramOffset + k / 4)[k % 4];
		color += SampleSource(uv) * (inside.x * inside.y * weight);
	}
	return color;
}

// drop-shadow: the source's alpha, moved by uvOffset, in gDraw.color; the region as for PSBlur
// (at paramOffset).
float4 PSDropShadow(VertexOut pin) : SV_Target
{
	const float4 limits = Data(gDraw.paramOffset);
	const float2 uv = pin.uv + gDraw.uvOffset;
	const float2 inside = step(limits.xy, uv) * step(uv, limits.zw);
	return SampleSource(uv).a * inside.x * inside.y * gDraw.color;
}
