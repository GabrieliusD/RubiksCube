#pragma once
#include <Game/Script.h>

// Rotate: describe what it does.
class Rotate : public Script
{
public:
	// Fields registered in Rotate.cpp are edited in the inspector and saved with the scene.
	float speed = 1.0f;

	void OnStart() override;
	void OnUpdate(float deltaTime) override;

private:
	// Runtime state: not saved, reset on every Play.
	float mTime = 0.0f;
	float mAngle = 0.0f;
};
