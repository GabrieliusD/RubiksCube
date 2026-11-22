// Foliage.hlsl
// Trees and ground cover (Materials/Foliage.mat): the engine's voxel model surface, swaying
// in the wind. UndertimeModule::UpdateLighting sets the wind:
//   ShaderGlobal(2).xy = the direction it blows (x, z, normalized)
//   ShaderGlobal(2).z  = how strong it is now (0 calm .. 1 windy)
//   ShaderGlobal(2).w  = gust phase (radians, grows with time): gusts roll downwind
// How much a vertex moves:
//   trees (vertex alpha 1): by its height in the model (positionL.y), squared, over
//     param 0 metres, so the trunk's foot stays put and the crown bends;
//   ground cover (vertex alpha below 1, ValleyScenery bakes it so): 0.99 - alpha is the
//     weight (0 at the foot of a tuft, 0.98 at its tip).
//   param 1 = how far a tree's crown moves (metres), param 2 = how far a tuft's tip moves.
// The offset is a smooth function of position, so neighbouring faces move together (no cracks).
#define CHERRY_SURFACE_VERTEX_OFFSET 1
#define CHERRY_SURFACE_BOUNDS_PADDING 0.6
#include "VoxelModelSurface.hlsli"

float3 EvaluateVertexOffset(SurfaceInput input)
{
	const float4 wind = ShaderGlobal(2);
	if (wind.z <= 0.0)
		return 0.0;
	const bool cover = input.vertexColor.a < 0.995;
	float weight, reach;
	if (cover)
	{
		weight = saturate((0.99 - input.vertexColor.a) / 0.98);
		reach = ParamFloat(input, 2);
	}
	else
	{
		weight = saturate(input.positionL.y / max(ParamFloat(input, 0), 0.1));
		reach = ParamFloat(input, 1);
	}
	weight *= weight;
	if (weight <= 0.0)
		return 0.0;

	// Gusts: waves of wind rolling downwind across the valley (two of them, so it doesn't
	// look regular), on top of a steady lean; a quicker flutter for grass and leaves.
	const float2 p = input.positionW.xz;
	const float along = dot(p, wind.xy) * 0.07;
	const float gust = 0.5 + 0.3 * sin(along - wind.w) + 0.2 * sin(along * 2.3 - wind.w * 1.7 + 1.3);
	const float flutter = sin(input.time * (cover ? 4.0 : 2.2) + dot(p, float2(0.9, 1.3))) * (cover ? 0.35 : 0.15);
	const float bend = wind.z * reach * weight * (0.35 + 0.65 * gust + flutter);
	// Leaning over: along the wind, a little down (so blades don't stretch).
	return float3(wind.x * bend, -0.25 * abs(bend) * weight, wind.y * bend);
}
