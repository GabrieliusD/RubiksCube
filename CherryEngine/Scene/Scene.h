#pragma once
#include "CherryAPI.h"
#include "Scene/ECS.h"
#include "Scene/Components.h"
#include "Scene/Reflection.h"
#include <memory>

// Systems that only collect the entities the renderer and camera care about.
class MeshRendererSet : public System {};
class VoxelVolumeSet : public System {};
class CameraSet : public System {};
class RigidBodySet : public System {};
class PointLightSet : public System {};

// Owns the ECS world and the built-in component registrations.
class CHERRY_API Scene
{
public:
	Scene();
	~Scene();

	Entity CreateEntity();
	// Creates an entity with an EntityInfo (new guid, `name`) and a default Transform.
	Entity CreateNamedEntity(const std::string& name);
	void DestroyEntity(Entity entity);
	bool IsAlive(Entity entity) const { return mCoordinator.IsAlive(entity); }
	// Destroys every entity.
	void Clear();
	// Every live entity, in creation order (the order the editor lists and saves them).
	const std::vector<Entity>& Entities() const { return mAllEntities; }
	// Moves `entity` to position `index` of Entities() (sibling order in the editor).
	void MoveEntity(Entity entity, size_t index);

	template <typename T> void Add(Entity entity, const T& component) { mCoordinator.AddComponent<T>(entity, component); }
	template <typename T> void Remove(Entity entity) { mCoordinator.RemoveComponent<T>(entity); }
	template <typename T> T& Get(Entity entity) { return mCoordinator.GetComponent<T>(entity); }
	template <typename T> bool Has(Entity entity) { return mCoordinator.HasComponent<T>(entity); }
	template <typename T> T* TryGet(Entity entity) { return Has<T>(entity) ? &Get<T>(entity) : nullptr; }
	// Calls f(entity, component) for every entity that has a T (safe to destroy entities inside).
	template <typename T, typename F> void Each(F&& f)
	{
		if (!IsRegistered<T>()) return;
		const std::vector<Entity> entities = mAllEntities;
		for (Entity entity : entities)
		{
			if (IsAlive(entity) && Has<T>(entity)) f(entity, Get<T>(entity));
		}
	}

	// Registers a game-defined component type (call once, e.g. in OnStartup).
	template <typename T> void RegisterComponent()
	{
		if (!mCoordinator.IsComponentRegistered<T>()) mCoordinator.RegisterComponent<T>();
	}
	template <typename T> bool IsRegistered() const { return mCoordinator.IsComponentRegistered<T>(); }
	// Registers every reflected type this scene doesn't know yet (after a module loads).
	void RegisterReflectedTypes();

	Coordinator& GetCoordinator() { return mCoordinator; }

	const std::set<Entity>& MeshEntities() const { return mMeshes->mEntities; }
	const std::set<Entity>& VoxelEntities() const { return mVoxels->mEntities; }
	const std::set<Entity>& CameraEntities() const { return mCameras->mEntities; }
	const std::set<Entity>& RigidBodyEntities() const { return mRigidBodies->mEntities; }
	const std::set<Entity>& PointLightEntities() const { return mPointLights->mEntities; }

	// ---- Hierarchy ---------------------------------------------------------------
	// The entity's parent, or INVALID_ENTITY (also when the parent was destroyed).
	Entity GetParent(Entity entity);
	// World matrix: the Transform, times the parents' world matrices.
	DirectX::XMMATRIX WorldMatrix(Entity entity);
	DirectX::XMFLOAT3 WorldPosition(Entity entity);
	// Reparents (INVALID_ENTITY = make it a root). `keepWorld` keeps it where it is in the
	// world by rewriting its local Transform. Refuses to create a cycle.
	bool SetParent(Entity entity, Entity parent, bool keepWorld = true);
	// True if `ancestor` is `entity` or one of its parents.
	bool IsAncestor(Entity ancestor, Entity entity);
	// Direct children, in Entities() order.
	std::vector<Entity> Children(Entity entity);
	// Destroys the entity and all its descendants.
	void DestroyTree(Entity entity);

	// ---- Identity ----------------------------------------------------------------
	static uint64_t NewGuid();
	// The entity's guid (0 if it has no EntityInfo).
	uint64_t GetGuid(Entity entity);
	// Gives the entity an EntityInfo with a new guid if it has none; returns the guid.
	uint64_t EnsureGuid(Entity entity);
	Entity FindByGuid(uint64_t guid);
	// The EntityInfo name, or "Entity <id>".
	std::string DisplayName(Entity entity);

private:
	Coordinator mCoordinator;
	std::shared_ptr<MeshRendererSet> mMeshes;
	std::shared_ptr<VoxelVolumeSet> mVoxels;
	std::shared_ptr<CameraSet> mCameras;
	std::shared_ptr<RigidBodySet> mRigidBodies;
	std::shared_ptr<PointLightSet> mPointLights;
	std::vector<Entity> mAllEntities;
	ComponentType mParentType = 0;
	ComponentType mInfoType = 0;
};

// ---- TypeRegistry's scene operations (declared in Reflection.h) ----------------------
template <typename C> void TypeRegistry::RegisterInScene(Scene& scene) { scene.RegisterComponent<C>(); }
template <typename C> bool TypeRegistry::HasIn(Scene& scene, Entity entity) { return scene.IsRegistered<C>() && scene.Has<C>(entity); }
template <typename C> C* TypeRegistry::GetIn(Scene& scene, Entity entity) { return HasIn<C>(scene, entity) ? &scene.Get<C>(entity) : nullptr; }
template <typename C> C* TypeRegistry::AddIn(Scene& scene, Entity entity, const C& value)
{
	scene.RegisterComponent<C>();
	scene.Add(entity, value);
	return &scene.Get<C>(entity);
}
template <typename C> void TypeRegistry::RemoveIn(Scene& scene, Entity entity)
{
	if (HasIn<C>(scene, entity)) scene.Remove<C>(entity);
}
