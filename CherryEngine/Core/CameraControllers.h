#pragma once
#include "Core/Input.h"
#include "Scene/Components.h"
#include <algorithm>
#include <cmath>

// Ready-made camera controllers a game can drive from OnUpdate.

// Orbits a target point: drag with `rotateButton` to rotate, wheel to zoom.
struct OrbitCameraController
{
	DirectX::XMFLOAT3 target{ 0, 0, 0 };
	float distance = 15.0f;
	float yaw = 0.6f;            // radians around +Y
	float pitch = 0.45f;         // radians above the horizon
	float minDistance = 2.0f;
	float maxDistance = 200.0f;
	float rotateSpeed = 0.006f;  // radians per pixel
	MouseButton rotateButton = MouseButton::Left;

	void Update(const Input& input, Transform& camera)
	{
		if (input.IsMouseDown(rotateButton))
		{
			const DirectX::XMFLOAT2 delta = input.MouseDelta();
			yaw += delta.x * rotateSpeed;
			pitch = std::clamp(pitch + delta.y * rotateSpeed, -1.5f, 1.5f);
		}
		const float wheel = input.WheelDelta();
		if (wheel != 0.0f)
		{
			distance = std::clamp(distance * std::pow(0.88f, wheel), minDistance, maxDistance);
		}
		Apply(camera);
	}

	void Apply(Transform& camera) const
	{
		const float cp = std::cos(pitch);
		camera.position =
		{
			target.x - distance * cp * std::sin(yaw),
			target.y + distance * std::sin(pitch),
			target.z - distance * cp * std::cos(yaw),
		};
		camera.LookAt(target);
	}
};

// Free-fly camera: hold right mouse to look, WASD to move, Q/E down/up, Shift to go faster.
struct FlyCameraController
{
	float yaw = 0.0f;
	float pitch = 0.0f;
	float speed = 8.0f;
	float lookSpeed = 0.004f;

	void Update(const Input& input, Transform& camera, float deltaTime)
	{
		using namespace DirectX;
		if (input.IsMouseDown(MouseButton::Right))
		{
			const XMFLOAT2 delta = input.MouseDelta();
			yaw += delta.x * lookSpeed;
			pitch = std::clamp(pitch + delta.y * lookSpeed, -1.55f, 1.55f);
		}
		camera.SetRotationEuler(pitch, yaw, 0.0f);

		XMVECTOR move = XMVectorZero();
		if (input.IsKeyDown('W')) move += camera.Forward();
		if (input.IsKeyDown('S')) move -= camera.Forward();
		if (input.IsKeyDown('D')) move += camera.Right();
		if (input.IsKeyDown('A')) move -= camera.Right();
		if (input.IsKeyDown('E')) move += XMVectorSet(0, 1, 0, 0);
		if (input.IsKeyDown('Q')) move -= XMVectorSet(0, 1, 0, 0);
		const float boost = input.IsKeyDown(VK_SHIFT) ? 4.0f : 1.0f;
		XMVECTOR position = XMLoadFloat3(&camera.position) + XMVector3Normalize(move) * speed * boost * deltaTime;
		if (!XMVector3Equal(move, XMVectorZero()))
		{
			XMStoreFloat3(&camera.position, position);
		}
	}
};
