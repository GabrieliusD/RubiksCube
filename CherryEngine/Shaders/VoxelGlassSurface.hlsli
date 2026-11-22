// VoxelGlassSurface.hlsli
// The glass part of voxel models (BakeVoxelMesh with VoxelPart::Glass), used by the
// built-in translucent "engine/VoxelGlass" material:
//   vertex colour = the glass colour (linear, corner shading baked in)
//   vertex alpha  = how opaque it is (1 - VoxelMaterial::glass)
//   uv.x          = glow strength (as VoxelModelSurface.hlsli)
// Glossy, so it picks up the sky; a material's shader can change the glow like
// VoxelModelSurface.hlsli's (#define CHERRY_VOXEL_GLOW(input) before including this).
#ifndef CHERRY_VOXEL_GLASS_SURFACE_HLSLI
#define CHERRY_VOXEL_GLASS_SURFACE_HLSLI

#ifndef CHERRY_VOXEL_GLOW
#define CHERRY_VOXEL_GLOW(input) (input.vertexColor.rgb * input.uv.x)
#endif

SurfaceOutput EvaluateSurface(SurfaceInput input)
{
	SurfaceOutput s = DefaultSurfaceOutput(input);
	s.baseColor = input.vertexColor.rgb;
	s.alpha = input.vertexColor.a;
	s.roughness = 0.08;
	s.specular = 0.5;
	s.emissive = CHERRY_VOXEL_GLOW(input);
	return s;
}

#endif
