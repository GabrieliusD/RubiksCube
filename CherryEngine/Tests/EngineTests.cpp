// Tests for the scene, reflection and scene files (no window, no GPU). Run via CTest or directly.
#include <Scene/Scene.h>
#include <Scene/SceneSerializer.h>
#include <Game/Script.h>
#include <Game/BuiltinScripts.h>
#include <cmath>
#include <cstdio>
#include <string>

using namespace DirectX;

int RunShaderGraphTests();   // ShaderGraphTests.cpp
int RunVoxelModelTests();    // VoxelModelTests.cpp
int RunRendererTests();      // RendererTests.cpp

namespace
{
	int gFailures = 0;

	void Check(bool condition, const char* what)
	{
		std::printf("%s  %s\n", condition ? "[pass]" : "[FAIL]", what);
		if (!condition) ++gFailures;
	}

	bool Near(const XMFLOAT3& a, const XMFLOAT3& b, float epsilon = 1e-4f)
	{
		return std::fabs(a.x - b.x) < epsilon && std::fabs(a.y - b.y) < epsilon && std::fabs(a.z - b.z) < epsilon;
	}

	// A small scene that uses every kind of saved data.
	void BuildSample(Scene& scene, Entity& root, Entity& child)
	{
		const Entity environment = scene.CreateNamedEntity("Environment");
		Environment env;
		env.exposure = 1.7f;
		env.fogEnabled = true;
		scene.Add(environment, env);

		root = scene.CreateNamedEntity("Root");
		Transform& rootTransform = scene.Get<Transform>(root);
		rootTransform.position = { 1.0f, 2.0f, 3.0f };
		rootTransform.SetRotationEuler(0.0f, XM_PIDIV2, 0.0f);
		MeshRenderer mesh;
		mesh.meshAsset = "engine/Cube";
		mesh.materialAsset = "engine/Red";
		mesh.layer = RenderLayer::ViewModel;
		scene.Add(root, mesh);

		child = scene.CreateNamedEntity("Child");
		scene.Get<Transform>(child).position = { 0.0f, 0.0f, 2.0f };
		scene.SetParent(child, root, false);
		PointLightComponent light;
		light.range = 12.5f;
		scene.Add(child, light);
	}
}

