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

// Snaps a fragment's voxels at the given pose into world voxel cells (pure helper, testable).
struct BakedVoxel
{
	int x, y, z;
	cfg::Block block;
};
std::vector<BakedVoxel> SnapToGrid(const FragmentVoxels& fragment, const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT4& rotation);

// Owns every loose fragment: its physics body, its small voxel volume (rendered with the
// world's palette) and its lifetime. Fragments bake back into the world when they rest.
class FragmentSystem
{
public:
	void Initialize(VoxelVolumeHandle worldVolume);

	bool Spawn(Engine& engine, FragmentVoxels&& fragment, const DirectX::XMFLOAT3& position,
		const DirectX::XMFLOAT3& linearVelocity, const DirectX::XMFLOAT3& angularVelocity);

	// Bakes fragments that are asleep (or too old) into `world`. The world boxes that changed
	// are appended to `changed`. Cells overlapping `keepClearMin/Max` (the player) are skipped.
	void Update(Engine& engine, VoxelWorld& world, float deltaTime,
		const DirectX::XMFLOAT3& keepClearMin, const DirectX::XMFLOAT3& keepClearMax,
		std::vector<VoxelWorld::Box>& changed);

	bool Owns(BodyId body) const;
	void Clear(Engine& engine);

	int Count() const { return static_cast<int>(mFragments.size()); }
	int Baked() const { return mBakedCount; }
	int Vanished() const { return mVanishedCount; }
	float LastBakeMs() const { return mLastBakeMs; }

private:
	struct Fragment
	{
		Entity entity = INVALID_ENTITY;
		BodyId body = kInvalidBody;
		VoxelVolumeHandle volume;
		FragmentVoxels voxels;
		float age = 0.0f;
	};

	void Bake(Engine& engine, VoxelWorld& world, Fragment& fragment,
		const DirectX::XMFLOAT3& keepClearMin, const DirectX::XMFLOAT3& keepClearMax,
		std::vector<VoxelWorld::Box>& changed);
	void Release(Engine& engine, Fragment& fragment);

	std::vector<Fragment> mFragments;
	VoxelVolumeHandle mWorldVolume;
	int mBakedCount = 0;
	int mVanishedCount = 0;
	float mLastBakeMs = 0.0f;
};
