#pragma once
#include "CherryAPI.h"
#include "Scene/ECS.h"
#include "Scene/Components.h"
#include <memory>

// Systems that only collect the entities the renderer and camera care about.
class MeshRendererSet : public System {};
class VoxelVolumeSet : public System {};
class CameraSet : public System {};
class RigidBodySet : public System {};

// Owns the ECS world and the built-in component registrations.
class CHERRY_API Scene
{
public:
	Scene();
	~Scene();

	Entity CreateEntity();
	void DestroyEntity(Entity entity);
	bool IsAlive(Entity entity) const { return mCoordinator.IsAlive(entity); }
	// Destroys every entity.
	void Clear();

	template <typename T> void Add(Entity entity, const T& component) { mCoordinator.AddComponent<T>(entity, component); }
	template <typename T> void Remove(Entity entity) { mCoordinator.RemoveComponent<T>(entity); }
	template <typename T> T& Get(Entity entity) { return mCoordinator.GetComponent<T>(entity); }
	template <typename T> bool Has(Entity entity) { return mCoordinator.HasComponent<T>(entity); }

	// Registers a game-defined component type (call once, e.g. in OnStartup).
	template <typename T> void RegisterComponent()
	{
		if (!mCoordinator.IsComponentRegistered<T>()) mCoordinator.RegisterComponent<T>();
	}

	Coordinator& GetCoordinator() { return mCoordinator; }

	const std::set<Entity>& MeshEntities() const { return mMeshes->mEntities; }
	const std::set<Entity>& VoxelEntities() const { return mVoxels->mEntities; }
	const std::set<Entity>& CameraEntities() const { return mCameras->mEntities; }
	const std::set<Entity>& RigidBodyEntities() const { return mRigidBodies->mEntities; }

private:
	Coordinator mCoordinator;
	std::shared_ptr<MeshRendererSet> mMeshes;
	std::shared_ptr<VoxelVolumeSet> mVoxels;
	std::shared_ptr<CameraSet> mCameras;
	std::shared_ptr<RigidBodySet> mRigidBodies;
	std::vector<Entity> mAllEntities;
};
