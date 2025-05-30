#include "CameraSystem.h"
#include <Camera.h>
#include <MathHelper.h>

extern Coordinator gCoordinator;

void CameraSystem::Init()
{
	mCamera = gCoordinator.CreateEntity();
	Transform transform{ XMFLOAT3(0,-10,10), XMFLOAT3(-0.3,0,0), XMFLOAT3(0,0,0) };
	gCoordinator.AddComponent(mCamera, transform);

	Camera camera;
	camera.CreateProjection(0.6f * MathHelper::Pi, 800.0f / 600.0f, 0.05f, 1000.0f);
	camera.CreateViewFromTransform(transform);
	gCoordinator.AddComponent(mCamera, camera);
}

void CameraSystem::Update(float dt)
{
	
}
