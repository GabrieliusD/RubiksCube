#include "Scene/Reflection.h"
#include "Scene/Scene.h"
#include "Game/BuiltinScripts.h"
#include <algorithm>

const FieldInfo* ComponentInfo::FindField(const std::string& fieldName) const
{
	for (const FieldInfo& field : fields)
	{
		if (field.name == fieldName) return &field;
	}
	return nullptr;
}

namespace
{
	void RegisterBuiltins(TypeRegistry& registry)
	{
		registry.SetCurrentOwner("engine");

		registry.Component<EntityInfo>("EntityInfo", Component_Hidden | Component_NotAddable | Component_NotRemovable)
			.Field("guid", &EntityInfo::guid, Field_ReadOnly | Field_Hidden)
			.Field("name", &EntityInfo::name)
			.Field("prefab", &EntityInfo::prefab, Field_Hidden);

		registry.Component<Transform>("Transform", Component_NotRemovable)
			.Field("position", &Transform::position).Speed(0.05f)
			.Field("rotation", &Transform::rotation, Field_Rotation)
			.Field("scale", &Transform::scale).Speed(0.02f);

		registry.Component<Parent>("Parent", Component_Hidden | Component_NotAddable)
			.EntityRef("parent", &Parent::parent);

		registry.Component<MeshRenderer>("MeshRenderer")
			.Asset("mesh", &MeshRenderer::meshAsset, "mesh")
			.Asset("material", &MeshRenderer::materialAsset, "material")
			.Field("visible", &MeshRenderer::visible)
			.Enum("layer", &MeshRenderer::layer, { "World", "ViewModel" });

		registry.Component<Camera>("Camera")
			.Field("fovY", &Camera::fovY, 0.1f, 3.0f, Field_Angle)
			.Field("nearZ", &Camera::nearZ, 0.001f, 100.0f).Speed(0.01f)
			.Field("farZ", &Camera::farZ, 1.0f, 100000.0f).Speed(1.0f);

		registry.Component<PointLightComponent>("PointLight")
			.Field("color", &PointLightComponent::color, Field_Color)
			.Field("intensity", &PointLightComponent::intensity, 0.0f, 100.0f).Speed(0.05f)
			.Field("range", &PointLightComponent::range, 0.1f, 200.0f).Speed(0.05f);

		registry.Component<Environment>("Environment")
			.Field("skyColor", &Environment::skyColor, Field_Color)
			.Field("ambient", &Environment::ambient, Field_Color)
			.Field("sunDirection", &Environment::sunDirection, Field_Direction).Speed(0.01f)
			.Field("sunColor", &Environment::sunColor, Field_Color)
			.Field("fillLightScale", &Environment::fillLightScale, 0.0f, 4.0f)
			.Field("fogEnabled", &Environment::fogEnabled)
			.Field("fogColor", &Environment::fogColor, Field_Color)
			.Field("fogStart", &Environment::fogStart, 0.0f, 10000.0f).Speed(0.5f)
			.Field("fogEnd", &Environment::fogEnd, 0.0f, 10000.0f).Speed(0.5f)
			.Field("exposure", &Environment::exposure, 0.05f, 8.0f).Speed(0.01f);

		// Runtime-made renderers and bodies: shown in the inspector while playing, never saved.
		registry.Component<VoxelVolumeRenderer>("VoxelVolumeRenderer", Component_Transient | Component_NotAddable)
			.Field("voxelSize", &VoxelVolumeRenderer::voxelSize, Field_NotSaved).Speed(0.005f)
			.Field("visible", &VoxelVolumeRenderer::visible, Field_NotSaved)
			.Field("colorJitter", &VoxelVolumeRenderer::colorJitter, 0.0f, 1.0f, Field_NotSaved)
			.Field("aoStrength", &VoxelVolumeRenderer::aoStrength, 0.0f, 1.0f, Field_NotSaved)
			.Field("jitterOffset", &VoxelVolumeRenderer::jitterOffset, Field_NotSaved);

		registry.Component<RigidBody>("RigidBody", Component_Transient | Component_NotAddable)
			.Field("body", &RigidBody::body, Field_ReadOnly | Field_NotSaved);

		// Saved by the scene serializer itself (each entry is its own component in the file).
		registry.Component<UnknownComponents>("UnknownComponents", Component_Hidden | Component_NotAddable);

		RegisterBuiltinScripts(registry);
	}
}

TypeRegistry& TypeRegistry::Get()
{
	static TypeRegistry* registry = []()
	{
		auto* instance = new TypeRegistry();   // never destroyed: game modules may outlive static teardown order
		RegisterBuiltins(*instance);
		return instance;
	}();
	return *registry;
}

ComponentInfo& TypeRegistry::Add(std::unique_ptr<ComponentInfo> info)
{
	++mVersion;
	for (auto& existing : mComponents)
	{
		if (existing->name == info->name)
		{
			existing = std::move(info);
			return *existing;
		}
	}
	mComponents.push_back(std::move(info));
	return *mComponents.back();
}

const ComponentInfo* TypeRegistry::Find(const std::string& name) const
{
	for (const auto& info : mComponents)
	{
		if (info->name == name) return info.get();
	}
	return nullptr;
}

const ComponentInfo* TypeRegistry::FindByTypeKey(const std::string& typeKey) const
{
	for (const auto& info : mComponents)
	{
		if (info->typeKey == typeKey) return info.get();
	}
	return nullptr;
}

void TypeRegistry::Unregister(const std::string& owner)
{
	const size_t before = mComponents.size();
	mComponents.erase(std::remove_if(mComponents.begin(), mComponents.end(),
		[&](const std::unique_ptr<ComponentInfo>& info) { return info->owner == owner; }), mComponents.end());
	if (mComponents.size() != before) ++mVersion;
}
