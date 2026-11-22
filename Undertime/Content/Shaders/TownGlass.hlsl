// TownGlass.hlsl
// The town's windows (Materials/TownGlass.mat, the glass part of its voxel models): see-through
// by day, glowing in the lamp colour at night. UndertimeModule::UpdateLighting sets the globals:
//   ShaderGlobal(0).x = how dark it is (0 day .. 1 night)
//   ShaderGlobal(1).rgb = the lamp colour (linear)
#define CHERRY_SURFACE_TRANSLUCENT
#define CHERRY_VOXEL_GLOW(input) (ShaderGlobal(1).rgb * 1.6 * ShaderGlobal(0).x)
#include "VoxelGlassSurface.hlsli"
