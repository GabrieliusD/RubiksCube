// VoxelModelSurface.hlsli
// The surface of meshes made of blocks: baked voxel models (Voxel/VoxelModel.h,
// BakeVoxelMesh) and anything built the same way (UNDERTIME's terrain). Used by the built-in
// "engine/VoxelModel" material.
//   vertex colour = the block colour, with corner shading baked in
//   uv.x          = how strongly that colour glows (0 for most colours)
//   uv.y          = the block size in the mesh's own units (0 = no per-block shading)
// Each block gets a slightly different brightness, so merged faces still read as blocks.
#ifndef CHERRY_VOXEL_MODEL_SURFACE_HLSLI
#define CHERRY_VOXEL_MODEL_SURFACE_HLSLI

static const float kVoxelBlockJitter = 0.08;   // +- relative brightness per block

// What the glowing blocks emit. A material's shader can define this before including the
// file to change it (e.g. windows that only light up at night):
//   #define CHERRY_VOXEL_GLOW(input) (...)   // float3, linear HDR
#ifndef CHERRY_VOXEL_GLOW
#define CHERRY_VOXEL_GLOW(input) (input.vertexColor.rgb * input.uv.x)
#endif

float VoxelBlockHash(int3 c)
{
	uint h = asuint(c.x) * 73856093u ^ asuint(c.y) * 19349663u ^ asuint(c.z) * 83492791u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return (h & 0xFFFF) / 65535.0;
}

SurfaceOutput EvaluateSurface(SurfaceInput input)
{
	SurfaceOutput s = DefaultSurfaceOutput(input);
	const float block = input.uv.y;
	if (block > 0.0)
	{
		// The block this pixel is on: half a block into the face (the face's geometric normal
		// from screen derivatives works for rotated and scaled models too; either side is
		// fine as long as a whole face picks the same one).
		float3 n = cross(ddx(input.positionL), ddy(input.positionL));
		n *= rsqrt(max(dot(n, n), 1e-20));
		const int3 cell = int3(floor(input.positionL / block - n * 0.5));
		s.baseColor *= 1.0 + (VoxelBlockHash(cell) - 0.5) * 2.0 * kVoxelBlockJitter;
	}
	s.emissive = CHERRY_VOXEL_GLOW(input);
	return s;
}

#endif
