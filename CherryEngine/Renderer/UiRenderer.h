#pragma once
#include "RHI/RHI.h"
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Matrix4.h>
#include <array>
#include <initializer_list>
#include <memory>
#include <vector>

namespace rhi { class PipelineLibrary; }

// RmlUi's render interface on the RHI (Shaders/Ui.hlsl), drawing a context over the LDR view
// after the tonemap and anti-aliasing, so the UI shows in the full-window game and in the
// editor's viewport alike. All of RmlUi 6 is implemented, following its OpenGL 3 reference
// renderer: clipping (rounded and transformed boxes), layers, filters (opacity, blur,
// drop-shadow, the colour-matrix ones, mask-image), shaders (linear, radial and conic gradients,
// repeating too); so box-shadow, filter, backdrop-filter and mask-image work.
//
// Geometry: RmlUi keeps a compiled geometry's vertices alive and unchanged until it releases
// it, so compiling only remembers them; each frame the geometry that's drawn is copied into
// this frame's upload buffer (once per frame, however often it's drawn). A HUD is a few
// thousand vertices, and RmlUi recompiles an element whenever its colour or opacity changes,
// which per-geometry GPU buffers would turn into an allocation and an upload each time.
//
// Per-frame data (transforms, effect parameters) goes into one structured buffer of float4s;
// draws find theirs by offset.
//
// Layers: layer 0 is the view itself, so backdrop-filter sees the game; pushed layers, the
// three post-processing targets, the mask image and the clip mask are view-sized textures made
// when first needed and kept, as are the half, quarter... size levels blurs go down to. There's no stencil buffer: the clip mask is an R16F count per
// pixel that every UI shader tests (Ui.hlsl).
class UiRenderer : public Rml::RenderInterface
{
public:
	struct Stats
	{
		uint32_t draws = 0;
		uint32_t vertices = 0;
		uint32_t indices = 0;
		uint32_t passes = 0;       // render target switches (layers, filters, clip masks)
		uint32_t geometries = 0;   // compiled and not yet released
		uint32_t textures = 0;
		double cpuMs = 0.0;        // Context::Render, including recording the draws
	};

	void Initialize(rhi::Device& device, rhi::PipelineLibrary& pipelines, rhi::Format targetFormat);
	void Shutdown();

	// Draws `context` over `target` (which is in ShaderResource state before and after).
	// `supersampling` > 1: drawn that many times larger and averaged down into `target`
	// (smooth edges; see RenderSettings::uiSupersampling).
	void Render(rhi::CommandList& cmd, rhi::Texture& target, class Rml::Context& context, uint32_t supersampling = 1);
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

