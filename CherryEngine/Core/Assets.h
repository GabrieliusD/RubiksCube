#pragma once
#include "CherryAPI.h"
#include "Renderer/RenderTypes.h"
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class Renderer;

// A material asset (.mat JSON file in Content, or a material inside a glTF file).
struct MaterialFile
{
	DirectX::XMFLOAT4 baseColor{ 1, 1, 1, 1 };   // linear
	float roughness = 0.5f;
	float metallic = 0.0f;                       // 0 = dielectric, 1 = metal tinted by baseColor
	std::string albedoTexture;                   // texture asset path, or empty
};

// Loads assets by path and hands out renderer handles. Assets stay loaded for the
// session (scenes, the play world and the editor share them), and reload when their
// file changes on disk.
//
// Paths:
//   "engine/Cube", "engine/Sphere", "engine/Plane", "engine/Cylinder"   built-in meshes
//   "engine/Default", "engine/Red", ...                                   built-in materials
//   "Meshes/rock.glb"                 a file in the project's Content folder (glTF: .gltf / .glb)
//   "Meshes/rock.glb#0"               the first material of that glTF file
//   "Materials/rock.mat"              a material file
//   "Textures/rock.png"               a texture (.png .jpg .tga .bmp .dds)
//   "Prefabs/door.prefab"             a prefab (an entity tree, spawned with Engine::Spawn)
class CHERRY_API AssetManager
{
public:
	AssetManager();
	~AssetManager();
	AssetManager(const AssetManager&) = delete;
	AssetManager& operator=(const AssetManager&) = delete;

	void Initialize(Renderer& renderer);
	void Shutdown();
	// The project's Content folder (file assets are relative to it).
	void SetContentRoot(const std::filesystem::path& root);
	const std::filesystem::path& ContentRoot() const;

	// Invalid handle (and one warning) when the path names no loadable asset.
	MeshHandle GetMesh(const std::string& path);
	MaterialHandle GetMaterial(const std::string& path);
	TextureHandle GetTexture(const std::string& path);

	// Every asset path of a type ("mesh", "material", "texture", "scene"), for pickers.
	// The Content folder is scanned on first use and by Refresh().
	std::vector<std::string> List(const std::string& type);
	void Refresh();

	// "mesh", "material", "texture", "scene" or "" for an asset path / file name.
	static std::string TypeOf(const std::string& path);
	std::filesystem::path FullPath(const std::string& path) const;
	// Content-relative asset path for a file under the Content folder ("" if outside).
	std::string AssetPathOf(const std::filesystem::path& file) const;

	// Prefab files (JSON text), cached and reloaded when the file changes.
	bool ReadPrefab(const std::string& path, std::string& out);

	// Material files: read, write (and apply to the loaded material at once), create.
	bool ReadMaterial(const std::string& path, MaterialFile& out);
	bool WriteMaterial(const std::string& path, const MaterialFile& material);
	// Shows `material` on the loaded material at `path` without writing the file (live editing).
	void PreviewMaterial(const std::string& path, const MaterialFile& material);

	// Checks loaded files for changes (call about once a second). Reloads them; meshes get
	// new handles, returned as (old, new) so scenes can be patched.
	void PollChanges(std::vector<std::pair<MeshHandle, MeshHandle>>& replacedMeshes);

private:
	struct Impl;
	std::unique_ptr<Impl> mImpl;
};
