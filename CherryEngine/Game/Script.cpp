#include "Game/Script.h"
#include "Core/Engine.h"

Scene& Script::GetScene() const { return mEngine->GetScene(); }
Transform& Script::GetTransform() const { return GetScene().Get<Transform>(mEntity); }
DirectX::XMFLOAT3 Script::WorldPosition() const { return GetScene().WorldPosition(mEntity); }

Entity Script::Spawn(const std::string& prefab, const DirectX::XMFLOAT3& position) const
{
	return mEngine->Spawn(prefab, position);
}

void Script::Destroy(Entity entity) const
{
	mEngine->DestroyLater(entity == INVALID_ENTITY ? mEntity : entity);
}

Entity Script::Find(const std::string& name) const
{
	Scene& scene = GetScene();
	for (Entity entity : scene.Entities())
	{
		if (scene.Has<EntityInfo>(entity) && scene.Get<EntityInfo>(entity).name == name) return entity;
	}
	return INVALID_ENTITY;
}
