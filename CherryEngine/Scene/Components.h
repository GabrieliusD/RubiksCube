#pragma once
#include "Renderer/RenderTypes.h"
#include <DirectXMath.h>

// Built-in components, registered by Scene. Games may register their own.

struct Transform
{
	DirectX::XMFLOAT3 position{ 0, 0, 0 };
	DirectX::XMFLOAT4 rotation{ 0, 0, 0, 1 };   // quaternion
	DirectX::XMFLOAT3 scale{ 1, 1, 1 };

	// World matrix in DirectXMath row-vector convention: scale, then rotate, then translate.
	DirectX::XMMATRIX Matrix() const
	{
		using namespace DirectX;
		return XMMatrixScaling(scale.x, scale.y, scale.z)
			* XMMatrixRotationQuaternion(XMLoadFloat4(&rotation))
			* XMMatrixTranslation(position.x, position.y, position.z);
	}

	// Angles in radians, applied roll (z), then pitch (x), then yaw (y).
	void SetRotationEuler(float pitch, float yaw, float roll)
	{
		DirectX::XMStoreFloat4(&rotation, DirectX::XMQuaternionRotationRollPitchYaw(pitch, yaw, roll));
	}

	// Rotates so +Z (forward) points from `position` towards `target`.
	void LookAt(const DirectX::XMFLOAT3& target, const DirectX::XMFLOAT3& up = { 0, 1, 0 })
	{
		using namespace DirectX;
		const XMVECTOR eye = XMLoadFloat3(&position);
		const XMVECTOR at = XMLoadFloat3(&target);
		if (XMVector3NearEqual(eye, at, XMVectorReplicate(1e-5f))) return;
		const XMMATRIX view = XMMatrixLookAtLH(eye, at, XMLoadFloat3(&up));
		const XMMATRIX world = XMMatrixTranspose(view);   // inverse of the pure rotation part
		XMStoreFloat4(&rotation, XMQuaternionNormalize(XMQuaternionRotationMatrix(world)));
	}

	DirectX::XMVECTOR Forward() const { return DirectX::XMVector3Rotate(DirectX::XMVectorSet(0, 0, 1, 0), DirectX::XMLoadFloat4(&rotation)); }
	DirectX::XMVECTOR Right() const   { return DirectX::XMVector3Rotate(DirectX::XMVectorSet(1, 0, 0, 0), DirectX::XMLoadFloat4(&rotation)); }
	DirectX::XMVECTOR Up() const      { return DirectX::XMVector3Rotate(DirectX::XMVectorSet(0, 1, 0, 0), DirectX::XMLoadFloat4(&rotation)); }
};

enum class RenderLayer : uint8_t
{
	World,       // normal scene geometry
	ViewModel,   // first-person items: drawn after the world with depth cleared, so they never clip into walls
};

struct MeshRenderer
{
	MeshHandle mesh;
	MaterialHandle material;
	bool visible = true;
	RenderLayer layer = RenderLayer::World;
};

// Perspective camera. The view matrix comes from the entity's Transform
// (view = inverse of the camera's world matrix without scale).
struct Camera
{
	float fovY = DirectX::XM_PIDIV4;
	float nearZ = 0.1f;
	float farZ = 1000.0f;
};

// A ray-marched voxel volume. Transform positions the volume's (0,0,0) corner;
// each voxel is `voxelSize` world units.
struct VoxelVolumeRenderer
{
	VoxelVolumeHandle volume;
	float voxelSize = 0.1f;
	bool visible = true;
};
