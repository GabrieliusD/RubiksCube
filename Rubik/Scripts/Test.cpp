#include "Test.h"
#include <Core/Engine.h>
#include <Game/GameModule.h>   // CHERRY_SCRIPT

// Registers the script when the game module loads (no line in RegisterTypes needed).
CHERRY_SCRIPT(Test, "Test")
{
	script.Field("speed", &Test::speed, 0.0f, 10.0f);
}

void Test::OnStart()
{
	// Runs once, on the first frame of play (also for entities spawned later).
}

void Test::OnUpdate(float deltaTime)
{
	mTime += deltaTime;
	// Every frame while playing. Helpers: GetTransform(), Get<T>(), Has<T>(),
	// Spawn("Prefabs/x.prefab", position), Destroy(), Find("name"), GetScene(), GetEngine().
}
