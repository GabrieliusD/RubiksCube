#pragma once
#include "RHI/RHI.h"
#include <array>
#include <memory>

struct ImDrawData;
namespace rhi { class PipelineLibrary; }

// Draws Dear ImGui on any RHI backend: one pipeline (Shaders/ImGui.hlsl), the draw lists in
// per-frame upload buffers, and ImGui 1.92's textures (the font atlas) as bindless textures. A
// texture id is a bindless SRV index + 1, so renderer images (the view, previews) are shown
// with ImGui::Image(TextureId(srvIndex)).
class ImGuiRenderer
{
public:
	void Initialize(rhi::Device& device, rhi::PipelineLibrary& pipelines);
	// Destroys ImGui's textures (call before the ImGui context goes).
	void Shutdown();
	// Inside BeginRendering on the back buffer.
	void Render(ImDrawData* data, rhi::CommandList& cmd);

	static unsigned long long TextureId(uint32_t srvIndex) { return srvIndex == rhi::kInvalidIndex ? 0ull : srvIndex + 1ull; }

private:
	void UpdateTextures(ImDrawData* data);

	rhi::Device* mDevice = nullptr;
	rhi::PipelineLibrary* mPipelines = nullptr;
	std::array<std::unique_ptr<rhi::Buffer>, rhi::kFramesInFlight> mVertices;
	std::array<std::unique_ptr<rhi::Buffer>, rhi::kFramesInFlight> mIndices;
};
