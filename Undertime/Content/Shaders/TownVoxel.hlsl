// TownVoxel.hlsl
// The town's voxel models (Materials/Town.mat): like the engine's VoxelModel surface, but
// the glow follows the time of day (UndertimeModule::UpdateLighting sets the globals):
//   glow strength under 3 (windows, lamps): lit only at night, in the lamp colour
//   glow strength 3 and up (fire): always, in the block's own colour
//   ShaderGlobal(0).x = how dark it is (0 day .. 1 night)
//   ShaderGlobal(1).rgb = the lamp colour (linear)
#define CHERRY_VOXEL_GLOW(input) (input.uv.x >= 3.0 ? input.vertexColor.rgb * input.uv.x : ShaderGlobal(1).rgb * input.uv.x * ShaderGlobal(0).x)
#include "VoxelModelSurface.hlsli"
