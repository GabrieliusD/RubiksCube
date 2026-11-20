#include "Rotate.h"
#include <Core/Engine.h>
#include <Game/GameModule.h>   // CHERRY_SCRIPT

// Registers the script when the game module loads (no line in RegisterTypes needed).
CHERRY_SCRIPT(Rotate, "Rotate")
{
	script.Field("speed", &Rotate::speed, 0.0f, 10.0f);
}

void Rotate::OnStart()
{
	// Runs once, on the first frame of play (also for entities spawned later).
}

void Rotate::OnUpdate(float deltaTime)
{
	mTime += deltaTime;
	mAngle += speed * deltaTime;
	GetTransform().SetRotationEuler(mAngle, 0, 0);
	// Every frame while playing. Helpers: GetTransform(), Get<T>(), Has<T>(),
	// Spawn("Prefabs/x.prefab", position), Destroy(), Find("name"), GetScene(), GetEngine().
}
