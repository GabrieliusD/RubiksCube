#pragma once
#include "CherryAPI.h"
#include "Renderer/RenderTypes.h"
#include "ShaderGraph/GraphCompiler.h"
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class Renderer;
struct VoxelModel;

// A material's value for one shader graph parameter (by name): numbers (a Scalar uses
// value[0]) or, for a texture parameter, a texture asset path.
struct MaterialParameter
{
	sg::Float4 value{ 0, 0, 0, 0 };
	std::string texture;
	bool operator==(const MaterialParameter& other) const { return value == other.value && texture == other.texture; }
};

// A material asset (.mat JSON file in Content, or a material inside a glTF file).
struct MaterialFile
{
	DirectX::XMFLOAT4 baseColor{ 1, 1, 1, 1 };   // linear
	float roughness = 0.5f;
	float metallic = 0.0f;                       // 0 = dielectric, 1 = metal tinted by baseColor
	std::string albedoTexture;                   // texture asset path, or empty
	// Surface shader: a .hlsl or .shadergraph asset path, or empty = the built-in one.
	std::string shader;
	std::vector<float> params;                   // .hlsl shaders: the parameter block (ParamFloat* in HLSL)
	// Shader graphs: values for the graph's parameters, by name. Missing ones use the graph's
	// defaults (a material instance overrides only what it sets).
	std::map<std::string, MaterialParameter> parameters;
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
//   "Models/cabin.vox"                a MagicaVoxel model, baked into a mesh (Voxel/VoxelModel.h);
//                                     meshes from .vox files default to the "engine/VoxelModel" material
//   "Materials/rock.mat"              a material file
//   "Textures/rock.png"               a texture (.png .jpg .tga .bmp .dds)
//   "Prefabs/door.prefab"             a prefab (an entity tree, spawned with Engine::Spawn)
//   "Shaders/glow.hlsl"               a surface shader (defines EvaluateSurface; see Shaders/Surface.hlsli)
//   "Shaders/rock.shadergraph"        a shader graph: a surface shader, or a function (subgraph) for other graphs
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
	// `srgb` = colour data (decoded to linear when sampled); false for normal maps and masks.
	TextureHandle GetTexture(const std::string& path, bool srgb = true);
	// A surface shader (.hlsl) or shader graph (.shadergraph), compiled into its own pipeline
	// (Renderer::CreateSurfaceShader). The handle stays the same when the file changes: it is
	// recompiled in place, and Renderer::SurfaceShaderError / ShaderGraphErrors tell whether
	// that worked (the last version that compiled keeps running).
	SurfaceShaderHandle GetSurfaceShader(const std::string& path);

	// ---- Shader graphs ----------------------------------------------------------------------
	bool ReadShaderGraph(const std::string& path, sg::Graph& out);
	// Writes the file and applies it at once (recompiles it and the graphs that use it as a subgraph).
	bool WriteShaderGraph(const std::string& path, const sg::Graph& graph);
	// Compiles `graph` in place of the file's, without writing it (live editing in the graph
	// editor): materials using it, and graphs using it as a subgraph, show it at once.
	sg::CompiledGraph PreviewShaderGraph(const std::string& path, const sg::Graph& graph);
	// Ends a preview: back to the file's version.
	void RevertShaderGraph(const std::string& path);
	// The parameters of a shader graph (of the version that's running), or nullptr.
	const std::vector<sg::GraphParameter>* ShaderGraphParameters(const std::string& path);
	// Errors of the last compile of a shader graph (empty when it compiled).
	std::vector<sg::GraphError> ShaderGraphErrors(const std::string& path);
	// Loads graphs by path for Subgraph nodes (previews included).
	sg::GraphLoader GraphLoader();
	// A material using a surface shader with its default parameters (the graph editor's
	// preview); it follows recompiles.
	MaterialHandle ShaderPreviewMaterial(const std::string& shaderPath);

	// Every asset path of a type ("mesh", "material", "texture", "scene", "shader", "shadergraph"), for pickers.
	// The Content folder is scanned on first use and by Refresh().
	std::vector<std::string> List(const std::string& type);
	void Refresh();

	// "mesh", "material", "texture", "scene", "prefab", "shader", "shadergraph" or "" for an asset path / file name.
	static std::string TypeOf(const std::string& path);
	std::filesystem::path FullPath(const std::string& path) const;
	// Content-relative asset path for a file under the Content folder ("" if outside).
	std::string AssetPathOf(const std::filesystem::path& file) const;

	// Prefab files (JSON text), cached and reloaded when the file changes.
	bool ReadPrefab(const std::string& path, std::string& out);

	// A voxel model file ("Models/cabin.vox"), e.g. for its collision (BakeVoxelBoxes).
	// As a mesh, the same path works with GetMesh (baked with the default VoxelBakeOptions).
	bool ReadVoxelModel(const std::string& path, VoxelModel& out);

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
