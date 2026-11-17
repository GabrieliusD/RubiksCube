#pragma once
#include "Config.h"
#include <Core/Input.h>
#include <Physics/PhysicsWorld.h>
#include <DirectXMath.h>
#include <memory>

// First-person controller: mouse look plus WASD + jump, moving an engine PhysicsCharacter
// (Jolt), so the player walks on the voxel world, climbs low ledges, stands on and pushes
// loose rock, and gets hit by falling rock.
class PlayerController
{
public:
	DirectX::XMFLOAT3 position{ cfg::kSpawnX, cfg::kGroundY, cfg::kSpawnZ };   // feet, centre of the body
	DirectX::XMFLOAT3 velocity{ 0, 0, 0 };
	float yaw = 0.0f;      // radians, 0 = facing +Z (into the mountain)
	float pitch = 0.0f;    // radians, positive = looking down
	bool grounded = false;

	void CreateBody(PhysicsWorld& physics);
	void DestroyBody();

	// `speedScale` slows walking (e.g. a heavy bag). `look` enables mouse look.
	void Update(const Input& input, float deltaTime, float speedScale, bool look);

	DirectX::XMFLOAT3 EyePosition() const { return { position.x, position.y + cfg::kEyeHeight, position.z }; }
	DirectX::XMFLOAT3 LookDirection() const;
	DirectX::XMVECTOR Rotation() const;   // camera orientation quaternion

private:
	std::unique_ptr<PhysicsCharacter> mBody;
};
