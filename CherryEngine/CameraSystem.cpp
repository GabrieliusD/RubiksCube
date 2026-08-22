#include "CameraSystem.h"
#include <Camera.h>
#include <MathHelper.h>
#include <D3DApp.h>

void CameraSystem::Init()
{
	auto& coordinator = D3DApp::GetApp()->GetScene().GetCoordinator();
	mCamera = coordinator.CreateEntity();
	Transform transform{ XMFLOAT3(0,-10,10), XMFLOAT3(-0.3,0,0), XMFLOAT3(0,0,0) };
	coordinator.AddComponent(mCamera, transform);

	Camera camera;
	camera.CreateProjection(0.6f * MathHelper::Pi, 800.0f / 600.0f, 0.05f, 1000.0f);
	camera.CreateViewFromTransform(transform);
	coordinator.AddComponent(mCamera, camera);
}

void CameraSystem::Update(float dt)
{
	
}
