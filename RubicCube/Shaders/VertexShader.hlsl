// Defaults for number of lights.
#ifndef NUM_DIR_LIGHTS
#define NUM_DIR_LIGHTS 3
#endif
#ifndef NUM_POINT_LIGHTS
#define NUM_POINT_LIGHTS 0
#endif
#ifndef NUM_SPOT_LIGHTS
#define NUM_SPOT_LIGHTS 0
#endif
// Include structures and functions for lighting.
#include "LightingUtils.hlsl"

Texture2D gDiffuseMap : register(t0);

SamplerState gsamPointWrap : register(s0);
SamplerState gsamPointClamp : register(s1);
SamplerState gsamLinearWrap : register(s2);
SamplerState gsamLinearClamp : register(s3);
SamplerState gsamAnisotropicWrap : register(s4);
SamplerState gsamAnisotropicClamp : register(s5);

cbuffer cbPerObject : register(b0)
{
	float4x4 gWorld;
    float4x4 gTexTransform;
};
cbuffer cbPerPass : register(b1)
{
    float4x4 gView;
    float4x4 gInvView;
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    float3 gEyePosW;
    float cbPerObjectPad1;
    float2 gRenderTargetSize;
    float2 gInvRenderTargetSize;
    float gNearZ;
    float gFarZ;
    float gTotalTime;
    float gDeltaTime;
    float4 gAmbientLight;
    
    Light gLights[MaxLights];
};

cbuffer cbMaterial : register(b2)
{
    float4 gDiffuseAlbedo;
    float3 gFresnelR0;
    float gRoughness;
    float4x4 gMatTransform;
};

struct VertexIn
{
    float3 PosL : POSITION;
    float4 Color : COLOR;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION;
    float4 Color : COLOR;
    float3 NormalW : NORMAL;
    float2 TexC : TEXCOORD;
};

VertexOut VS(VertexIn vin)
{
    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
	// Transform to homogeneous clip space.
    VertexOut vout = (VertexOut)0.0f;
    vout.PosW = posW.xyz;
	vout.PosH = mul(posW, gViewProj);
    vout.NormalW = mul(vin.NormalL, (float3x3)gWorld);
	// Just pass vertex color into the pixel shader.
	vout.Color = vin.Color;
    
    //float4 texC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform);
    vout.TexC = vin.TexC; //mul(texC, gMatTransform).xy;
    
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    float4 diffuseAlbedo = gDiffuseMap.Sample(gsamAnisotropicWrap, pin.TexC) * gDiffuseAlbedo;
    pin.NormalW = normalize(pin.NormalW);
    float3 toEyeW = normalize(gEyePosW - pin.PosW);
    float4 ambient = gAmbientLight * diffuseAlbedo;
    
    float shininess = 1.0f - gRoughness;
    Material mat = { gDiffuseAlbedo, gFresnelR0, shininess };
    float3 shadowFactor = 1.0f;
    float4 directLight = ComputeLighting(gLights, mat, pin.PosW, pin.NormalW, toEyeW, shadowFactor);
    float4 litColor = ambient + directLight;
    litColor.a = gDiffuseAlbedo.a;
    return litColor;
}