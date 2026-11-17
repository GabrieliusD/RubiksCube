#pragma once
#include "VoxelWorld.h"
#include <Core/Input.h>
#include <DirectXMath.h>

// First-person controller: mouse look, WASD + jump, gravity, and per-axis AABB
// collision against the voxel world.
class PlayerController
{
public:
	DirectX::XMFLOAT3 position{ cfg::kSpawnX, cfg::kGroundY, cfg::kSpawnZ };   // feet, centre of the body
	DirectX::XMFLOAT3 velocity{ 0, 0, 0 };
	float yaw = 0.0f;      // radians, 0 = facing +Z (into the mountain)
	float pitch = 0.0f;    // radians, positive = looking down
	bool grounded = false;

	// `speedScale` slows walking (e.g. a heavy bag). `look` enables mouse look.
	void Update(const Input& input, const VoxelWorld& world, float deltaTime, float speedScale, bool look);

	DirectX::XMFLOAT3 EyePosition() const { return { position.x, position.y + cfg::kEyeHeight, position.z }; }
	DirectX::XMFLOAT3 LookDirection() const;
	DirectX::XMVECTOR Rotation() const;   // camera orientation quaternion

private:
	void MoveAxis(const VoxelWorld& world, int axis, float delta, bool canStep);
	bool TryStepUp(const VoxelWorld& world);
	void BodyBox(DirectX::XMFLOAT3& min, DirectX::XMFLOAT3& max) const;
};
