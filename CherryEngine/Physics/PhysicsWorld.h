#pragma once
#include "CherryAPI.h"
#include <DirectXMath.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class DebugDraw;

// Rigid-body physics (Jolt Physics underneath, kept private to the engine).
// Bodies are referred to by integer handles; positions are metres, rotations quaternions.
using BodyId = uint32_t;
constexpr BodyId kInvalidBody = 0xFFFFFFFFu;

struct PhysicsBox
{
	DirectX::XMFLOAT3 center{ 0, 0, 0 };        // in the body's local space
	DirectX::XMFLOAT3 halfExtents{ 0.5f, 0.5f, 0.5f };
};

struct PhysicsRayHit
{
	BodyId body = kInvalidBody;
	float distance = 0.0f;
	DirectX::XMFLOAT3 point{ 0, 0, 0 };
	DirectX::XMFLOAT3 normal{ 0, 0, 0 };
	bool dynamic = false;
};

struct PhysicsStats
{
	uint32_t bodies = 0;
	uint32_t activeBodies = 0;
	uint32_t stepsLastFrame = 0;
	float stepMs = 0.0f;           // CPU time of the last frame's steps
};

// Owns the physics simulation. Steps at a fixed rate (with an accumulator) and keeps
// the previous and current transforms of dynamic bodies so rendering can interpolate.
class CHERRY_API PhysicsWorld
{
public:
	PhysicsWorld();
	~PhysicsWorld();
	PhysicsWorld(const PhysicsWorld&) = delete;
	PhysicsWorld& operator=(const PhysicsWorld&) = delete;

	// ---- Bodies -----------------------------------------------------------------
	// Static triangle mesh; `vertices` holds 3 per triangle, wound so that
	// (b - a) x (c - a) points out of the solid.
	BodyId CreateStaticMesh(const std::vector<DirectX::XMFLOAT3>& vertices);
	BodyId CreateStaticBox(const DirectX::XMFLOAT3& center, const DirectX::XMFLOAT3& halfExtents);
	// Dynamic body made of boxes in local space; `position`/`rotation` place the local origin.
	// `ignoreCharacters`: debris that collides with the world and other bodies but passes
	// through PhysicsCharacters (they neither push it nor stand on it).
	BodyId CreateDynamicBoxes(const std::vector<PhysicsBox>& boxes, float density,
		const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT4& rotation,
		const DirectX::XMFLOAT3& linearVelocity, const DirectX::XMFLOAT3& angularVelocity,
		bool ignoreCharacters = false);
	void RemoveBody(BodyId body);

	bool IsValid(BodyId body) const;
	bool IsSleeping(BodyId body) const;
	// Position/rotation of the body's local origin. `interpolated` blends the last two
	// fixed steps for smooth rendering at any frame rate.
	void GetTransform(BodyId body, DirectX::XMFLOAT3& position, DirectX::XMFLOAT4& rotation, bool interpolated = true) const;
	DirectX::XMFLOAT3 GetLinearVelocity(BodyId body) const;
	void AddImpulse(BodyId body, const DirectX::XMFLOAT3& impulse, const DirectX::XMFLOAT3& worldPoint);

	// `dynamicOnly`: skip the static world (still hits characters' inner bodies and debris).
	std::optional<PhysicsRayHit> Raycast(const DirectX::XMFLOAT3& origin, const DirectX::XMFLOAT3& direction, float maxDistance, bool dynamicOnly = false) const;

	// ---- Simulation -------------------------------------------------------------
	void SetGravity(float metresPerSecondSquared);
	float Gravity() const;
	const PhysicsStats& Stats() const;
	// Debug view: world-space bounds of every body.
	void DrawBounds(DebugDraw& debug) const;

	// ---- Engine-facing ----------------------------------------------------------
	void Initialize();
	void Shutdown();
	void Update(float deltaTime);   // runs 0..N fixed steps

	struct Impl;
	Impl& GetImpl() { return *mImpl; }

private:
	std::unique_ptr<Impl> mImpl;
};

// A kinematic first-person character (capsule) that walks on the physics world:
// slopes, stairs/ledges up to `stepHeight`, standing on and pushing dynamic bodies.
// Its feet are at the position; falling bodies collide with it.
class CHERRY_API PhysicsCharacter
{
public:
	struct Settings
	{
		float radius = 0.3f;
		float height = 1.7f;
		float stepHeight = 0.3f;
		float maxSlopeDegrees = 50.0f;
		float mass = 70.0f;
		float pushStrength = 400.0f;   // newtons it can push other bodies with
	};

	PhysicsCharacter(PhysicsWorld& world, const Settings& settings, const DirectX::XMFLOAT3& feetPosition);
	~PhysicsCharacter();
	PhysicsCharacter(const PhysicsCharacter&) = delete;
	PhysicsCharacter& operator=(const PhysicsCharacter&) = delete;

	// Moves for one frame. `horizontalVelocity` is the desired walk velocity (y ignored).
	void Update(float deltaTime, const DirectX::XMFLOAT3& horizontalVelocity, bool jump, float jumpSpeed);

	DirectX::XMFLOAT3 Position() const;   // feet
	DirectX::XMFLOAT3 Velocity() const;
	bool IsGrounded() const;
	void SetPosition(const DirectX::XMFLOAT3& feetPosition);

private:
	struct Impl;
	std::unique_ptr<Impl> mImpl;
};
