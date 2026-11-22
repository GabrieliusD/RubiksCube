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
	bool flying = false;   // debug (G): free flight through everything; Shift goes faster

	void CreateBody(PhysicsWorld& physics);
	// Moves the player (feet position) and sets the facing; clears velocity.
	void Teleport(const DirectX::XMFLOAT3& feet, float facingYaw);
	void DestroyBody();

	// `speedScale` slows walking (e.g. a heavy bag). `look` enables mouse look. Debug: Shift
	// moves faster; when flying, Space rises and Ctrl sinks.
	void Update(const Input& input, float deltaTime, float speedScale, bool look);

	DirectX::XMFLOAT3 EyePosition() const { return { position.x, position.y + cfg::kEyeHeight, position.z }; }
	DirectX::XMFLOAT3 LookDirection() const;
	DirectX::XMVECTOR Rotation() const;   // camera orientation quaternion

private:
	std::unique_ptr<PhysicsCharacter> mBody;
};
