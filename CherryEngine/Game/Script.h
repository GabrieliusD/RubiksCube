#pragma once
#include "CherryAPI.h"
#include "Scene/Scene.h"
#include <DirectXMath.h>
#include <string>
#include <type_traits>

class Engine;

// A script: a component with behaviour (like Unity's MonoBehaviour or Unreal's
// ActorComponent). Subclass it, register it with GameTypes::Script<T>() and add it to
// entities in the editor. Its reflected fields are edited in the inspector and saved
// with the scene; other members are runtime state.
//
//   class Door : public Script
//   {
//   public:
//       float openAngle = 90.0f;
//       void OnUpdate(float dt) override { GetTransform().SetRotationEuler(0, ..., 0); }
//   };
//
//   types.Script<Door>("Door").Field("openAngle", &Door::openAngle, 0.0f, 180.0f);
//
// While playing, the engine calls OnStart once (on the first frame the script exists,
// so also for spawned entities), then OnUpdate every frame after the game module's
// OnUpdate and its systems, and OnDestroy when the entity is destroyed with Destroy()
// or play stops. Scripts don't run while editing.
//
// Scripts are stored by value in component arrays and copied into the play world:
// keep entity ids, not pointers, and don't keep pointers to the script itself.
class CHERRY_API Script
{
public:
	virtual ~Script() = default;

	virtual void OnStart() {}
	virtual void OnUpdate(float /*deltaTime*/) {}
	virtual void OnDestroy() {}
	// Extra inspector UI below the script's fields (edit and play).
	virtual void OnInspectorGui() {}

	// ---- Helpers (valid from OnStart on; the inspector hook gets them too) -------------
	Entity GetEntity() const { return mEntity; }
	Engine& GetEngine() const { return *mEngine; }
	Scene& GetScene() const;
	Transform& GetTransform() const;
	DirectX::XMFLOAT3 WorldPosition() const;

	template <typename T> bool Has() const { return GetScene().IsRegistered<T>() && GetScene().Has<T>(mEntity); }
	template <typename T> T& Get() const { return GetScene().Get<T>(mEntity); }
	template <typename T> T* TryGet() const { return Has<T>() ? &GetScene().Get<T>(mEntity) : nullptr; }

	// Spawns a prefab (Content path) at a world position. Returns its root entity.
	Entity Spawn(const std::string& prefab, const DirectX::XMFLOAT3& position) const;
	// Destroys an entity (default: this one) and its children at the end of the frame,
	// after calling OnDestroy on their scripts.
	void Destroy(Entity entity = INVALID_ENTITY) const;
	// The first entity with this EntityInfo name, or INVALID_ENTITY.
	Entity Find(const std::string& name) const;

	bool HasStarted() const { return mStarted; }

private:
	friend struct ScriptBinding;
	Engine* mEngine = nullptr;
	Entity mEntity = INVALID_ENTITY;
	bool mStarted = false;
};

// Engine-internal access to a script's binding.
struct ScriptBinding
{
	static void Bind(Script& script, Engine* engine, Entity entity) { script.mEngine = engine; script.mEntity = entity; }
	static void SetStarted(Script& script, bool started) { script.mStarted = started; }
	static bool IsBound(const Script& script) { return script.mEngine != nullptr; }
};

// Registers a Script subclass as a component that the engine runs.
template <typename S>
ComponentBuilder<S> RegisterScript(TypeRegistry& registry, const std::string& name, uint32_t flags = Component_None)
{
	static_assert(std::is_base_of_v<Script, S>, "Scripts must derive from Script");
	ComponentBuilder<S> builder = registry.Component<S>(name, flags | Component_Script);
	builder.Info().asScript = [](void* component) -> Script* { return static_cast<S*>(component); };
	return builder;
}
