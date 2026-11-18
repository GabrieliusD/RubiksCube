#pragma once
#include "CherryAPI.h"
#include "Scene/ECS.h"
#include <filesystem>
#include <string>
#include <unordered_map>

class Scene;

// Scene files: JSON, one object per entity with its guid and its reflected components.
//
//   { "format": "cherry-scene", "version": 1,
//     "entities": [ { "guid": "1f3a...", "components": { "EntityInfo": { "name": "Floor" },
//                                                         "Transform": { "position": [0, 0, 0], ... } } } ] }
//
// Only entities with an EntityInfo are saved (entities made by code at runtime have none).
// Entity references are written as guids. Components this build doesn't know are kept
// (UnknownComponents) and written back unchanged. Transient components aren't saved.
namespace cherry
{
	// Replaces the scene's contents with the file's.
	CHERRY_API bool LoadScene(Scene& scene, const std::filesystem::path& file, std::string& error);
	CHERRY_API bool SaveScene(Scene& scene, const std::filesystem::path& file, std::string& error);

	CHERRY_API std::string SceneToJson(Scene& scene);
	CHERRY_API bool SceneFromJson(Scene& scene, const std::string& json, std::string& error);

	// Copies every entity of `from` into `to` (which should be empty) with all non-transient
	// components, including runtime handles such as meshes, so the copy renders at once.
	// Entity references are remapped to the copies. Used to make the play world.
	CHERRY_API void CopyScene(Scene& from, Scene& to);

	// Copies an entity and its children within the scene (new guids, "<name> (copy)").
	CHERRY_API Entity DuplicateEntity(Scene& scene, Entity entity);
}
