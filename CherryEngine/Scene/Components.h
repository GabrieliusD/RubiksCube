#pragma once
#include "Renderer/RenderTypes.h"
#include "Physics/PhysicsWorld.h"
#include "Scene/ECS.h"
#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Built-in components, registered by Scene and reflected in Scene/Reflection.cpp
// (that field list drives scene files, the play-world copy and the inspector).
// Games may register their own.

// Identity of an entity in scene files and the editor. Entities created by game code
// at runtime usually don't have one; scene files give every saved entity one.
struct EntityInfo
{
	uint64_t guid = 0;      // stable across saves; entity references are saved as guids
	std::string name;
	std::string prefab;     // the prefab this entity is the root of (Content path), if any
};

// Makes the entity's Transform relative to another entity's (see Scene::WorldMatrix).
// Rigid bodies ignore it: physics writes their Transform as a world pose.
struct Parent
{
	Entity parent = INVALID_ENTITY;
};

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
	// Assets by path (e.g. "engine/Cube"). Saved in scene files; the engine loads them and
	// fills `mesh` / `material` whenever those handles are unset. Game code that makes its
	// own meshes leaves these empty and sets the handles directly.
	std::string meshAsset;
	std::string materialAsset;
};

// Perspective camera. The view matrix comes from the entity's Transform
// (view = inverse of the camera's world matrix without scale).
struct Camera
{
	float fovY = DirectX::XM_PIDIV4;
	float nearZ = 0.1f;
	float farZ = 1000.0f;
};

// Drives the entity's Transform from a physics body every frame (interpolated between
// fixed physics steps). Create the body with Engine::GetPhysics().
struct RigidBody
{
	BodyId body = kInvalidBody;
};

// A ray-marched voxel volume. Transform positions the volume's (0,0,0) corner;
// each voxel is `voxelSize` world units.
struct VoxelVolumeRenderer
{
	VoxelVolumeHandle volume;
	float voxelSize = 0.1f;
	bool visible = true;
	float colorJitter = 0.0f;   // +- brightness variation per voxel (e.g. 0.15 = 15%)
	float aoStrength = 0.0f;    // corner ambient occlusion, 0..1
	DirectX::XMINT3 jitterOffset{ 0, 0, 0 };   // added to voxel coords for the jitter pattern (keeps a
	                                           // fragment's look when it breaks off a larger volume)
	// Optional material: its surface shader (e.g. a shader graph) shades the voxels, with the
	// palette colour as its Vertex Color. Without one, the built-in look (palette colour, rough
	// stone). As with MeshRenderer, the engine fills `material` from `materialAsset` when unset.
	MaterialHandle material;
	std::string materialAsset;
};

// A point light at the entity's world position (added to RenderSettings::pointLights).
struct PointLightComponent
{
	DirectX::XMFLOAT3 color{ 1.0f, 0.9f, 0.75f };   // linear
	float intensity = 4.0f;
	float range = 8.0f;
};

// Scene-wide lighting and look, saved with the scene. In the editor the renderer settings
// follow the first Environment in the edit scene every frame; on Play they're applied once,
// before the game's OnStartup (which may change them).
struct Environment
{
	DirectX::XMFLOAT4 skyColor{ 0.35f, 0.48f, 0.75f, 1.0f };   // linear HDR clear colour
	DirectX::XMFLOAT3 ambient{ 0.25f, 0.25f, 0.32f };
	float ambientContrast = 0.35f;   // RenderSettings::ambientContrast
	DirectX::XMFLOAT3 sunDirection{ 0.57735f, -0.57735f, 0.57735f };
	DirectX::XMFLOAT3 sunColor{ 1.0f, 0.97f, 0.9f };
	float fillLightScale = 1.0f;
	bool fogEnabled = false;
	DirectX::XMFLOAT3 fogColor{ 0.35f, 0.48f, 0.75f };
	float fogStart = 50.0f;
	float fogEnd = 200.0f;
	float exposure = 1.0f;
};

// Components a scene file has but this build doesn't know (e.g. a game component whose
// module isn't loaded): kept as JSON text so saving the scene doesn't lose them.
struct UnknownComponents
{
	std::vector<std::pair<std::string, std::string>> entries;   // component name, JSON
};
