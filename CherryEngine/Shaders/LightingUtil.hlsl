//***************************************************************************************
// LightingUtil.hlsl, based on Frank Luna's (C) 2015 lighting helpers.
// Light comes from Shared/ShaderInterop.h; directional lights are counted at runtime.
//***************************************************************************************
#ifndef CHERRY_LIGHTING_UTIL_HLSL
#define CHERRY_LIGHTING_UTIL_HLSL

#include "Shared/ShaderInterop.h"

struct Material
{
    float4 DiffuseAlbedo;
    float3 FresnelR0;
    float Shininess;
};

// Schlick gives an approximation to Fresnel reflectance (see pg. 233 "Real-Time Rendering 3rd Ed.").
// R0 = ( (n-1)/(n+1) )^2, where n is the index of refraction.
float3 SchlickFresnel(float3 R0, float3 normal, float3 lightVec)
{
    float cosIncidentAngle = saturate(dot(normal, lightVec));

    float f0 = 1.0f - cosIncidentAngle;
    float3 reflectPercent = R0 + (1.0f - R0)*(f0*f0*f0*f0*f0);

    return reflectPercent;
}

float3 BlinnPhong(float3 lightStrength, float3 lightVec, float3 normal, float3 toEye, Material mat)
{
    const float m = mat.Shininess * 256.0f;
    float3 halfVec = normalize(toEye + lightVec);

    float roughnessFactor = (m + 8.0f)*pow(max(dot(halfVec, normal), 0.0f), m) / 8.0f;
    float3 fresnelFactor = SchlickFresnel(mat.FresnelR0, halfVec, lightVec);

    float3 specAlbedo = fresnelFactor*roughnessFactor;
    specAlbedo = specAlbedo / (specAlbedo + 1.0f);

    return (mat.DiffuseAlbedo.rgb + specAlbedo) * lightStrength;
}

float3 ComputeDirectionalLight(Light L, Material mat, float3 normal, float3 toEye)
{
    // The light vector aims opposite the direction the light rays travel.
    float3 lightVec = -L.Direction;

    // Scale light down by Lambert's cosine law.
    float ndotl = max(dot(lightVec, normal), 0.0f);
    float3 lightStrength = L.Strength * ndotl;

    return BlinnPhong(lightStrength, lightVec, normal, toEye, mat);
}

float3 ComputeLighting(Light lights[CHERRY_MAX_LIGHTS], uint numDirLights, Material mat, float3 normal, float3 toEye)
{
    float3 result = 0.0f;
    for (uint i = 0; i < min(numDirLights, (uint)CHERRY_MAX_LIGHTS); ++i)
    {
        result += ComputeDirectionalLight(lights[i], mat, normal, toEye);
    }
    return result;
}

// Point light with a smooth falloff that reaches exactly zero at L.FalloffEnd (the range).
float3 ComputePointLight(Light L, Material mat, float3 pos, float3 normal, float3 toEye)
{
    float3 lightVec = L.Position - pos;
    float d = length(lightVec);
    if (d >= L.FalloffEnd)
        return 0.0f;

    lightVec /= max(d, 1e-4);
    float ndotl = max(dot(lightVec, normal), 0.0f);
    float falloff = saturate(1.0f - d / L.FalloffEnd);
    float3 lightStrength = L.Strength * ndotl * falloff * falloff;

    return BlinnPhong(lightStrength, lightVec, normal, toEye, mat);
}

float3 ComputePointLights(Light lights[CHERRY_MAX_POINT_LIGHTS], uint count, Material mat, float3 pos, float3 normal, float3 toEye)
{
    float3 result = 0.0f;
    for (uint i = 0; i < min(count, (uint)CHERRY_MAX_POINT_LIGHTS); ++i)
    {
        result += ComputePointLight(lights[i], mat, pos, normal, toEye);
    }
    return result;
}

// Linear distance fog; returns the colour blended towards the fog colour.
float3 ApplyFog(float3 color, float distance, float3 fogColor, float fogStart, float fogEnd)
{
    if (fogEnd <= fogStart)
        return color;
    float f = saturate((distance - fogStart) / (fogEnd - fogStart));
    return lerp(color, fogColor, f);
}

#endif