int main()
{
	// ---- Hierarchy -------------------------------------------------------------------
	{
		Scene scene;
		Entity root, child;
		BuildSample(scene, root, child);
		// Root is turned 90 degrees about Y, so the child's local +Z lands on world +X.
		Check(Near(scene.WorldPosition(child), { 3.0f, 2.0f, 3.0f }), "child world position goes through the parent's rotation");
		Check(scene.GetParent(child) == root, "GetParent returns the parent");
		Check(!scene.SetParent(root, child), "SetParent refuses a cycle");

		const XMFLOAT3 before = scene.WorldPosition(child);
		Check(scene.SetParent(child, INVALID_ENTITY, true) && Near(scene.WorldPosition(child), before), "unparenting with keepWorld keeps the world position");
		Check(scene.SetParent(child, root, true) && Near(scene.WorldPosition(child), before), "reparenting with keepWorld keeps the world position");

		scene.DestroyEntity(root);
		Check(scene.GetParent(child) == INVALID_ENTITY, "a child of a destroyed parent becomes a root");
	}

	// ---- Save / load round trip --------------------------------------------------------
	{
		Scene scene;
		Entity root, child;
		BuildSample(scene, root, child);
		const std::string first = cherry::SceneToJson(scene);

		Scene loaded;
		std::string error;
		Check(cherry::SceneFromJson(loaded, first, error), "scene JSON loads");
		Check(loaded.Entities().size() == 3, "all entities are loaded");
		const Entity loadedRoot = loaded.FindByGuid(scene.GetGuid(root));
		const Entity loadedChild = loaded.FindByGuid(scene.GetGuid(child));
		Check(loadedRoot != INVALID_ENTITY && loadedChild != INVALID_ENTITY, "guids survive the round trip");
		Check(loaded.GetParent(loadedChild) == loadedRoot, "parent references are restored");
		Check(Near(loaded.WorldPosition(loadedChild), { 3.0f, 2.0f, 3.0f }), "world transforms are restored");
		Check(loaded.Has<MeshRenderer>(loadedRoot) && loaded.Get<MeshRenderer>(loadedRoot).meshAsset == "engine/Cube"
			&& loaded.Get<MeshRenderer>(loadedRoot).layer == RenderLayer::ViewModel, "strings and enums are restored");
		Check(loaded.Has<PointLightComponent>(loadedChild) && loaded.Get<PointLightComponent>(loadedChild).range == 12.5f, "floats are restored");
		Check(loaded.DisplayName(loadedRoot) == "Root", "names are restored");

		const std::string second = cherry::SceneToJson(loaded);
		Check(first == second, "save -> load -> save gives the same file");
	}

	// ---- Unknown components and bad data ------------------------------------------------------
	{
		const std::string text = R"({
			"format": "cherry-scene", "version": 1,
			"entities": [
				{ "guid": "00000000000000aa", "components": {
					"EntityInfo": { "name": "Lantern" },
					"Transform": { "position": [1, 2], "scale": "oops" },
					"GameOnlyThing": { "power": 3, "tags": ["a", "b"] } } },
				{ "guid": "00000000000000aa", "components": { "EntityInfo": { "name": "Duplicate guid" } } }
			] })";
		Scene scene;
		std::string error;
		Check(cherry::SceneFromJson(scene, text, error), "a scene with unknown components loads");
		const Entity lantern = scene.FindByGuid(0xaa);
		Check(lantern != INVALID_ENTITY && scene.Has<UnknownComponents>(lantern), "unknown components are kept");
		Check(lantern != INVALID_ENTITY && scene.Get<Transform>(lantern).scale.x == 1.0f, "a field of the wrong type keeps its default");
		Check(scene.Entities().size() == 2 && scene.GetGuid(scene.Entities()[1]) != 0xaa, "a duplicate guid gets a new one");
		const std::string saved = cherry::SceneToJson(scene);
		Check(saved.find("GameOnlyThing") != std::string::npos && saved.find("\"tags\"") != std::string::npos, "unknown components are written back");

		Scene broken;
		Check(!cherry::SceneFromJson(broken, "{ not json", error), "invalid JSON is rejected");
		Check(!cherry::SceneFromJson(broken, "{ \"hello\": 1 }", error), "JSON without entities is rejected");
	}

	// ---- Play-world copy --------------------------------------------------------------------
	{
		Scene edit;
		Entity root, child;
		BuildSample(edit, root, child);
		MeshHandle handle;
		handle.index = 7;
		handle.generation = 3;
		edit.Get<MeshRenderer>(root).mesh = handle;
		edit.Add(root, RigidBody{ 42 });

		Scene play;
		cherry::CopyScene(edit, play);
		const Entity playRoot = play.FindByGuid(edit.GetGuid(root));
		const Entity playChild = play.FindByGuid(edit.GetGuid(child));
		Check(play.Entities().size() == edit.Entities().size(), "the copy has every entity");
		Check(playRoot != INVALID_ENTITY && play.Get<MeshRenderer>(playRoot).mesh == handle, "the copy keeps runtime handles");
		Check(play.GetParent(playChild) == playRoot, "the copy's parent points at the copied parent");
		Check(playRoot != INVALID_ENTITY && !play.Has<RigidBody>(playRoot), "transient components are not copied");

		play.Get<Transform>(playRoot).position.x = 99.0f;
		Check(edit.Get<Transform>(root).position.x == 1.0f, "editing the copy leaves the original alone");
	}

	// ---- Duplicate -----------------------------------------------------------------------------
	{
		Scene scene;
		Entity root, child;
		BuildSample(scene, root, child);
		const Entity copy = cherry::DuplicateEntity(scene, root);
		Check(scene.Entities().size() == 5, "duplicating copies the children too");
		Check(copy != INVALID_ENTITY && scene.GetGuid(copy) != scene.GetGuid(root), "the duplicate gets a new guid");
		const std::vector<Entity> children = scene.Children(copy);
		Check(children.size() == 1 && children[0] != child, "the duplicate's child is a new entity");
		Check(scene.Children(root).size() == 1, "the original keeps its own child");
		scene.DestroyTree(copy);
		Check(scene.Entities().size() == 3, "DestroyTree removes the duplicate and its child");
	}

	// ---- ECS limits ----------------------------------------------------------------------------
	{
		Scene scene;
		bool ok = true;
		for (int i = 0; i < 20000; ++i)
		{
			const Entity entity = scene.CreateEntity();
			scene.Add(entity, Transform{});
			if (entity == INVALID_ENTITY) ok = false;
		}
		Check(ok && scene.Entities().size() == 20000, "20000 entities fit (the old limit was 5000)");
		for (int i = 0; i < 20000; i += 2) scene.DestroyEntity(scene.Entities()[0]);
		Check(scene.Entities().size() == 10000, "destroying entities keeps the component arrays consistent");
	}

	// ---- Reflection registry ----------------------------------------------------------------------
	{
		struct TestComponent { float power = 1.0f; std::string label; };
		TypeRegistry& registry = TypeRegistry::Get();
		registry.SetCurrentOwner("tests");
		registry.Component<TestComponent>("TestComponent").Field("power", &TestComponent::power).Field("label", &TestComponent::label);
		registry.SetCurrentOwner("engine");

		Scene scene;
		const Entity entity = scene.CreateNamedEntity("Thing");
		const ComponentInfo* info = registry.Find("TestComponent");
		Check(info != nullptr, "a registered component can be found by name");
		if (info)
		{
			TestComponent* component = static_cast<TestComponent*>(info->addDefault(scene, entity));
			component->power = 5.0f;
			component->label = "hot";
		}
		const std::string savedWithType = cherry::SceneToJson(scene);
		Scene loaded;
		std::string error;
		cherry::SceneFromJson(loaded, savedWithType, error);
		const Entity loadedEntity = loaded.FindByGuid(scene.GetGuid(entity));
		Check(loadedEntity != INVALID_ENTITY && loaded.IsRegistered<TestComponent>() && loaded.Has<TestComponent>(loadedEntity)
			&& loaded.Get<TestComponent>(loadedEntity).power == 5.0f, "a game component round-trips through a scene file");

		registry.Unregister("tests");
		Check(registry.Find("TestComponent") == nullptr, "Unregister removes a module's components");
		Scene withoutType;
		cherry::SceneFromJson(withoutType, savedWithType, error);
		const Entity kept = withoutType.FindByGuid(scene.GetGuid(entity));
		Check(kept != INVALID_ENTITY && withoutType.Has<UnknownComponents>(kept), "after Unregister its data is kept as unknown");
	}

	// ---- Prefabs ------------------------------------------------------------------------------
	{
		Scene scene;
		Entity root, child;
		BuildSample(scene, root, child);
		const Entity outsider = scene.CreateNamedEntity("Outsider");
		struct Pointer { Entity target = INVALID_ENTITY; Entity inside = INVALID_ENTITY; };
		TypeRegistry::Get().SetCurrentOwner("tests");
		TypeRegistry::Get().Component<Pointer>("Pointer").EntityRef("target", &Pointer::target).EntityRef("inside", &Pointer::inside);
		TypeRegistry::Get().SetCurrentOwner("engine");
		scene.RegisterComponent<Pointer>();
		scene.Add(root, Pointer{ outsider, child });

		const std::string prefab = cherry::PrefabToJson(scene, root);
		Check(prefab.find("cherry-prefab") != std::string::npos && prefab.find("Outsider") == std::string::npos, "a prefab holds the entity and its children only");

		std::string error;
		const Entity a = cherry::InstantiatePrefab(scene, prefab, error);
		const Entity b = cherry::InstantiatePrefab(scene, prefab, error);
		Check(a != INVALID_ENTITY && b != INVALID_ENTITY && a != b, "a prefab can be placed twice");
		Check(scene.Entities().size() == 4 + 4, "each instance adds the root and its child");
		Check(scene.GetGuid(a) != scene.GetGuid(root) && scene.GetGuid(a) != scene.GetGuid(b), "instances get new guids");
		Check(scene.GetParent(a) == INVALID_ENTITY, "an instance's root has no parent");
		const std::vector<Entity> childrenA = scene.Children(a);
		Check(childrenA.size() == 1 && childrenA[0] != child, "an instance has its own child");
		const Pointer& pointer = scene.Get<Pointer>(a);
		Check(pointer.target == INVALID_ENTITY, "references to entities outside the prefab are cleared");
		Check(childrenA.size() == 1 && pointer.inside == childrenA[0], "references inside the prefab point into the instance");
		Check(scene.Has<MeshRenderer>(a) && scene.Get<MeshRenderer>(a).meshAsset == "engine/Cube", "components come with the instance");
		TypeRegistry::Get().Unregister("tests");
	}

	// ---- Scripts -------------------------------------------------------------------------------
	{
		const ComponentInfo* spinner = TypeRegistry::Get().Find("Spinner");
		Check(spinner && spinner->Is(Component_Script) && spinner->asScript, "built-in scripts are registered as scripts");

		struct BaseScript : public Script { float speed = 1.0f; };
		struct DerivedScript : public BaseScript { float jump = 2.0f; };
		TypeRegistry& registry = TypeRegistry::Get();
		registry.SetCurrentOwner("tests");
		RegisterScript<BaseScript>(registry, "BaseScript").Field("speed", &BaseScript::speed);
		RegisterScript<DerivedScript>(registry, "DerivedScript").Base<BaseScript>().Field("jump", &DerivedScript::jump);
		registry.SetCurrentOwner("engine");
		const ComponentInfo* derived = registry.Find("DerivedScript");
		Check(derived && derived->fields.size() == 2 && derived->fields[0].name == "speed", "Base<>() inherits the base class's fields first");

		Scene scene;
		const Entity entity = scene.CreateNamedEntity("Scripted");
		DerivedScript* script = derived ? static_cast<DerivedScript*>(derived->addDefault(scene, entity)) : nullptr;
		if (script) { script->speed = 7.0f; script->jump = 3.0f; }
		Spinner spin;
		spin.degreesPerSecond = 45.0f;
		scene.Add(entity, spin);
		Scene loaded;
		std::string error;
		cherry::SceneFromJson(loaded, cherry::SceneToJson(scene), error);
		const Entity loadedEntity = loaded.FindByGuid(scene.GetGuid(entity));
		Check(loadedEntity != INVALID_ENTITY && loaded.IsRegistered<DerivedScript>() && loaded.Has<DerivedScript>(loadedEntity)
			&& loaded.Get<DerivedScript>(loadedEntity).speed == 7.0f && loaded.Get<DerivedScript>(loadedEntity).jump == 3.0f,
			"a script's own and inherited fields are saved");
		Check(loadedEntity != INVALID_ENTITY && loaded.Get<Spinner>(loadedEntity).degreesPerSecond == 45.0f, "built-in script fields are saved");

		Scene play;
		cherry::CopyScene(scene, play);
		const Entity copy = play.FindByGuid(scene.GetGuid(entity));
		Check(copy != INVALID_ENTITY && play.Has<Spinner>(copy) && !play.Get<Spinner>(copy).HasStarted(), "scripts are copied into the play world, not started");
		registry.Unregister("tests");
	}


	// ---- Base<>() before the base is registered (self-registered scripts) ---------------------------
	{
		struct LateBase : public Script { float health = 10.0f; };
		struct EarlyChild : public LateBase { float wings = 2.0f; };
		TypeRegistry& registry = TypeRegistry::Get();
		registry.SetCurrentOwner("tests");
		RegisterScript<EarlyChild>(registry, "EarlyChild").Base<LateBase>().Field("wings", &EarlyChild::wings);
		const bool waited = registry.Find("EarlyChild")->fields.size() == 1;
		RegisterScript<LateBase>(registry, "LateBase").Field("health", &LateBase::health);
		registry.ResolvePendingBases();
		registry.SetCurrentOwner("engine");
		const ComponentInfo* child = registry.Find("EarlyChild");
		Check(waited && child && child->fields.size() == 2 && child->fields[0].name == "health", "Base<>() works when the base registers later");
		registry.Unregister("tests");
	}

	// ---- Shader graphs ----------------------------------------------------------------------------
	gFailures += RunShaderGraphTests();

	// ---- Voxel models -----------------------------------------------------------------------------
	gFailures += RunVoxelModelTests();

	// ---- Renderer (no GPU) ------------------------------------------------------------------------
	gFailures += RunRendererTests();

	std::printf("\n%s (%d failure%s)\n", gFailures == 0 ? "All engine tests passed" : "ENGINE TESTS FAILED", gFailures, gFailures == 1 ? "" : "s");
	return gFailures == 0 ? 0 : 1;
}
