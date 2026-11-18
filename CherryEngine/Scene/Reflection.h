#pragma once
#include "CherryAPI.h"
#include "Scene/ECS.h"
#include <DirectXMath.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

class Scene;

// Component reflection: one description per component type drives scene files, the
// play-world copy and the editor's inspector.
//
//   TypeRegistry::Get().Component<Lantern>("Lantern")
//       .Field("intensity", &Lantern::intensity, 0.0f, 20.0f)
//       .Field("color", &Lantern::color, Field_Color);
//
// Fields are found by name when loading, so adding, removing or reordering fields keeps
// old scene files loadable (missing fields keep their default value).

enum class FieldKind : uint8_t
{
	Bool, Int, UInt, UInt64, Float, Float2, Float3, Float4, Int3, String, Enum, EntityRef,
};

enum FieldFlags : uint32_t
{
	Field_None = 0,
	Field_NotSaved = 1u << 0,     // shown and copied, but not written to scene files
	Field_ReadOnly = 1u << 1,
	Field_Color = 1u << 2,        // Float3 / Float4 edited as a colour
	Field_Rotation = 1u << 3,     // Float4 quaternion, edited as Euler angles in degrees
	Field_Angle = 1u << 4,        // Float in radians, edited in degrees
	Field_Hidden = 1u << 5,       // saved, not shown
	Field_Direction = 1u << 6,    // Float3 that should stay normalised
};

struct FieldInfo
{
	std::string name;
	FieldKind kind = FieldKind::Float;
	uint32_t flags = Field_None;
	size_t size = 0;                        // bytes of the stored value (enums)
	float min = 0.0f, max = 0.0f;           // min == max: no limits
	float speed = 0.05f;                    // drag speed in the inspector
	std::vector<std::string> enumNames;     // index = enum value
	std::string assetType;                  // String naming an asset: "mesh", "material"
	std::function<void*(void*)> access;     // component -> field
};

enum ComponentFlags : uint32_t
{
	Component_None = 0,
	Component_Transient = 1u << 0,       // runtime only: never saved, not copied into the play world
	Component_Hidden = 1u << 1,          // not shown in the inspector
	Component_NotAddable = 1u << 2,      // not offered by "Add Component"
	Component_NotRemovable = 1u << 3,
};

struct CHERRY_API ComponentInfo
{
	std::string name;        // in scene files and the editor: "Transform"
	std::string typeKey;     // EcsTypeName<T>()
	uint32_t flags = Component_None;
	std::string owner;       // "engine", or the game module that registered it
	std::vector<FieldInfo> fields;

	std::function<void(Scene&)> registerIn;                     // make sure a scene can hold it
	std::function<bool(Scene&, Entity)> has;
	std::function<void*(Scene&, Entity)> get;
	std::function<void*(Scene&, Entity)> addDefault;            // returns the new component
	std::function<void(Scene&, Entity, const void*)> addCopy;   // copies a component of this type
	std::function<void(Scene&, Entity)> remove;

	const FieldInfo* FindField(const std::string& fieldName) const;
	bool Is(uint32_t flag) const { return (flags & flag) != 0; }
};

namespace reflection_detail
{
	template <typename T> struct Always { static constexpr bool value = false; };

	template <typename T>
	constexpr FieldKind KindOf()
	{
		if constexpr (std::is_same_v<T, bool>) return FieldKind::Bool;
		else if constexpr (std::is_enum_v<T>) return FieldKind::Enum;
		else if constexpr (std::is_same_v<T, int32_t>) return FieldKind::Int;
		else if constexpr (std::is_same_v<T, uint32_t>) return FieldKind::UInt;
		else if constexpr (std::is_same_v<T, uint64_t>) return FieldKind::UInt64;
		else if constexpr (std::is_same_v<T, float>) return FieldKind::Float;
		else if constexpr (std::is_same_v<T, DirectX::XMFLOAT2>) return FieldKind::Float2;
		else if constexpr (std::is_same_v<T, DirectX::XMFLOAT3>) return FieldKind::Float3;
		else if constexpr (std::is_same_v<T, DirectX::XMFLOAT4>) return FieldKind::Float4;
		else if constexpr (std::is_same_v<T, DirectX::XMINT3>) return FieldKind::Int3;
		else if constexpr (std::is_same_v<T, std::string>) return FieldKind::String;
		else static_assert(Always<T>::value, "Unsupported field type for reflection");
	}
}

template <typename C>
class ComponentBuilder
{
public:
	explicit ComponentBuilder(ComponentInfo& info) : mInfo(info) {}

	template <typename T>
	ComponentBuilder& Field(const char* name, T C::* member, uint32_t flags = Field_None)
	{
		FieldInfo& field = AddField(name, member, flags);
		(void)field;
		return *this;
	}

