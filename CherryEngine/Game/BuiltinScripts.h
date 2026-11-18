#pragma once
#include "Game/Script.h"
#include <DirectXMath.h>
#include <string>
#include <vector>

// Small general-purpose scripts that come with the engine (and examples of writing one).

// Turns the entity around an axis.
class CHERRY_API Spinner : public Script
{
public:
	DirectX::XMFLOAT3 axis{ 0, 1, 0 };   // in the entity's own space
	float degreesPerSecond = 90.0f;

	void OnUpdate(float deltaTime) override;
};

// Spawns a prefab at the entity's position every `interval` seconds, keeping at most
// `maxAlive` of them; each lives `lifetime` seconds (0 = until play stops).
class CHERRY_API Spawner : public Script
{
public:
	std::string prefab;
	float interval = 1.0f;
	int32_t maxAlive = 10;
	float lifetime = 5.0f;
	float scatter = 0.5f;               // m of random offset around the entity

	void OnUpdate(float deltaTime) override;
	void OnInspectorGui() override;

private:
	float mTimer = 0.0f;
	uint32_t mCount = 0;
	std::vector<Entity> mAlive;
};

// Destroys its entity (and children) after `seconds`.
class CHERRY_API Lifetime : public Script
{
public:
	float seconds = 5.0f;

	void OnUpdate(float deltaTime) override;

private:
	float mAge = 0.0f;
};

// Registers the scripts above (called with the other built-in components).
CHERRY_API void RegisterBuiltinScripts(TypeRegistry& registry);
