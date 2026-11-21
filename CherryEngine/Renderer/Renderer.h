#pragma once
#include "CherryAPI.h"
#include "Renderer/RenderTypes.h"
#include "Renderer/DebugDraw.h"
#include <windows.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

class Scene;

// Camera data for one rendered view.
struct RenderView
{
	DirectX::XMFLOAT4X4 view;
	DirectX::XMFLOAT4X4 proj;
	DirectX::XMFLOAT3 position{ 0, 0, 0 };
	float nearZ = 0.1f;
	float farZ = 1000.0f;
};

// The engine's renderer. Games use the resource functions (meshes, materials,
// textures, voxel volumes), Settings() and Debug(); the frame functions are
// driven by Engine.
//
// Frame structure:
//   MeshPass   (forward, HDR + depth; point lights + fog; RenderLayer::World)
//   VoxelPass  (ray-marched volumes, writes SV_Depth into the same depth buffer)
//   DebugDraw  (lines into HDR, depth tested against the world)
//   ViewModels (depth cleared, then RenderLayer::ViewModel meshes)
//   Tonemap    (compute: HDR -> gamma-encoded LDR view texture)
//   Present    (LDR blitted full-window in game mode, or shown in the editor
//               viewport via ImGui; ImGui drawn on top)
class CHERRY_API Renderer
{
public:
	Renderer();
	~Renderer();
	Renderer(const Renderer&) = delete;
	Renderer& operator=(const Renderer&) = delete;

	// ---- Resources (game-facing) ---------------------------------------------
	MeshHandle CreateMesh(const MeshData& data);
	void DestroyMesh(MeshHandle mesh);
	// Local-space bounds, or nullptr for an invalid handle.
	const DirectX::BoundingBox* GetMeshBounds(MeshHandle mesh) const;

	// Loads a .dds texture. Relative paths resolve against the executable folder.
	// `srgb` = colour data stored gamma-encoded (true for albedo maps).
	TextureHandle LoadTexture(const std::wstring& path, bool srgb = true);
	// From RGBA8 pixels (rows top to bottom, `width * 4` bytes each). Builds the mip chain.
	TextureHandle CreateTexture(uint32_t width, uint32_t height, const uint8_t* rgba, bool srgb = true);
	void DestroyTexture(TextureHandle texture);

	MaterialHandle CreateMaterial(const MaterialDesc& desc);
	void SetMaterial(MaterialHandle material, const MaterialDesc& desc);
	const MaterialDesc* GetMaterial(MaterialHandle material) const;
	void DestroyMaterial(MaterialHandle material);

	// Surface shaders: HLSL defining `SurfaceOutput EvaluateSurface(SurfaceInput input)`
	// (see Shaders/Surface.hlsli), compiled into a mesh-pass pipeline of its own and used by
	// materials whose MaterialDesc::shader is set. `name` labels compiler errors (e.g. an
	// asset path). Engine shader edits rebuild them too.
	//
	// Builds run on worker threads: the handle is usable at once, and `done` runs on the main
	// thread (in BeginFrame) with whether the build worked. Until a first build works, meshes
	// using it draw with the built-in surface shader while it compiles, and as a magenta
	// checker if it failed. A failed rebuild keeps the last version that compiled. A newer
	// SetSurfaceShaderSource makes a pending one moot (its `done` never runs).
	SurfaceShaderHandle CreateSurfaceShader(const std::string& name, const std::string& source, const SurfaceShaderOptions& options = {},
		std::function<void(bool ok)> done = {});
	void SetSurfaceShaderSource(SurfaceShaderHandle shader, const std::string& source, const SurfaceShaderOptions& options = {},
		std::function<void(bool ok)> done = {});
	// A build of this shader is still running.
	bool IsSurfaceShaderCompiling(SurfaceShaderHandle shader) const;
	void DestroySurfaceShader(SurfaceShaderHandle shader);
	// Compiler output of the last build ("" when it compiled). Voxel volumes using a surface
	// shader get a second pipeline from it, built the first time one draws; its errors are
	// reported here too.
	const std::string& SurfaceShaderError(SurfaceShaderHandle shader) const;

	VoxelVolumeHandle CreateVoxelVolume(const VoxelVolumeDesc& desc);
	// Overwrites a box of voxels (palette indices, x fastest, then y, then z).
	// Synchronous; meant for edits such as mining, not for streaming whole volumes.
	bool UpdateVoxelVolume(VoxelVolumeHandle volume, const DirectX::XMUINT3& offset, const DirectX::XMUINT3& size, const uint8_t* voxels);
	void DestroyVoxelVolume(VoxelVolumeHandle volume);
	DirectX::XMUINT3 GetVoxelVolumeSize(VoxelVolumeHandle volume) const;

	RenderSettings& Settings();
	DebugDraw& Debug();
	const RenderStats& Stats() const;
	const std::string& AdapterName() const;
	void ReloadShaders();
	const std::string& ShaderError() const;

	// ---- Frame (engine-facing) -----------------------------------------------
	bool Initialize(HWND hwnd, uint32_t width, uint32_t height, std::string& error);
	void Shutdown();
	void OnWindowResized(uint32_t width, uint32_t height);
	// Size of the offscreen view the scene is rendered into (applied next frame).
	void SetViewSize(uint32_t width, uint32_t height);
	uint32_t ViewWidth() const;
	uint32_t ViewHeight() const;
	// ImTextureID of the final (LDR) view, for showing it in an ImGui window.
	unsigned long long ViewTextureId() const;

	// Renders one mesh with one material into a square offscreen image (the material preview
	// of the shader graph editor): lit by the sun, fill lights and ambient of Settings(), no
	// point lights or fog, the camera orbiting the mesh (yaw/pitch in radians). Call after
	// BeginFrame, once per frame at most. Returns an ImTextureID, or 0.
	unsigned long long RenderPreview(MeshHandle mesh, MaterialHandle material, uint32_t size, float yaw, float pitch);

	// Starts the GPU frame and the ImGui frame.
	void BeginFrame(float totalTime, float deltaTime);
	void RenderScene(Scene& scene, const RenderView& view);
	// Composites, renders ImGui and presents. `viewFullscreen` blits the view to
	// the whole window (game mode); otherwise only ImGui is drawn (editor mode).
	void EndFrame(bool viewFullscreen);

private:
	struct Impl;
	std::unique_ptr<Impl> mImpl;
};

// Compiles a surface shader the way CreateSurfaceShader does (vertex and pixel shader), with
// DXC only: no device needed. For tests and tools. `shaderRoot` is the engine's Shaders folder;
// `voxel` compiles it for the voxel pass instead of the mesh pass.
CHERRY_API bool CompileSurfaceShaderOffline(const std::string& name, const std::string& source,
	const std::filesystem::path& shaderRoot, std::string& errors, bool voxel = false);
