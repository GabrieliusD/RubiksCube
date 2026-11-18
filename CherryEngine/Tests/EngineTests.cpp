// Tests for the scene, reflection and scene files (no window, no GPU). Run via CTest or directly.
#include <Scene/Scene.h>
#include <Scene/SceneSerializer.h>
#include <cmath>
#include <cstdio>
#include <string>

using namespace DirectX;

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

	std::printf("\n%s (%d failure%s)\n", gFailures == 0 ? "All engine tests passed" : "ENGINE TESTS FAILED", gFailures, gFailures == 1 ? "" : "s");
	return gFailures == 0 ? 0 : 1;
}
