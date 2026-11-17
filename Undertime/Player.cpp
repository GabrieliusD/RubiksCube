#include "Player.h"
#include <algorithm>
#include <cmath>

using namespace DirectX;

void PlayerController::Update(const Input& input, const VoxelWorld& world, float dt, float speedScale, bool look)
{
	if (look)
	{
		const XMFLOAT2 delta = input.MouseDelta();
		yaw += delta.x * cfg::kMouseSensitivity;
		pitch = std::clamp(pitch + delta.y * cfg::kMouseSensitivity, -1.55f, 1.55f);
	}

	// Horizontal movement relative to where we're facing.
	float forward = 0.0f, strafe = 0.0f;
	if (input.IsKeyDown('W')) forward += 1.0f;
	if (input.IsKeyDown('S')) forward -= 1.0f;
	if (input.IsKeyDown('D')) strafe += 1.0f;
	if (input.IsKeyDown('A')) strafe -= 1.0f;
	const float length = std::sqrt(forward * forward + strafe * strafe);
	if (length > 0.0f)
	{
		forward /= length;
		strafe /= length;
	}
	const float speed = cfg::kWalkSpeed * speedScale;
	const float s = std::sin(yaw), c = std::cos(yaw);
	velocity.x = (s * forward + c * strafe) * speed;
	velocity.z = (c * forward - s * strafe) * speed;

	if (grounded && input.WasKeyPressed(VK_SPACE))
	{
		velocity.y = cfg::kJumpSpeed;
	}
	velocity.y = std::max(velocity.y - cfg::kGravity * dt, -cfg::kMaxFallSpeed);

	// Sub-step so no single move exceeds a quarter cell (no tunnelling through walls).
	const float maxStep = 0.25f;
	const float travel = std::max({ std::fabs(velocity.x), std::fabs(velocity.y), std::fabs(velocity.z) }) * dt;
	const int steps = std::max(1, static_cast<int>(std::ceil(travel / maxStep)));
	const float stepDt = dt / steps;
	grounded = false;
	for (int i = 0; i < steps; ++i)
	{
		MoveAxis(world, 1, velocity.y * stepDt);
		MoveAxis(world, 0, velocity.x * stepDt);
		MoveAxis(world, 2, velocity.z * stepDt);
	}
}

void PlayerController::BodyBox(XMFLOAT3& mn, XMFLOAT3& mx) const
{
	const float half = cfg::kBodyWidth * 0.5f;
	mn = { position.x - half, position.y, position.z - half };
	mx = { position.x + half, position.y + cfg::kBodyHeight, position.z + half };
}

void PlayerController::MoveAxis(const VoxelWorld& world, int axis, float delta)
{
	if (delta == 0.0f) return;
	float* p = &position.x;
	float* v = &velocity.x;
	p[axis] += delta;

	XMFLOAT3 mn, mx;
	BodyBox(mn, mx);
	if (!world.BoxOverlapsSolid(mn, mx)) return;

	// Snap against the face of the cell we moved into. Moves are < 1 cell, so the
	// blocking face is the cell boundary the leading edge crossed.
	const float eps = 1e-3f;
	const float* boxMin = &mn.x;
	const float* boxMax = &mx.x;
	if (delta > 0.0f)
	{
		const float face = std::floor(boxMax[axis]);
		p[axis] -= (boxMax[axis] - face) + eps;
	}
	else
	{
		const float face = std::floor(boxMin[axis]) + 1.0f;
		p[axis] += (face - boxMin[axis]) + eps;
		if (axis == 1) grounded = true;
	}
	v[axis] = 0.0f;
}

XMFLOAT3 PlayerController::LookDirection() const
{
	XMFLOAT3 dir;
	XMStoreFloat3(&dir, XMVector3Rotate(XMVectorSet(0, 0, 1, 0), Rotation()));
	return dir;
}

XMVECTOR PlayerController::Rotation() const
{
	return XMQuaternionRotationRollPitchYaw(pitch, yaw, 0.0f);
}
