#pragma once
#include <Game/Script.h>

// Test: describe what it does.
class Test : public Script
{
public:
	// Fields registered in Test.cpp are edited in the inspector and saved with the scene.
	float speed = 1.0f;

	void OnStart() override;
	void OnUpdate(float deltaTime) override;

private:
	// Runtime state: not saved, reset on every Play.
	float mTime = 0.0f;
};
