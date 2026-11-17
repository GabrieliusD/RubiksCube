#pragma once
#include "CherryAPI.h"
#include "Renderer/RenderTypes.h"
#include "Renderer/DebugDraw.h"
#include <windows.h>
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
	void DestroyTexture(TextureHandle texture);

	MaterialHandle CreateMaterial(const MaterialDesc& desc);
	void SetMaterial(MaterialHandle material, const MaterialDesc& desc);
	const MaterialDesc* GetMaterial(MaterialHandle material) const;
	void DestroyMaterial(MaterialHandle material);

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
