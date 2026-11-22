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
	// A voxel model with glass (VoxelMaterial::glass) is drawn as two meshes: `mesh` is its
	// opaque part, `glassMesh` its glass (the engine loads "<meshAsset>#glass"), drawn with
	// `glassMaterial`: glassMaterialAsset, or the built-in translucent "engine/VoxelGlass".
	std::string glassMaterialAsset;
	MeshHandle glassMesh;
	MaterialHandle glassMaterial;
	std::string glassCheckedFor;   // the meshAsset the engine last looked for a glass part of (runtime only)
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
// One particle (Particles/ParticleSystem.h): it carries its own look and forces.
struct Particle
{
	DirectX::XMFLOAT3 position{ 0, 0, 0 };
	DirectX::XMFLOAT3 velocity{ 0, 0, 0 };
	float age = 0.0f, life = 1.0f;
	float size0 = 0.1f, size1 = 0.1f;
	DirectX::XMFLOAT4 color0{ 1, 1, 1, 1 }, color1{ 1, 1, 1, 0 };
	float gravity = 0.0f, drag = 0.0f, turbulence = 0.0f, emissive = 0.0f, rotation = 0.0f, stretch = 0.0f;
	bool additive = false, square = false;
};

// Spawns particles from its entity (at `offset` in the entity's space), lit like the scene
// and drawn as billboards. Also the settings for ParticleSystem::Burst.
struct ParticleEmitter
{
	bool enabled = true;
	float rate = 10.0f;                              // particles per second
	int maxParticles = 200;
	float lifetime = 2.0f;                           // seconds
	float lifetimeJitter = 0.3f;                     // +- share of the lifetime
	DirectX::XMFLOAT3 offset{ 0, 0, 0 };             // where they start, in the entity's space
	DirectX::XMFLOAT3 spawnBox{ 0, 0, 0 };           // +- metres around it (world axes)
	DirectX::XMFLOAT3 velocity{ 0, 0.5f, 0 };        // m/s, world
	DirectX::XMFLOAT3 velocityJitter{ 0.1f, 0.1f, 0.1f };
	float gravity = 0.0f;                            // m/s^2 down (negative: they rise)
	float drag = 0.0f;                               // how quickly they slow down (1/s)
	float turbulence = 0.0f;                         // m/s^2 of random push
	float sizeStart = 0.1f, sizeEnd = 0.1f;          // metres across
	float sizeJitter = 0.2f;                         // +- share
	DirectX::XMFLOAT4 colorStart{ 1, 1, 1, 1 };      // linear colour, alpha = opacity
	DirectX::XMFLOAT4 colorEnd{ 1, 1, 1, 0 };
	float emissive = 0.0f;                           // glow (x colour), e.g. sparks
	bool additive = false;                           // add light (sparks, glow) instead of covering
	bool square = false;                             // square blocks instead of soft round puffs
	float stretch = 0.0f;                            // seconds of motion drawn as a streak (rain, sparks); 0 = none

	// Runtime (not saved)
	std::vector<Particle> particles;
	float carry = 0.0f;
	uint32_t rng = 0;
};

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
	// The gradient sky with a sun disk and stars (RenderSettings::sky). Off: the background
	// is skyColor.
	bool skyEnabled = false;
	DirectX::XMFLOAT3 skyZenith{ 0.16f, 0.34f, 0.78f };
	DirectX::XMFLOAT3 skyHorizon{ 0.60f, 0.74f, 0.90f };
	DirectX::XMFLOAT3 skyGround{ 0.30f, 0.29f, 0.27f };
	float skyGradientPower = 0.5f;
	float sunSize = 0.6f;          // degrees
	float sunIntensity = 30.0f;
	float starIntensity = 0.0f;
	// Sun shadows (RenderSettings::shadows) and glow (RenderSettings::bloom).
	bool shadowsEnabled = false;
	float shadowNearDistance = 40.0f;
	float shadowFarDistance = 220.0f;
	bool bloomEnabled = false;
	float bloomIntensity = 0.06f;
	float bloomThreshold = 1.2f;
};

// Components a scene file has but this build doesn't know (e.g. a game component whose
// module isn't loaded): kept as JSON text so saving the scene doesn't lose them.
struct UnknownComponents
{
	std::vector<std::pair<std::string, std::string>> entries;   // component name, JSON
};
