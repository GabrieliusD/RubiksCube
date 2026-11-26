// VoxelShadow.hlsl
// Voxel volumes casting sun shadows: drawn into a shadow map like VoxelPass draws into the
// view, but each pixel marches a ray along the sunlight (the shadow view is orthographic, so
// every ray is parallel) and writes the depth of the first solid voxel.
#include "Common.hlsli"

CHERRY_PUSH_CONSTANTS ConstantBuffer<VoxelDrawConstants> gDraw : register(b0);

// The volume's box, outward faces wound counter-clockwise (see VoxelPass.hlsli).
static const uint kCubeIndices[36] =
{
	0, 2, 6, 0, 6, 4,
	1, 5, 7, 1, 7, 3,
	0, 4, 5, 0, 5, 1,
	2, 3, 7, 2, 7, 6,
	0, 1, 3, 0, 3, 2,
	4, 6, 7, 4, 7, 5,
};

VoxelVolumeData LoadVolume()
{
	StructuredBuffer<VoxelVolumeData> volumes = ResourceDescriptorHeap[gFrame.voxelVolumeBufferIndex];
	return volumes[gDraw.volumeIndex];
}

struct VertexOut
{
	float4 position : SV_POSITION;
	float3 positionW : POSITION;
};

VertexOut VSMain(uint vertexId : SV_VertexID)
{
	VoxelVolumeData volume = LoadVolume();
	const uint corner = kCubeIndices[vertexId];
	const float3 local = float3(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1) * float3(volume.dims);
	VertexOut vout;
	const float4 positionW = mul(float4(local, 1.0), volume.world);
	vout.positionW = positionW.xyz;
	vout.position = mul(positionW, gView.viewProj);
	return vout;
}

float PSMain(VertexOut pin) : SV_Depth
{
	VoxelVolumeData volume = LoadVolume();
	Texture3D<uint> voxels = ResourceDescriptorHeap[volume.volumeTextureIndex];

	// The sunlight ray through this pixel, from well before the volume.
	const float3 rayDirW = normalize(gFrame.lights[0].Direction);
	const float3 startW = pin.positionW - rayDirW * 4096.0;
	const float3 origin = mul(float4(startW, 1.0), volume.invWorld).xyz;
	const float3 dir = mul(float4(rayDirW, 0.0), volume.invWorld).xyz;
	const float3 dims = float3(volume.dims);

	const float3 invDir = 1.0 / select(abs(dir) < 1e-8, 1e-8, dir);
	const float3 t0 = (0.0 - origin) * invDir;
	const float3 t1 = (dims - origin) * invDir;
	const float3 tNear = min(t0, t1);
	const float3 tFar = max(t0, t1);
	const float tEnter = max(max(tNear.x, tNear.y), tNear.z);
	const float tExit = min(min(tFar.x, tFar.y), tFar.z);
	if (tExit < max(tEnter, 0.0))
		discard;

	float t = max(tEnter, 0.0);
	const float3 p = origin + dir * (t + 1e-4);
	int3 cell = clamp(int3(floor(p)), int3(0, 0, 0), int3(volume.dims) - 1);
	const int3 stepDir = int3(select(dir >= 0.0, 1, -1));
	const float3 tDelta = abs(invDir);
	float3 tMax = (float3(cell) + float3(select(stepDir > 0, 1.0, 0.0)) - origin) * invDir;

	bool hit = false;
	const uint maxSteps = volume.dims.x + volume.dims.y + volume.dims.z;
	for (uint i = 0; i < maxSteps; ++i)
	{
		if (voxels.Load(int4(cell, 0)) != 0) { hit = true; break; }
		if (tMax.x < tMax.y && tMax.x < tMax.z) { cell.x += stepDir.x; t = tMax.x; tMax.x += tDelta.x; }
		else if (tMax.y < tMax.z) { cell.y += stepDir.y; t = tMax.y; tMax.y += tDelta.y; }
		else { cell.z += stepDir.z; t = tMax.z; tMax.z += tDelta.z; }
		if (any(cell < 0) || any(cell >= int3(volume.dims)) || t > tExit) break;
	}
	if (!hit)
		discard;

	const float3 hitW = startW + rayDirW * t;
	return mul(float4(hitW, 1.0), gView.viewProj).z;
}
