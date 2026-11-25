// Shared by the mobile shaders (Renderer/Vulkan/VulkanRenderer.cpp keeps the C++ side of these
// layouts: FrameConstants, InstanceGpu, ParticleGpu, the push constants). Matrices are
// DirectXMath row-vector ones, uploaded as they are: row_major, mul(v, M).
// Vertex shaders are compiled with -fvk-invert-y, so clip space is D3D's (y up) as in the
// engine's shaders.
#ifndef CHERRY_MOBILE_COMMON_HLSLI
#define CHERRY_MOBILE_COMMON_HLSLI

static const float CHERRY_PI = 3.14159265;

struct FrameConstants
{
	row_major float4x4 viewProj;
	row_major float4x4 shadowViewProj;   // world -> the shadow map's clip space
	row_major float4x4 shadowUv;         // world -> shadow map uv (xy) and depth (z)
	float4 eye;                          // xyz; w: shadows on (1) or off (0)
	float4 sunDirection;                 // xyz: the way the light travels; w: shadow normal bias (texels)
	float4 sunStrength;                  // rgb (includes pi); w: one shadow texel in metres
	float4 ambientSky;                   // rgb
	float4 ambientGround;                // rgb
	float4 cameraRight;                  // xyz (particles)
	float4 cameraUp;                     // xyz; w: 1 / shadow map size
};

struct InstanceGpu
{
	row_major float4x4 world;
	row_major float4x4 normalMatrix;     // inverse transpose of world (upper 3x3)
	float4 tint;
};

struct ParticleGpu
{
	float4 positionSize;                 // xyz, w: size (metres across)
	float4 color;                        // linear, a: opacity
	float4 params;                       // x: emissive, y: rotation, z: square (1) or round (0)
};

[[vk::binding(0, 0)]] cbuffer FrameBuffer { FrameConstants gFrame; };
[[vk::binding(1, 0)]] StructuredBuffer<InstanceGpu> gInstances;
[[vk::binding(2, 0)]] StructuredBuffer<ParticleGpu> gParticles;
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] Texture2D<float> gShadowMap;
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] SamplerComparisonState gShadowSampler;

struct DrawConstants
{
	float4 baseColor;                    // the material's, linear
	float roughness;
	float metallic;
	float f0;                            // reflectance of the non-metal part
	uint firstInstance;                  // into gInstances (or gParticles)
};
[[vk::push_constant]] DrawConstants gDraw;

#endif
