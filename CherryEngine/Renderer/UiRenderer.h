#pragma once
#include "RHI/RHI.h"
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Matrix4.h>
#include <array>
#include <memory>
#include <vector>

namespace rhi { class PipelineLibrary; }

// RmlUi's render interface on the RHI (Shaders/Ui.hlsl), drawing a context over the LDR view
// after the tonemap and anti-aliasing, so the UI shows in the full-window game and in the
// editor's viewport alike.
//
// Geometry: RmlUi keeps a compiled geometry's vertices alive and unchanged until it releases
// it, so compiling only remembers them; each frame the geometry that's drawn is copied into
// this frame's upload buffers (once per frame, however often it's drawn). A HUD is a few
// thousand vertices, and RmlUi recompiles an element whenever its colour or opacity changes,
// which per-geometry GPU buffers would turn into an allocation and an upload each time.
//
// Transforms: projection x the element's transform, one float4x4 per SetTransform in a
// per-frame structured buffer; draws pick theirs by index.
//
// Not done yet (first pass): clip masks (rounded or transformed clipping falls back to the
// scissor rectangle), layers, filters (blur, drop-shadow) and shaders (gradients); RmlUi skips
// what they draw.
class UiRenderer : public Rml::RenderInterface
{
public:
	struct Stats
	{
		uint32_t draws = 0;
		uint32_t vertices = 0;
		uint32_t indices = 0;
		uint32_t transforms = 0;
		uint32_t geometries = 0;   // compiled and not yet released
		uint32_t textures = 0;
		double cpuMs = 0.0;        // Context::Render, including recording the draws
	};

	void Initialize(rhi::Device& device, rhi::PipelineLibrary& pipelines, rhi::Format targetFormat);
	void Shutdown();

	// Draws `context` over `target` (which is in ShaderResource state before and after).
	void Render(rhi::CommandList& cmd, rhi::Texture& target, class Rml::Context& context);
	const Stats& LastStats() const { return mStats; }

	// ---- Rml::RenderInterface ----
	Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
	void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
	void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
	Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override;
	Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override;
	void ReleaseTexture(Rml::TextureHandle texture) override;
	void EnableScissorRegion(bool enable) override;
	void SetScissorRegion(Rml::Rectanglei region) override;
	void SetTransform(const Rml::Matrix4f* transform) override;

private:
	struct Geometry
	{
		Rml::Span<const Rml::Vertex> vertices;
		Rml::Span<const int> indices;
		uint64_t copiedStream = ~0ull;   // the stream buffer (mStreamGeneration) its vertices were last copied into
		uint32_t baseVertex = 0;
		uint32_t firstIndex = 0;
	};
	struct Texture
	{
		std::unique_ptr<rhi::Texture> texture;
	};
	// Per frame slot: vertices then indices in one upload buffer, and the transforms.
	struct FrameBuffers
	{
		std::unique_ptr<rhi::Buffer> stream;
		std::unique_ptr<rhi::Buffer> transforms;
	};

	Rml::TextureHandle MakeTexture(const uint8_t* rgbaPremultiplied, int width, int height, const char* name);
	bool Reserve(uint64_t vertexBytes, uint64_t indexBytes, uint64_t& vertexOffset, uint64_t& indexOffset);
	void BindStream();
	// Index 0 of the transforms is always the plain projection.
	void StartTransforms(uint32_t capacity);
	uint32_t PushTransform(const Rml::Matrix4f& transform);
	void ApplyScissor();

	rhi::Device* mDevice = nullptr;
	rhi::PipelineLibrary* mPipelines = nullptr;
	std::array<FrameBuffers, rhi::kFramesInFlight> mFrames;

	// While Render runs.
	rhi::CommandList* mCmd = nullptr;
	FrameBuffers* mFrame = nullptr;
	uint64_t mStreamUsed = 0;
	uint32_t mTransformCount = 0;
	uint32_t mTransformIndex = 0;
	float mWidth = 0, mHeight = 0;
	Rml::Matrix4f mProjection;
	bool mScissorEnabled = false;
	Rml::Rectanglei mScissor;
	uint64_t mStreamGeneration = 0;   // goes up for every frame and every new stream buffer
	uint64_t mBoundGeneration = ~0ull;

	Stats mStats;
	Stats mCounting;
	uint32_t mLiveGeometries = 0, mLiveTextures = 0;
};
