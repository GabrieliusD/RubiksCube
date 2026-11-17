#include "Scene/Scene.h"
#include <algorithm>

Scene::Scene()
{
	mCoordinator.Init();
	mCoordinator.RegisterComponent<Transform>();
	mCoordinator.RegisterComponent<MeshRenderer>();
	mCoordinator.RegisterComponent<Camera>();
	mCoordinator.RegisterComponent<VoxelVolumeRenderer>();

	mMeshes = mCoordinator.RegisterSystem<MeshRendererSet>();
	mVoxels = mCoordinator.RegisterSystem<VoxelVolumeSet>();
	mCameras = mCoordinator.RegisterSystem<CameraSet>();

	Signature meshSignature;
	meshSignature.set(mCoordinator.GetComponentType<Transform>());
	meshSignature.set(mCoordinator.GetComponentType<MeshRenderer>());
	mCoordinator.SetSystemSignature<MeshRendererSet>(meshSignature);

	Signature voxelSignature;
	voxelSignature.set(mCoordinator.GetComponentType<Transform>());
	voxelSignature.set(mCoordinator.GetComponentType<VoxelVolumeRenderer>());
	mCoordinator.SetSystemSignature<VoxelVolumeSet>(voxelSignature);

	Signature cameraSignature;
	cameraSignature.set(mCoordinator.GetComponentType<Transform>());
	cameraSignature.set(mCoordinator.GetComponentType<Camera>());
	mCoordinator.SetSystemSignature<CameraSet>(cameraSignature);
}

Scene::~Scene() = default;

Entity Scene::CreateEntity()
{
	Entity entity = mCoordinator.CreateEntity();
	mAllEntities.push_back(entity);
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
