#include "Game/BuiltinScripts.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>

using namespace DirectX;

void Spinner::OnUpdate(float deltaTime)
{
	Transform& transform = GetTransform();
	XMVECTOR axisVector = XMLoadFloat3(&axis);
	if (XMVectorGetX(XMVector3LengthSq(axisVector)) < 1e-8f) return;
	const XMVECTOR turn = XMQuaternionRotationAxis(XMVector3Normalize(axisVector), XMConvertToRadians(degreesPerSecond) * deltaTime);
	// Local axis: the turn comes before the current rotation.
	XMStoreFloat4(&transform.rotation, XMQuaternionNormalize(XMQuaternionMultiply(turn, XMLoadFloat4(&transform.rotation))));
}

void Spawner::OnUpdate(float deltaTime)
{
	Scene& scene = GetScene();
	mAlive.erase(std::remove_if(mAlive.begin(), mAlive.end(), [&](Entity entity) { return !scene.IsAlive(entity); }), mAlive.end());
	if (prefab.empty() || interval <= 0.0f) return;

	mTimer += deltaTime;
	while (mTimer >= interval)
	{
		mTimer -= interval;
		if (static_cast<int32_t>(mAlive.size()) >= maxAlive) continue;

		// A deterministic scatter (golden angle spiral), so runs look the same.
		++mCount;
		const float angle = mCount * 2.39996f;
		const float radius = scatter * std::sqrt(static_cast<float>(mCount % 16) / 16.0f);
		XMFLOAT3 position = WorldPosition();
		position.x += std::cos(angle) * radius;
		position.z += std::sin(angle) * radius;

		const Entity spawned = Spawn(prefab, position);
		if (spawned == INVALID_ENTITY) return;
		if (lifetime > 0.0f)
		{
			Lifetime life;
			life.seconds = lifetime;
			scene.Add(spawned, life);
		}
		mAlive.push_back(spawned);
	}
}

void Spawner::OnInspectorGui()
{
	if (HasStarted()) ImGui::TextDisabled("%zu alive, %u spawned", mAlive.size(), mCount);
}

void Lifetime::OnUpdate(float deltaTime)
{
	mAge += deltaTime;
	if (mAge >= seconds) Destroy();
}

void RegisterBuiltinScripts(TypeRegistry& registry)
{
	RegisterScript<Spinner>(registry, "Spinner")
		.Field("axis", &Spinner::axis, Field_Direction).Speed(0.01f)
		.Field("degreesPerSecond", &Spinner::degreesPerSecond, -1440.0f, 1440.0f).Speed(1.0f);
	RegisterScript<Spawner>(registry, "Spawner")
		.Asset("prefab", &Spawner::prefab, "prefab")
		.Field("interval", &Spawner::interval, 0.02f, 60.0f).Speed(0.01f)
		.Field("maxAlive", &Spawner::maxAlive, 0.0f, 1000.0f)
		.Field("lifetime", &Spawner::lifetime, 0.0f, 600.0f).Speed(0.05f)
		.Field("scatter", &Spawner::scatter, 0.0f, 50.0f).Speed(0.01f);
	RegisterScript<Lifetime>(registry, "Lifetime")
		.Field("seconds", &Lifetime::seconds, 0.0f, 600.0f).Speed(0.05f);
}
