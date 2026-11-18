#include "Scene/Scene.h"
#include <algorithm>
#include <random>

using namespace DirectX;

namespace
{
	template <typename A, typename B>
	Signature MakeSignature(Coordinator& coordinator)
	{
		Signature signature;
		signature.set(coordinator.GetComponentType<A>());
		signature.set(coordinator.GetComponentType<B>());
		return signature;
	}
}

Scene::Scene()
{
	mCoordinator.Init();
	mCoordinator.RegisterComponent<Transform>();
	mCoordinator.RegisterComponent<MeshRenderer>();
	mCoordinator.RegisterComponent<Camera>();
	mCoordinator.RegisterComponent<VoxelVolumeRenderer>();
	mCoordinator.RegisterComponent<RigidBody>();
	mCoordinator.RegisterComponent<EntityInfo>();
	mCoordinator.RegisterComponent<Parent>();
	mCoordinator.RegisterComponent<PointLightComponent>();
	mCoordinator.RegisterComponent<Environment>();
	mCoordinator.RegisterComponent<UnknownComponents>();
	mParentType = mCoordinator.GetComponentType<Parent>();
	mInfoType = mCoordinator.GetComponentType<EntityInfo>();

	mMeshes = mCoordinator.RegisterSystem<MeshRendererSet>();
	mVoxels = mCoordinator.RegisterSystem<VoxelVolumeSet>();
	mCameras = mCoordinator.RegisterSystem<CameraSet>();
	mRigidBodies = mCoordinator.RegisterSystem<RigidBodySet>();
	mPointLights = mCoordinator.RegisterSystem<PointLightSet>();

	mCoordinator.SetSystemSignature<MeshRendererSet>(MakeSignature<Transform, MeshRenderer>(mCoordinator));
	mCoordinator.SetSystemSignature<VoxelVolumeSet>(MakeSignature<Transform, VoxelVolumeRenderer>(mCoordinator));
	mCoordinator.SetSystemSignature<CameraSet>(MakeSignature<Transform, Camera>(mCoordinator));
	mCoordinator.SetSystemSignature<RigidBodySet>(MakeSignature<Transform, RigidBody>(mCoordinator));
	mCoordinator.SetSystemSignature<PointLightSet>(MakeSignature<Transform, PointLightComponent>(mCoordinator));

	RegisterReflectedTypes();
}

Scene::~Scene() = default;

void Scene::RegisterReflectedTypes()
{
	for (const auto& info : TypeRegistry::Get().Components())
	{
		info->registerIn(*this);
	}
}

Entity Scene::CreateEntity()
{
	Entity entity = mCoordinator.CreateEntity();
	mAllEntities.push_back(entity);
	return entity;
}

Entity Scene::CreateNamedEntity(const std::string& name)
{
	const Entity entity = CreateEntity();
	EntityInfo info;
	info.guid = NewGuid();
	info.name = name;
	Add(entity, info);
	Add(entity, Transform{});
	return entity;
}

void Scene::DestroyEntity(Entity entity)
{
	if (!mCoordinator.IsAlive(entity)) return;
	mCoordinator.DestroyEntity(entity);
	mAllEntities.erase(std::remove(mAllEntities.begin(), mAllEntities.end(), entity), mAllEntities.end());
}

void Scene::Clear()
{
	std::vector<Entity> entities = mAllEntities;
	for (Entity entity : entities)
	{
		DestroyEntity(entity);
	}
}

void Scene::MoveEntity(Entity entity, size_t index)
{
	auto it = std::find(mAllEntities.begin(), mAllEntities.end(), entity);
	if (it == mAllEntities.end()) return;
	mAllEntities.erase(it);
	index = std::min(index, mAllEntities.size());
	mAllEntities.insert(mAllEntities.begin() + static_cast<std::ptrdiff_t>(index), entity);
}

Entity Scene::GetParent(Entity entity)
{
	if (!IsAlive(entity) || !mCoordinator.GetSignature(entity).test(mParentType)) return INVALID_ENTITY;
	const Entity parent = Get<Parent>(entity).parent;
	return IsAlive(parent) ? parent : INVALID_ENTITY;
}