	template <typename T>
	ComponentBuilder& Field(const char* name, T C::* member, float min, float max, uint32_t flags = Field_None)
	{
		FieldInfo& field = AddField(name, member, flags);
		field.min = min;
		field.max = max;
		if (max > min) field.speed = (max - min) / 200.0f;
		return *this;
	}

	template <typename E>
	ComponentBuilder& Enum(const char* name, E C::* member, std::vector<std::string> names, uint32_t flags = Field_None)
	{
		static_assert(std::is_enum_v<E>, "Enum() needs an enum member");
		FieldInfo& field = AddField(name, member, flags);
		field.enumNames = std::move(names);
		return *this;
	}

	// An Entity member that refers to another entity (saved as that entity's guid).
	ComponentBuilder& EntityRef(const char* name, Entity C::* member, uint32_t flags = Field_None)
	{
		FieldInfo& field = AddField(name, member, flags);
		field.kind = FieldKind::EntityRef;
		return *this;
	}

	// A std::string member holding an asset path of the given type ("mesh", "material").
	ComponentBuilder& Asset(const char* name, std::string C::* member, const char* assetType, uint32_t flags = Field_None)
	{
		FieldInfo& field = AddField(name, member, flags);
		field.assetType = assetType;
		return *this;
	}

	// Inspector drag speed for the last field.
	ComponentBuilder& Speed(float speed)
	{
		if (!mInfo.fields.empty()) mInfo.fields.back().speed = speed;
		return *this;
	}

private:
	template <typename T>
	FieldInfo& AddField(const char* name, T C::* member, uint32_t flags)
	{
		FieldInfo field;
		field.name = name;
		field.kind = reflection_detail::KindOf<T>();
		field.flags = flags;
		field.size = sizeof(T);
		field.access = [member](void* component) -> void* { return &(static_cast<C*>(component)->*member); };
		mInfo.fields.push_back(std::move(field));
		return mInfo.fields.back();
	}

	ComponentInfo& mInfo;
};

class CHERRY_API TypeRegistry
{
public:
	// The engine's built-in components are registered on first use.
	static TypeRegistry& Get();
	TypeRegistry(const TypeRegistry&) = delete;
	TypeRegistry& operator=(const TypeRegistry&) = delete;

	// Registers (or, for an existing name, replaces) a component description. Its scene
	// operations are compiled into the caller, so a game module must call Unregister()
	// with its owner name before it is unloaded.
	template <typename C>
	ComponentBuilder<C> Component(const std::string& name, uint32_t flags = Component_None)
	{
		auto info = std::make_unique<ComponentInfo>();
		info->name = name;
		info->typeKey = EcsTypeName<C>();
		info->flags = flags;
		info->owner = mCurrentOwner;
		info->registerIn = [](Scene& scene) { RegisterInScene<C>(scene); };
		info->has = [](Scene& scene, Entity entity) { return HasIn<C>(scene, entity); };
		info->get = [](Scene& scene, Entity entity) -> void* { return GetIn<C>(scene, entity); };
		info->addDefault = [](Scene& scene, Entity entity) -> void* { return AddIn<C>(scene, entity, C{}); };
		info->addCopy = [](Scene& scene, Entity entity, const void* source) { AddIn<C>(scene, entity, *static_cast<const C*>(source)); };
		info->remove = [](Scene& scene, Entity entity) { RemoveIn<C>(scene, entity); };
		return ComponentBuilder<C>(Add(std::move(info)));
	}

	const ComponentInfo* Find(const std::string& name) const;
	const ComponentInfo* FindByTypeKey(const std::string& typeKey) const;
	template <typename C> const ComponentInfo* Find() const { return FindByTypeKey(EcsTypeName<C>()); }
	// In registration order (the order components are listed in the inspector and files).
	const std::vector<std::unique_ptr<ComponentInfo>>& Components() const { return mComponents; }

	// Registrations from now on belong to `owner` ("engine" by default).
	void SetCurrentOwner(const std::string& owner) { mCurrentOwner = owner; }
	void Unregister(const std::string& owner);
	// Increments on every change; lets scenes notice new component types.
	uint32_t Version() const { return mVersion; }

private:
	TypeRegistry() = default;
	ComponentInfo& Add(std::unique_ptr<ComponentInfo> info);

	// Defined in Scene.h (which includes this header) to avoid a dependency cycle.
	template <typename C> static void RegisterInScene(Scene& scene);
	template <typename C> static bool HasIn(Scene& scene, Entity entity);
	template <typename C> static C* GetIn(Scene& scene, Entity entity);
	template <typename C> static C* AddIn(Scene& scene, Entity entity, const C& value);
	template <typename C> static void RemoveIn(Scene& scene, Entity entity);

	std::vector<std::unique_ptr<ComponentInfo>> mComponents;
	std::string mCurrentOwner = "engine";
	uint32_t mVersion = 0;
};
