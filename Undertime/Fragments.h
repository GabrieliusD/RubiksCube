#pragma once
#include "VoxelWorld.h"
#include <Core/Engine.h>
#include <Physics/PhysicsWorld.h>
#include <DirectXMath.h>
#include <array>
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

// What happens to a fragment once it stops moving.
enum class FragmentKind
{
	Structural,   // rock that broke off (cave-ins, undercut overhangs): a loose boulder the pick can break up
	Rubble,       // chips knocked out by the pick: crumble away to nothing
	Nugget,       // ore knocked out by the pick: pulled to the player and collected
};

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

// Owns every loose fragment: its physics body, its small voxel volume (rendered with the
// world's palette) and its lifetime.
//  - Structural fragments stay loose as boulders you can break with the pick; only very big
//    ones (and the oldest, past the cap) bake back into the world when they rest.
//  - Rubble shrinks away once it settles, so mined rock never re-solidifies in the hole.
//  - Nuggets are pulled to the player and collected.
class FragmentSystem
{
public:
	void Initialize(VoxelVolumeHandle worldVolume);

	bool Spawn(Engine& engine, FragmentVoxels&& fragment, const DirectX::XMFLOAT3& position,
		const DirectX::XMFLOAT3& linearVelocity, const DirectX::XMFLOAT3& angularVelocity,
		FragmentKind kind = FragmentKind::Structural, const DirectX::XMFLOAT4& rotation = { 0, 0, 0, 1 });

	// Bakes resting fragments that should bake into `world`. The world boxes that changed
	// are appended to `changed`. Cells overlapping `keepClearMin/Max` (the player) are skipped.
	void Update(Engine& engine, VoxelWorld& world, float deltaTime,
		const DirectX::XMFLOAT3& keepClearMin, const DirectX::XMFLOAT3& keepClearMax,
		std::vector<VoxelWorld::Box>& changed);

	// Pulls nearby nuggets towards `collector` and collects the ones that reach it. Adds the
	// collected voxels per block type to `collected`; returns how many nuggets were collected.
	int CollectNuggets(Engine& engine, const DirectX::XMFLOAT3& collector, float deltaTime,
		std::array<int, static_cast<int>(cfg::Block::Count)>& collected);

	// The pick hits a loose boulder: knocks a bite of voxels out of it around `point` and
	// splits what's left into its connected pieces (each respawned in place). Pieces too
	// small to matter are returned with the bite.
	ChipResult Chip(Engine& engine, BodyId body, const DirectX::XMFLOAT3& point, float pickFactor, uint32_t salt);

	// True for structural fragments only (rubble and nuggets aren't worth aiming the pick at).
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
		FragmentKind kind = FragmentKind::Structural;
		float age = 0.0f;
		float still = 0.0f;      // seconds it has been (nearly) motionless
		float crumble = -1.0f;   // rubble: seconds into crumbling away (< 0 = not yet)
		float pull = -1.0f;      // nugget: seconds being pulled to the player (< 0 = not yet)
		bool forceBake = false;  // structural: bake on the next update (over the cap)
	};

	void Bake(Engine& engine, VoxelWorld& world, Fragment& fragment,
		const DirectX::XMFLOAT3& keepClearMin, const DirectX::XMFLOAT3& keepClearMax,
		std::vector<VoxelWorld::Box>& changed);
	void Release(Engine& engine, Fragment& fragment);
	int CountKind(FragmentKind kind) const;

	std::vector<Fragment> mFragments;
	VoxelVolumeHandle mWorldVolume;
	int mBakedCount = 0;
	int mVanishedCount = 0;
	float mLastBakeMs = 0.0f;
};
