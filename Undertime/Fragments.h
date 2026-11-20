#pragma once
#include "VoxelWorld.h"
#include <Core/Engine.h>
#include <Physics/PhysicsWorld.h>
#include <DirectXMath.h>
#include <vector>

// A loose piece of rock: a small voxel grid that came out of the world.
struct FragmentVoxels
{
	DirectX::XMINT3 size{ 0, 0, 0 };
	DirectX::XMINT3 worldOrigin{ 0, 0, 0 };   // world voxel of local (0,0,0) when it broke off
	std::vector<cfg::Block> voxels;           // size.x * size.y * size.z, x fastest; Air = empty

	cfg::Block At(int x, int y, int z) const { return voxels[x + size.x * (y + size.y * z)]; }
	int SolidCount() const;
};

// Greedy decomposition of a fragment's voxels into boxes (local metres). If more than
// `maxBoxes` are needed, the voxels are merged 2x2x2 (then 4x4x4, ...) for the shape only.
std::vector<PhysicsBox> DecomposeIntoBoxes(const FragmentVoxels& fragment, int maxBoxes);

// Voxels knocked off a fragment by the pick, in that fragment's own voxel grid.
struct ChipResult
{
	bool hit = false;
	std::vector<VoxelWorld::RemovedVoxel> voxels;   // local voxel coordinates
	DirectX::XMINT3 worldOrigin{ 0, 0, 0 };          // palette/jitter origin of the local grid
	DirectX::XMFLOAT3 position{ 0, 0, 0 };           // world position of local (0,0,0)
	DirectX::XMFLOAT4 rotation{ 0, 0, 0, 1 };
	DirectX::XMFLOAT3 velocity{ 0, 0, 0 };
};

// Owns every loose boulder (rock that broke off in a cave-in): its physics body, its small
// voxel volume (rendered with the world's palette) and its age. Boulders never go back into
// the world: the pick breaks them up into loose blocks.
class FragmentSystem
{
public:
	void Initialize(VoxelVolumeHandle worldVolume);

	bool Spawn(Engine& engine, FragmentVoxels&& fragment, const DirectX::XMFLOAT3& position,
		const DirectX::XMFLOAT3& linearVelocity, const DirectX::XMFLOAT3& angularVelocity,
		const DirectX::XMFLOAT4& rotation = { 0, 0, 0, 1 });

	// Ages boulders and drops the ones that fell out of the world.
	void Update(Engine& engine, float deltaTime);

	// The pick hits a loose boulder: knocks a bite of voxels out of it around `point` and
	// splits what's left into its connected pieces (each respawned in place). Pieces too
	// small to matter are returned with the bite.
	ChipResult Chip(Engine& engine, BodyId body, const DirectX::XMFLOAT3& point, float pickFactor, uint32_t salt);

	// Past kMaxBoulders: removes the oldest boulder whole and returns all of its voxels (as
	// a ChipResult), to come apart into loose blocks.
	ChipResult ShatterOldestOverCap(Engine& engine);

	bool Owns(BodyId body) const;
	void Clear(Engine& engine);

	int Count() const { return static_cast<int>(mFragments.size()); }

private:
	struct Fragment
	{
		Entity entity = INVALID_ENTITY;
		BodyId body = kInvalidBody;
		VoxelVolumeHandle volume;
		FragmentVoxels voxels;
		float age = 0.0f;
	};

	void Release(Engine& engine, Fragment& fragment);

	std::vector<Fragment> mFragments;
	VoxelVolumeHandle mWorldVolume;
};
