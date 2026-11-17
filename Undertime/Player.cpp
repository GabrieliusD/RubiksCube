#include "Player.h"
#include <algorithm>
#include <cmath>

using namespace DirectX;

void PlayerController::CreateBody(PhysicsWorld& physics)
{
	PhysicsCharacter::Settings settings;
	settings.radius = cfg::kBodyWidth * 0.5f;
	settings.height = cfg::kBodyHeight;
	settings.stepHeight = cfg::kStepHeight;
	settings.maxSlopeDegrees = cfg::kCharacterMaxSlope;
	settings.pushStrength = cfg::kCharacterPushStrength;
	mBody = std::make_unique<PhysicsCharacter>(physics, settings, position);
}

void PlayerController::DestroyBody()
{
	mBody.reset();
}

void PlayerController::Update(const Input& input, float dt, float speedScale, bool look)
{
	if (look)
	{
		const XMFLOAT2 delta = input.MouseDelta();
		yaw += delta.x * cfg::kMouseSensitivity;
		pitch = std::clamp(pitch + delta.y * cfg::kMouseSensitivity, -1.55f, 1.55f);
	}
	if (!mBody) return;

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
	const XMFLOAT3 walk{ (s * forward + c * strafe) * speed, 0.0f, (c * forward - s * strafe) * speed };

	const bool jump = mBody->IsGrounded() && input.WasKeyPressed(VK_SPACE);
	mBody->Update(dt, walk, jump, cfg::kJumpSpeed);

	position = mBody->Position();
	velocity = mBody->Velocity();
	grounded = mBody->IsGrounded();
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
