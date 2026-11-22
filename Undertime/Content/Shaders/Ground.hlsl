// Ground.hlsl
// The valley's terrain (Materials/Ground.mat): the engine's voxel model surface, wet after rain.
//   ShaderGlobal(0).z = how wet the ground is (WeatherAt in DayNight.h): darker, with a sheen
//   on faces turned up; puddles gather on flat paths and dirt (not on grass).
#define EvaluateSurface DryVoxelSurface
#include "VoxelModelSurface.hlsli"
#undef EvaluateSurface

SurfaceOutput EvaluateSurface(SurfaceInput input)
{
	SurfaceOutput s = DryVoxelSurface(input);
	const float wet = ShaderGlobal(0).z;
	if (wet <= 0.0)
		return s;
	const float up = saturate(input.normalW.y);
	const float soaked = wet * lerp(0.45, 1.0, up);
	s.baseColor *= 1.0 - 0.35 * soaked;
	s.roughness = lerp(s.roughness, 0.32, soaked);
	s.specular = lerp(s.specular, 0.55, soaked);

	// Puddles on flat ground that isn't grass, in patches.
	const float3 c = input.vertexColor.rgb;
	const bool grass = c.g > c.r * 1.12 && c.g > c.b * 1.2;
	if (up > 0.95 && !grass)
	{
		const float2 p = input.positionW.xz;
		const float n = CloudValueNoise(p * 0.35) * 0.7 + CloudValueNoise(p * 1.1 + 3.7) * 0.3;
		const float puddle = smoothstep(0.58, 0.63, n) * saturate(wet * 1.6 - 0.4);
		s.baseColor = lerp(s.baseColor, s.baseColor * 0.45, puddle);
		s.roughness = lerp(s.roughness, 0.03, puddle);
		s.specular = lerp(s.specular, 1.0, puddle);
	}
	return s;
}
