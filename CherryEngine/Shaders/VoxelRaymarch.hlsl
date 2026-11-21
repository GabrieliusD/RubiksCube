// VoxelRaymarch.hlsl
// The built-in voxel surface: the palette colour as a rough dielectric (stone), used by
// volumes whose material has no surface shader. With CHERRY_ERROR_SURFACE it is the stand-in
// for surface shaders that don't compile. The ray march itself is in VoxelPass.hlsli.
#include "Surface.hlsli"

SurfaceOutput EvaluateSurface(SurfaceInput input)
{
	SurfaceOutput s;
	s.baseColor = input.vertexColor.rgb;
	s.metallic = 0.0;
	s.roughness = 0.85;
	s.specular = 0.25;   // 2% reflectance
	s.normalW = input.normalW;
	s.emissive = 0.0;
	s.alpha = 1.0;
	s.opacityMask = 1.0;
#ifdef CHERRY_ERROR_SURFACE
	int3 cell = int3(floor(input.positionW * 4.0));
	s.baseColor = 0.0;
	s.emissive = float3(1.0, 0.0, 1.0) * (((cell.x + cell.y + cell.z) & 1) ? 1.0 : 0.15);
#endif
	return s;
}

#include "VoxelPass.hlsli"