XMMATRIX Scene::WorldMatrix(Entity entity)
{
	XMMATRIX world = Has<Transform>(entity) ? Get<Transform>(entity).Matrix() : XMMatrixIdentity();
	// Bounded walk: a broken file can't hang the frame with a parent cycle.
	Entity parent = GetParent(entity);
	for (int depth = 0; parent != INVALID_ENTITY && depth < 64; ++depth)
	{
		if (Has<Transform>(parent)) world = world * Get<Transform>(parent).Matrix();
		parent = GetParent(parent);
	}
	return world;
}

XMFLOAT3 Scene::WorldPosition(Entity entity)
{
	XMFLOAT3 position;
	XMStoreFloat3(&position, WorldMatrix(entity).r[3]);
	return position;
}

bool Scene::IsAncestor(Entity ancestor, Entity entity)
{
	for (int depth = 0; entity != INVALID_ENTITY && depth < 64; ++depth)
	{
		if (entity == ancestor) return true;
		entity = GetParent(entity);
	}
	return false;
}

bool Scene::SetParent(Entity entity, Entity parent, bool keepWorld)
{
	if (!IsAlive(entity)) return false;
	if (parent != INVALID_ENTITY && (!IsAlive(parent) || IsAncestor(entity, parent))) return false;

	const XMMATRIX world = WorldMatrix(entity);
	if (parent == INVALID_ENTITY)
	{
		if (Has<Parent>(entity)) Remove<Parent>(entity);
	}
	else
	{
		Add(entity, Parent{ parent });
	}

	if (keepWorld && Has<Transform>(entity))
	{
		const XMMATRIX parentWorld = parent == INVALID_ENTITY ? XMMatrixIdentity() : WorldMatrix(parent);
		const XMMATRIX local = world * XMMatrixInverse(nullptr, parentWorld);
		XMVECTOR scale, rotation, translation;
		if (XMMatrixDecompose(&scale, &rotation, &translation, local))
		{
			Transform& transform = Get<Transform>(entity);
			XMStoreFloat3(&transform.scale, scale);
			XMStoreFloat4(&transform.rotation, XMQuaternionNormalize(rotation));
			XMStoreFloat3(&transform.position, translation);
		}
	}
	return true;
}

std::vector<Entity> Scene::Children(Entity entity)
{
	std::vector<Entity> children;
	for (Entity candidate : mAllEntities)
	{
		if (GetParent(candidate) == entity) children.push_back(candidate);
	}
	return children;
}

void Scene::DestroyTree(Entity entity)
{
	if (!IsAlive(entity)) return;
	for (Entity child : Children(entity))
	{
		DestroyTree(child);
	}
	DestroyEntity(entity);
}

uint64_t Scene::NewGuid()
{
	static std::mt19937_64 generator(std::random_device{}() ^ (static_cast<uint64_t>(std::random_device{}()) << 32));
	uint64_t guid = 0;
	while (guid == 0) guid = generator();
	return guid;
}

uint64_t Scene::GetGuid(Entity entity)
{
	if (!IsAlive(entity) || !mCoordinator.GetSignature(entity).test(mInfoType)) return 0;
	return Get<EntityInfo>(entity).guid;
}

uint64_t Scene::EnsureGuid(Entity entity)
{
	if (!IsAlive(entity)) return 0;
	if (!Has<EntityInfo>(entity)) Add(entity, EntityInfo{});
	EntityInfo& info = Get<EntityInfo>(entity);
	if (info.guid == 0) info.guid = NewGuid();
	return info.guid;
}

Entity Scene::FindByGuid(uint64_t guid)
{
	if (guid == 0) return INVALID_ENTITY;
	for (Entity entity : mAllEntities)
	{
		if (GetGuid(entity) == guid) return entity;
	}
	return INVALID_ENTITY;
}

std::string Scene::DisplayName(Entity entity)
{
	if (IsAlive(entity) && mCoordinator.GetSignature(entity).test(mInfoType))
	{
		const std::string& name = Get<EntityInfo>(entity).name;
		if (!name.empty()) return name;
	}
	return "Entity " + std::to_string(entity);
}