	void EnableClipMask(bool enable) override;
	void RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation) override;
	Rml::LayerHandle PushLayer() override;
	void CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blendMode,
		Rml::Span<const Rml::CompiledFilterHandle> filters) override;
	void PopLayer() override;
	Rml::TextureHandle SaveLayerAsTexture() override;
	Rml::CompiledFilterHandle SaveLayerAsMaskImage() override;
	Rml::CompiledFilterHandle CompileFilter(const Rml::String& name, const Rml::Dictionary& parameters) override;
	void ReleaseFilter(Rml::CompiledFilterHandle filter) override;
	Rml::CompiledShaderHandle CompileShader(const Rml::String& name, const Rml::Dictionary& parameters) override;
	void RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
		Rml::TextureHandle texture) override;
	void ReleaseShader(Rml::CompiledShaderHandle shader) override;

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
	struct Filter;
	struct Shader;
	// Per frame slot: vertices and indices in one upload buffer, and the float4 data.
	struct FrameBuffers
	{
		std::unique_ptr<rhi::Buffer> stream;
		std::unique_ptr<rhi::Buffer> data;
	};
	struct Pass;   // a full-view pass's settings (UiRenderer.cpp)

	Rml::TextureHandle MakeTexture(const uint8_t* rgbaPremultiplied, int width, int height, const char* name);
	bool Reserve(uint64_t vertexBytes, uint64_t indexBytes, uint64_t& vertexOffset, uint64_t& indexOffset);
	bool PrepareGeometry(Geometry& geometry);   // copies it into the stream and binds the stream
	void DrawGeometry(Geometry& geometry, const char* pipeline, uint32_t textureIndex, Rml::Vector2f translation,
		uint32_t paramOffset, float maskValue, bool clipped);
	// Data: StartData begins the frame's buffer (index 0..3: the plain projection); PushData
	// returns the offset of `count` float4s.
	void StartData(uint32_t capacity);
	uint32_t PushData(const float* float4s, uint32_t count);
	uint32_t CurrentTransformOffset();

	// Render targets.
	void EnsureTargets();
	rhi::Texture& LayerTexture(Rml::LayerHandle layer);
	rhi::Texture* TopLayer();
	// Ends the open pass, makes `reads` readable, and starts drawing into `target`.
	void BeginPass(rhi::Texture& target, std::initializer_list<rhi::Texture*> reads, bool clear = false, float clearValue = 0.0f);
	void EndPass();
	// Back to drawing into the top layer (with the clip mask readable).
	void ResumeTopLayer();
	void SetPipeline(const char* name);
	void SetScissorRect(const Rml::Rectanglei& rect);
	void ApplyScissor();
	Rml::Rectanglei ScissorRect() const;   // the active scissor, or the whole view
	void DrawPass(const Pass& pass);
	void CopyToPrimary(rhi::Texture& source);   // BlitLayerToPostprocessPrimary
	void SwapPostprocess() { std::swap(mPost[0], mPost[1]); }
	void Blur(float sigma, rhi::Texture& sourceDestination, rhi::Texture& temp, const Rml::Rectanglei& window);
	// Blur level n (1..10): the view at 1 / 2^n size, and a scratch texture of that size.
	struct BlurLevel
	{
		std::unique_ptr<rhi::Texture> image, temp;
	};
	BlurLevel& BlurLevelAt(int level);
	void ApplyFilters(Rml::Span<const Rml::CompiledFilterHandle> filters);

	rhi::Device* mDevice = nullptr;
	rhi::PipelineLibrary* mPipelines = nullptr;
	rhi::Format mFormat = rhi::Format::RGBA8Unorm;
	std::array<FrameBuffers, rhi::kFramesInFlight> mFrames;

	// While Render runs.
	rhi::CommandList* mCmd = nullptr;
	FrameBuffers* mFrame = nullptr;
	rhi::Texture* mBase = nullptr;   // layer 0: the view, or mSuper while supersampling
	std::unique_ptr<rhi::Texture> mSuper;   // the view, mScale times larger
	uint64_t mStreamUsed = 0;
	uint64_t mStreamGeneration = 0;   // goes up for every frame and every new stream buffer
	uint64_t mBoundGeneration = ~0ull;
	uint32_t mDataCount = 0;
	bool mDataOverflowed = false;     // this frame's data ran out of room
	uint32_t mDataWanted = 0;         // the room the next frames' data get (float4s)
	uint32_t mWidth = 0, mHeight = 0;   // the size drawn at: the view's times mScale
	uint32_t mScale = 1;                // supersampling: RmlUi's pixels to target pixels
	Rml::Matrix4f mProjection;
	Rml::Matrix4f mTransform;         // projection x the element's transform
	bool mHasTransform = false;
	uint32_t mTransformOffset = 0;    // where mTransform is in this frame's data (~0u: not yet)
	bool mScissorEnabled = false;
	bool mScissorEmpty = false;       // the scissor in force covers no pixel
	Rml::Rectanglei mScissor;
	rhi::Texture* mPassTarget = nullptr;
	bool mPassOpen = false;
	const char* mBoundPipeline = nullptr;
	bool mClipEnabled = false;
	float mClipRequired = 0.0f;

	// Render targets, view-sized, made when first needed.
	uint32_t mTargetWidth = 0, mTargetHeight = 0;
	std::vector<std::unique_ptr<rhi::Texture>> mLayerPool;   // pushed layers; layer n is mLayerPool[n - 1]
	uint32_t mLayerCount = 1;                                // the base layer plus the pushed ones
	std::array<std::unique_ptr<rhi::Texture>, 3> mPost;      // post-processing: primary, secondary, tertiary
	std::unique_ptr<rhi::Texture> mBlendMask;
	std::vector<BlurLevel> mBlurLevels;                      // made when a blur first needs them
	std::unique_ptr<rhi::Texture> mClipMask;

	Stats mStats;
	Stats mCounting;
	uint32_t mLiveGeometries = 0, mLiveTextures = 0;
};
