// Ray-marched voxel volume.
// The volume's bounding box is rasterized; each covered pixel marches the view
// ray through the voxel grid with an Amanatides-Woo DDA and writes SV_Depth, so
// voxels and raster meshes depth-test against each other in the same buffer.
// (A D3D12 depth buffer can't be bound as a UAV, which is why this is a pixel
// shader rather than a compute shader.)
#include "Common.hlsli"
#include "LightingUtil.hlsl"

ConstantBuffer<VoxelDrawConstants> gDraw : register(b0);

// Outward faces are wound counter-clockwise as seen from outside; the pipeline
// culls back faces so only the far side of the box is rasterized (one ray per pixel).
static const uint kCubeIndices[36] =
{
	0, 2, 6, 0, 6, 4,   // -X
	1, 5, 7, 1, 7, 3,   // +X
	0, 4, 5, 0, 5, 1,   // -Y
	2, 3, 7, 2, 7, 6,   // +Y
	0, 1, 3, 0, 3, 2,   // -Z
	4, 6, 7, 4, 7, 5,   // +Z
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
	uint corner = kCubeIndices[vertexId];
	float3 local = float3(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1) * float3(volume.dims);

	VertexOut vout;
	float4 positionW = mul(float4(local, 1.0), volume.world);
	vout.positionW = positionW.xyz;
	vout.position = mul(positionW, gView.viewProj);
	return vout;
}

struct PixelOut
{
	float4 color : SV_Target;
	float depth : SV_Depth;
};

PixelOut PSMain(VertexOut pin)
{
	VoxelVolumeData volume = LoadVolume();
	Texture3D<uint> voxels = ResourceDescriptorHeap[volume.volumeTextureIndex];
	StructuredBuffer<float4> palette = ResourceDescriptorHeap[volume.paletteBufferIndex];

	// World ray; transformed to volume space without renormalising, so `t` stays a world distance.
	float3 rayDirW = normalize(pin.positionW - gView.eyePosW);
	float3 origin = mul(float4(gView.eyePosW, 1.0), volume.invWorld).xyz;
	float3 dir = mul(float4(rayDirW, 0.0), volume.invWorld).xyz;
	float3 dims = float3(volume.dims);

	// Slab test against [0, dims].
	float3 invDir = 1.0 / select(abs(dir) < 1e-8, 1e-8, dir);
	float3 t0 = (0.0 - origin) * invDir;
	float3 t1 = (dims - origin) * invDir;
	float3 tNear = min(t0, t1);
	float3 tFar = max(t0, t1);
	float tEnter = max(max(tNear.x, tNear.y), tNear.z);
	float tExit = min(min(tFar.x, tFar.y), tFar.z);
	if (tExit < max(tEnter, 0.0))
	{
		discard;
	}

	// Axis we entered through (for the first voxel's normal).
	int axis = tNear.x >= tNear.y ? (tNear.x >= tNear.z ? 0 : 2) : (tNear.y >= tNear.z ? 1 : 2);
	float t = max(tEnter, 0.0);

	float3 p = origin + dir * (t + 1e-4);
	int3 cell = clamp(int3(floor(p)), int3(0, 0, 0), int3(volume.dims) - 1);
	int3 stepDir = int3(select(dir >= 0.0, 1, -1));
	float3 tDelta = abs(invDir);
	float3 nextBoundary = float3(cell) + float3(select(stepDir > 0, 1.0, 0.0));
	float3 tMax = (nextBoundary - origin) * invDir;

	uint hitValue = 0;
	const uint maxSteps = volume.dims.x + volume.dims.y + volume.dims.z;
	for (uint i = 0; i < maxSteps; ++i)
	{
		hitValue = voxels.Load(int4(cell, 0));
		if (hitValue != 0)
		{
			break;
		}

		if (tMax.x < tMax.y && tMax.x < tMax.z)
		{
			cell.x += stepDir.x; t = tMax.x; tMax.x += tDelta.x; axis = 0;
		}
		else if (tMax.y < tMax.z)
		{
			cell.y += stepDir.y; t = tMax.y; tMax.y += tDelta.y; axis = 1;
		}
		else
		{
			cell.z += stepDir.z; t = tMax.z; tMax.z += tDelta.z; axis = 2;
		}

		if (any(cell < 0) || any(cell >= int3(volume.dims)) || t > tExit)
		{
			break;
		}
	}

	if (hitValue == 0)
	{
		discard;
	}

	float3 normalL = 0;
	normalL[axis] = -float(stepDir[axis]);
	// Normals transform by the inverse-transpose: n_w = n_l * transpose(invWorld).
	float3 normalW = normalize(mul((float3x3)volume.invWorld, normalL));

	float3 hitW = gView.eyePosW + rayDirW * t;
	float4 albedo = palette[hitValue];

	Material mat;
	mat.DiffuseAlbedo = albedo;
	mat.FresnelR0 = float3(0.02, 0.02, 0.02);
	mat.Shininess = 0.1;
	float3 toEye = -rayDirW;
	float3 color = gFrame.ambientLight.rgb * albedo.rgb
		+ ComputeLighting(gFrame.lights, gFrame.numDirLights, mat, normalW, toEye);

	float4 clip = mul(float4(hitW, 1.0), gView.viewProj);

	PixelOut pout;
	pout.color = float4(color, 1.0);
	pout.depth = clip.z / clip.w;
	return pout;
}
