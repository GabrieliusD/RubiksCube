#pragma once
#include "Entity.h"

class CameraSystem : public System
{
public:
	CameraSystem() {}
	void Init();
	void Update(float dt);

	Entity GetCamera() { return mCamera; }
private:
	Entity mCamera;
};