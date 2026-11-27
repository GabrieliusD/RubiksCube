#include "Renderer/UiRenderer.h"
#include "RHI/Pipeline.h"
#include "Core/Log.h"
#include "Shaders/Shared/ShaderInterop.h"
#include <RmlUi/Core.h>
#include <stb_image.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>

namespace
{
	constexpr uint32_t kVertexBytes = sizeof(Rml::Vertex);   // 20
	static_assert(sizeof(Rml::Vertex) == 20, "VertexLayout::RmlUi expects 20-byte vertices");
	static_assert(sizeof(gpu::UiConstants) <= rhi::kDrawConstantBytes, "UiConstants must fit the draw constants");
	constexpr uint64_t kInitialStreamBytes = 256 * 1024;
	constexpr uint32_t kInitialData = 16 * 1024;   // float4s per frame: transforms, gradients, filters
	constexpr int kMaxGradientStops = 16;
	constexpr int kBlurWeights = 9;               // Ui.hlsl's PSBlur: 17 taps
	constexpr float kMaxPassSigma = 3.0f;         // larger blurs run at a halved resolution, as often as needed
	constexpr rhi::Format kClipMaskFormat = rhi::Format::R16Float;
	constexpr uint64_t AlignUp(uint64_t value, uint64_t alignment) { return (value + alignment - 1) / alignment * alignment; }

	enum class GradientFunction { Linear, Radial, Conic, RepeatingLinear, RepeatingRadial, RepeatingConic };   // as in Ui.hlsl
	enum class FilterType { Opacity, Blur, DropShadow, ColorMatrix, MaskImage };

	Rml::Colourf ToFloat(Rml::ColourbPremultiplied c)
	{
		return Rml::Colourf(c.red / 255.0f, c.green / 255.0f, c.blue / 255.0f, c.alpha / 255.0f);
	}
}

struct UiRenderer::Filter
{
	FilterType type = FilterType::Opacity;
	float value = 1.0f;              // opacity
	float sigma = 0.0f;              // blur, drop-shadow
	Rml::Vector2f offset;            // drop-shadow
	Rml::Colourf color;              // drop-shadow (premultiplied)
	Rml::Matrix4f matrix;            // colour matrix
};

struct UiRenderer::Shader
{
	// Ui.hlsl's PSGradient parameters: (function, stops), (p, v), 4 x 4 positions, 16 colours.
	std::array<float, 4 * (6 + kMaxGradientStops)> data{};
};

struct UiRenderer::Pass
{
	const char* pipeline = "ui_copy";
	rhi::Texture* source = nullptr;
	uint32_t paramOffset = 0;
	float color[4] = { 1, 1, 1, 1 };
	float uvScale[2] = { 1, 1 };
	float uvOffset[2] = { 0, 0 };
	bool clipped = false;
};

void UiRenderer::Initialize(rhi::Device& device, rhi::PipelineLibrary& pipelines, rhi::Format targetFormat)
{
	mDevice = &device;
	mPipelines = &pipelines;
	mFormat = targetFormat;
	auto add = [&](const char* name, const char* vs, const char* ps, rhi::Format format, bool geometry, int blend)
	{
		rhi::GraphicsPipelineDesc desc;
		desc.shaderFile = "Ui.hlsl";
		desc.vsEntry = vs;
		desc.psEntry = ps;
		desc.vertexLayout = geometry ? rhi::VertexLayout::RmlUi : rhi::VertexLayout::None;
		desc.colorFormats = { format };
		desc.depthTest = false;
		desc.depthWrite = false;
		desc.cullMode = rhi::CullMode::None;
		desc.premultipliedBlend = blend == 1;
		desc.additiveBlend = blend == 2;
		pipelines.AddGraphics(name, desc);
	};
	// Geometry.
	add("ui", "VSMain", "PSMain", targetFormat, true, 1);
	add("ui_gradient", "VSMain", "PSGradient", targetFormat, true, 1);
	add("ui_mask_write", "VSMain", "PSMaskWrite", kClipMaskFormat, true, 0);
	add("ui_mask_add", "VSMain", "PSMaskAdd", kClipMaskFormat, true, 2);
	// Full-view passes.
	add("ui_mask_fill", "VSFullscreen", "PSMaskWrite", kClipMaskFormat, false, 0);
	add("ui_copy", "VSFullscreen", "PSCopy", targetFormat, false, 0);
	add("ui_copy_point", "VSFullscreen", "PSCopyPoint", targetFormat, false, 0);
	add("ui_composite", "VSFullscreen", "PSCopy", targetFormat, false, 1);
	add("ui_color_matrix", "VSFullscreen", "PSColorMatrix", targetFormat, false, 0);
	add("ui_blend_mask", "VSFullscreen", "PSBlendMask", targetFormat, false, 0);
	add("ui_blur", "VSFullscreen", "PSBlur", targetFormat, false, 0);
	add("ui_drop_shadow", "VSFullscreen", "PSDropShadow", targetFormat, false, 0);
}

void UiRenderer::Shutdown()
{
	for (FrameBuffers& frame : mFrames)
	{
		frame.stream.reset();
		frame.data.reset();
	}
	mLayerPool.clear();
	mBlurLevels.clear();
	for (auto& post : mPost) post.reset();
	mBlendMask.reset();
	mClipMask.reset();
	mSuper.reset();
	if (mLiveGeometries || mLiveTextures)
		CHERRY_WARN("UI: %u geometries and %u textures were never released", mLiveGeometries, mLiveTextures);
}

void UiRenderer::Render(rhi::CommandList& cmd, rhi::Texture& target, Rml::Context& context, uint32_t supersampling)
{
	if (!mPipelines->Get("ui")) return;
	const auto start = std::chrono::steady_clock::now();

	mCmd = &cmd;
	mFrame = &mFrames[mDevice->FrameIndex()];
	mStreamGeneration++;   // last frame's copies are in another frame slot's buffer
	mStreamUsed = 0;
	mScale = std::clamp(supersampling, 1u, 4u);
	mWidth = target.Width() * mScale;
	mHeight = target.Height() * mScale;
	mScissorEnabled = false;
	mClipEnabled = false;
	mClipRequired = 0.0f;
	mLayerCount = 1;
	mCounting = {};
	if (mWidth != mTargetWidth || mHeight != mTargetHeight)
	{
		// The view changed size: effect targets are made again when next needed.
		mTargetWidth = mWidth;
		mTargetHeight = mHeight;
		mLayerPool.clear();
		mBlurLevels.clear();
		for (auto& post : mPost) post.reset();
		mBlendMask.reset();
		mClipMask.reset();
		mSuper.reset();
	}

	// RmlUi's pixels (y down, the view's size) to clip space, whatever size is drawn at.
	mProjection = Rml::Matrix4f::ProjectOrtho(0.0f, static_cast<float>(target.Width()), static_cast<float>(target.Height()), 0.0f, -10000.0f, 10000.0f);
	mHasTransform = false;
	StartData(std::max(mDataWanted, kInitialData));

	cmd.BeginEvent("UI");
	if (mScale > 1)
	{
		// Supersampled: the view copied into a larger base layer (so backdrop filters see the
		// game), the UI drawn there, then averaged back down into the view.
		if (!mSuper)
		{
			rhi::TextureDesc desc;
			desc.width = mWidth;
			desc.height = mHeight;
			desc.format = mFormat;
			desc.usage = rhi::TextureUsage_RenderTarget | rhi::TextureUsage_ShaderResource;
			desc.initialState = rhi::ResourceState::ShaderResource;
			desc.name = "UiSupersampled";
			mSuper = mDevice->CreateTexture(desc);
		}
		mBase = mSuper.get();
		BeginPass(*mSuper, { &target });
		Pass up;
		up.pipeline = "ui_copy_point";
		up.source = &target;
		DrawPass(up);
		context.Render();
		BeginPass(target, { mSuper.get() });
		Pass down;
		down.source = mSuper.get();   // bilinear at each view pixel's centre: the mean of its block (2x)
		DrawPass(down);
		EndPass();
	}
	else
	{
		mBase = &target;
		BeginPass(target, {});
		context.Render();
		EndPass();
	}
	cmd.EndEvent();
	target.Transition(cmd, rhi::ResourceState::ShaderResource);

	mCmd = nullptr;
	mFrame = nullptr;
	mBase = nullptr;
	mCounting.geometries = mLiveGeometries;
	mCounting.textures = mLiveTextures;
	mCounting.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	mStats = mCounting;
}

// ---- Per-frame buffers ---------------------------------------------------------------------

// Room for one geometry in this frame's stream buffer. A full buffer is replaced by one twice
// the size (the old one stays alive until the GPU is done with this frame).
bool UiRenderer::Reserve(uint64_t vertexBytes, uint64_t indexBytes, uint64_t& vertexOffset, uint64_t& indexOffset)
{
	// Every block starts on a multiple of 20 bytes, so vertex offsets are whole vertices and
	// index offsets whole indices.
	const uint64_t needed = AlignUp(vertexBytes, kVertexBytes) + AlignUp(indexBytes, kVertexBytes);
	if (!mFrame->stream || mStreamUsed + needed > mFrame->stream->Size())
	{
		rhi::BufferDesc desc;
		desc.size = AlignUp(std::max<uint64_t>({ kInitialStreamBytes, needed * 2, mFrame->stream ? mFrame->stream->Size() * 2 : 0 }), kVertexBytes);
		desc.heap = rhi::HeapKind::Upload;
		desc.name = "UiStream";
		mFrame->stream = mDevice->CreateBuffer(desc);
		mStreamGeneration++;
		mStreamUsed = 0;
		if (!mFrame->stream || !mFrame->stream->Mapped()) return false;
	}
	vertexOffset = mStreamUsed;
	indexOffset = mStreamUsed + AlignUp(vertexBytes, kVertexBytes);
	mStreamUsed += needed;
	return true;
}

bool UiRenderer::PrepareGeometry(Geometry& geometry)
{
	if (geometry.indices.empty()) return false;
	// Copied into this frame's stream once; drawn again (or the stream replaced since), it's
	// copied again.
	if (geometry.copiedStream != mStreamGeneration)
	{
		const uint64_t vertexBytes = geometry.vertices.size() * kVertexBytes;
		const uint64_t indexBytes = geometry.indices.size() * sizeof(int);
		uint64_t vertexOffset = 0, indexOffset = 0;
		if (!Reserve(vertexBytes, indexBytes, vertexOffset, indexOffset)) return false;
		auto* base = static_cast<uint8_t*>(mFrame->stream->Mapped());
		std::memcpy(base + vertexOffset, geometry.vertices.data(), vertexBytes);
		std::memcpy(base + indexOffset, geometry.indices.data(), indexBytes);
		geometry.copiedStream = mStreamGeneration;
		geometry.baseVertex = static_cast<uint32_t>(vertexOffset / kVertexBytes);
		geometry.firstIndex = static_cast<uint32_t>(indexOffset / sizeof(int));
		mCounting.vertices += static_cast<uint32_t>(geometry.vertices.size());
	}
	if (mBoundGeneration != mStreamGeneration)
	{
		mBoundGeneration = mStreamGeneration;
		mCmd->SetVertexBuffer(*mFrame->stream, kVertexBytes);
		mCmd->SetIndexBuffer(*mFrame->stream, false);
	}
	return true;
}

// The frame's float4 data. It never grows mid-frame (offsets already handed out would point into
// the old buffer): what doesn't fit is skipped, and the next frame in this slot gets twice the room.
void UiRenderer::StartData(uint32_t capacity)
{
	if (!mFrame->data || mFrame->data->Size() / 16 < capacity)
	{
		rhi::BufferDesc desc;
		desc.size = static_cast<uint64_t>(capacity) * 16;
		desc.stride = 16;
		desc.heap = rhi::HeapKind::Upload;
		desc.createSrv = true;
		desc.name = "UiData";
		mFrame->data = mDevice->CreateBuffer(desc);
	}
	std::memcpy(mFrame->data->Mapped(), mProjection.data(), 64);   // offset 0: the plain projection
	mDataCount = 4;
	mDataOverflowed = false;
}

uint32_t UiRenderer::PushData(const float* float4s, uint32_t count)
{
	const uint32_t capacity = static_cast<uint32_t>(mFrame->data->Size() / 16);
	if (mDataCount + count > capacity)
	{
		if (!mDataOverflowed) CHERRY_WARN("UI: this frame's effect data overflowed %u float4s (some drawing skipped); growing", capacity);
		mDataOverflowed = true;
		mDataWanted = std::max(mDataWanted, capacity * 2);
		return ~0u;
	}
	std::memcpy(static_cast<uint8_t*>(mFrame->data->Mapped()) + static_cast<size_t>(mDataCount) * 16, float4s, static_cast<size_t>(count) * 16);
	const uint32_t offset = mDataCount;
	mDataCount += count;
	return offset;
}

uint32_t UiRenderer::CurrentTransformOffset()
{
	if (!mHasTransform) return 0;
	if (mTransformOffset == ~0u) mTransformOffset = PushData(mTransform.data(), 4);
	return mTransformOffset;
}

// ---- Render targets ------------------------------------------------------------------------

void UiRenderer::EnsureTargets()
{
	auto make = [&](std::unique_ptr<rhi::Texture>& texture, rhi::Format format, const char* name)
	{
		if (texture) return;
		rhi::TextureDesc desc;
		desc.width = mTargetWidth;
		desc.height = mTargetHeight;
		desc.format = format;
		desc.usage = rhi::TextureUsage_RenderTarget | rhi::TextureUsage_ShaderResource;
		desc.clearColor[3] = 0.0f;   // cleared to transparent black
		desc.initialState = rhi::ResourceState::ShaderResource;
		desc.name = name;
		texture = mDevice->CreateTexture(desc);
	};
	make(mPost[0], mFormat, "UiPostprocess");
	make(mPost[1], mFormat, "UiPostprocess");
	make(mPost[2], mFormat, "UiPostprocess");
	make(mBlendMask, mFormat, "UiMaskImage");
	make(mClipMask, kClipMaskFormat, "UiClipMask");
	while (mLayerPool.size() + 1 < mLayerCount)
	{
		std::unique_ptr<rhi::Texture> layer;
		make(layer, mFormat, "UiLayer");
		mLayerPool.push_back(std::move(layer));
	}
}

rhi::Texture& UiRenderer::LayerTexture(Rml::LayerHandle layer)
{
	return layer == 0 ? *mBase : *mLayerPool[static_cast<size_t>(layer) - 1];
}

rhi::Texture* UiRenderer::TopLayer() { return &LayerTexture(mLayerCount - 1); }

void UiRenderer::EndPass()
{
	if (!mPassOpen) return;
	mCmd->EndRendering();
	mPassOpen = false;
}

void UiRenderer::BeginPass(rhi::Texture& target, std::initializer_list<rhi::Texture*> reads, bool clear, float clearValue)
{
	EndPass();
	for (rhi::Texture* read : reads)
		if (read && read != &target) read->Transition(*mCmd, rhi::ResourceState::ShaderResource);
	target.Transition(*mCmd, rhi::ResourceState::RenderTarget);
	rhi::RenderPassDesc pass;
	pass.color = &target;
	pass.clearColor = clear;
	pass.clearValue[0] = clearValue;
	pass.clearValue[1] = pass.clearValue[2] = pass.clearValue[3] = 0.0f;
	mCmd->BeginRendering(pass);
	mPassOpen = true;
	mPassTarget = &target;
	mBoundPipeline = nullptr;
	mBoundGeneration = ~0ull;
	mCmd->SetViewport(0.0f, 0.0f, static_cast<float>(target.Width()), static_cast<float>(target.Height()));
	ApplyScissor();
	mCounting.passes++;
}

void UiRenderer::ResumeTopLayer()
{
	BeginPass(*TopLayer(), { mClipEnabled ? mClipMask.get() : nullptr });
}

void UiRenderer::SetPipeline(const char* name)
{
	if (mBoundPipeline && std::strcmp(mBoundPipeline, name) == 0) return;
	if (rhi::Pipeline* pipeline = mPipelines->Get(name))
	{
		mCmd->SetPipeline(pipeline);
		mBoundPipeline = name;
	}
}

Rml::Rectanglei UiRenderer::ScissorRect() const
{
	// In target pixels (RmlUi's scissor is in its own pixels: scaled while supersampling).
	const Rml::Rectanglei whole = Rml::Rectanglei::FromSize(Rml::Vector2i(static_cast<int>(mWidth), static_cast<int>(mHeight)));
	if (!mScissorEnabled) return whole;
	const int s = static_cast<int>(mScale);
	return Rml::Rectanglei::FromCorners(mScissor.p0 * s, mScissor.p1 * s).Intersect(whole);
}

void UiRenderer::SetScissorRect(const Rml::Rectanglei& rect)
{
	if (!mPassOpen) return;
	const int32_t width = static_cast<int32_t>(mPassTarget->Width()), height = static_cast<int32_t>(mPassTarget->Height());
	const int32_t left = std::clamp(rect.Left(), 0, width), top = std::clamp(rect.Top(), 0, height);
	const int32_t right = std::clamp(rect.Right(), left, width), bottom = std::clamp(rect.Bottom(), top, height);
	mScissorEmpty = right <= left || bottom <= top;   // all outside the view: draws are skipped
	mCmd->SetScissor(left, top, right, bottom);
}

void UiRenderer::ApplyScissor() { SetScissorRect(ScissorRect()); }

// ---- Drawing -------------------------------------------------------------------------------

void UiRenderer::DrawGeometry(Geometry& geometry, const char* pipeline, uint32_t textureIndex, Rml::Vector2f translation,
	uint32_t paramOffset, float maskValue, bool clipped)
{
	if (!mCmd || mScissorEmpty || !PrepareGeometry(geometry)) return;
	const uint32_t transformOffset = CurrentTransformOffset();
	if (transformOffset == ~0u || paramOffset == ~0u) return;   // the frame's data is full (see PushData)
	SetPipeline(pipeline);
	gpu::UiConstants constants{};
	constants.translation = { translation.x, translation.y };
	constants.textureIndex = textureIndex;
	constants.dataIndex = mFrame->data->SrvIndex();
	constants.transformOffset = transformOffset;
	constants.paramOffset = paramOffset;
	constants.clipMaskIndex = clipped && mClipEnabled && mClipMask ? mClipMask->SrvIndex() : CHERRY_INVALID_INDEX;
	constants.clipRequired = mClipRequired;
	constants.color = { maskValue, maskValue, maskValue, maskValue };
	constants.uvScale = { 1, 1 };
	mCmd->SetConstants(constants);
	mCmd->DrawIndexed(static_cast<uint32_t>(geometry.indices.size()), 1, geometry.firstIndex, static_cast<int32_t>(geometry.baseVertex), 0);
	mCounting.draws++;
	mCounting.indices += static_cast<uint32_t>(geometry.indices.size());
}

void UiRenderer::DrawPass(const Pass& pass)
{
	if (mScissorEmpty) return;
	SetPipeline(pass.pipeline);
	gpu::UiConstants constants{};
	constants.textureIndex = pass.source ? pass.source->SrvIndex() : CHERRY_INVALID_INDEX;
	constants.dataIndex = mFrame->data->SrvIndex();
	constants.paramOffset = pass.paramOffset;
	constants.clipMaskIndex = pass.clipped && mClipEnabled && mClipMask ? mClipMask->SrvIndex() : CHERRY_INVALID_INDEX;
	constants.clipRequired = mClipRequired;
	constants.color = { pass.color[0], pass.color[1], pass.color[2], pass.color[3] };
	constants.uvScale = { pass.uvScale[0], pass.uvScale[1] };
	constants.uvOffset = { pass.uvOffset[0], pass.uvOffset[1] };
	mCmd->SetConstants(constants);
	mCmd->Draw(3);
	mCounting.draws++;
}

Rml::CompiledGeometryHandle UiRenderer::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices)
{
	auto* geometry = new Geometry{ vertices, indices };
	++mLiveGeometries;
	return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry);
}

void UiRenderer::ReleaseGeometry(Rml::CompiledGeometryHandle handle)
{
	delete reinterpret_cast<Geometry*>(handle);
	--mLiveGeometries;
}

void UiRenderer::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle textureHandle)
{
	if (!handle) return;
	const Texture* texture = reinterpret_cast<const Texture*>(textureHandle);
	DrawGeometry(*reinterpret_cast<Geometry*>(handle), "ui", texture && texture->texture ? texture->texture->SrvIndex() : CHERRY_INVALID_INDEX,
		translation, 0, 0.0f, true);
}

// ---- Textures ------------------------------------------------------------------------------

Rml::TextureHandle UiRenderer::MakeTexture(const uint8_t* rgba, int width, int height, const char* name)
{
	if (width <= 0 || height <= 0) return 0;
	rhi::TextureDesc desc;
	desc.width = static_cast<uint32_t>(width);
	desc.height = static_cast<uint32_t>(height);
	desc.format = rhi::Format::RGBA8Unorm;   // RmlUi's colours are gamma-encoded, like the LDR view
	desc.name = name;
	auto* texture = new Texture{ mDevice->CreateTexture(desc) };
	if (!texture->texture)
	{
		delete texture;
		return 0;
	}
	const uint32_t pitch = static_cast<uint32_t>(width) * 4;
	rhi::SubresourceData pixels{ rgba, pitch, pitch * static_cast<uint32_t>(height) };
	texture->texture->Upload(&pixels, 1, rhi::ResourceState::ShaderResource);
	++mLiveTextures;
	return reinterpret_cast<Rml::TextureHandle>(texture);
}

// Images named by documents (<img src>, decorators): PNG, JPG, TGA, BMP through stb_image,
// premultiplied as RmlUi expects.
Rml::TextureHandle UiRenderer::LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source)
{
	Rml::String file;
	if (!Rml::GetFileInterface()->LoadFile(source, file) || file.empty())
	{
		CHERRY_WARN("UI: can't read image %s", source.c_str());
		return 0;
	}
	int width = 0, height = 0, channels = 0;
	stbi_uc* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(file.data()), static_cast<int>(file.size()), &width, &height, &channels, 4);
	if (!pixels)
	{
		CHERRY_WARN("UI: can't load image %s (%s)", source.c_str(), stbi_failure_reason());
		return 0;
	}
	for (size_t i = 0, n = static_cast<size_t>(width) * height; i < n; ++i)
	{
		stbi_uc* p = pixels + i * 4;
		const unsigned a = p[3];
		p[0] = static_cast<stbi_uc>((p[0] * a + 127) / 255);
		p[1] = static_cast<stbi_uc>((p[1] * a + 127) / 255);
		p[2] = static_cast<stbi_uc>((p[2] * a + 127) / 255);
	}
	const Rml::TextureHandle handle = MakeTexture(pixels, width, height, "UiImage");
	stbi_image_free(pixels);
	if (handle) dimensions = { width, height };
	return handle;
}

// Font glyph atlases and the like, already premultiplied.
Rml::TextureHandle UiRenderer::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions)
{
	if (source.size() < static_cast<size_t>(dimensions.x) * dimensions.y * 4) return 0;
	return MakeTexture(source.data(), dimensions.x, dimensions.y, "UiGenerated");
}

void UiRenderer::ReleaseTexture(Rml::TextureHandle handle)
{
	if (!handle) return;
	delete reinterpret_cast<Texture*>(handle);   // its GPU memory goes once the frames using it are done
	--mLiveTextures;
}

// ---- State ---------------------------------------------------------------------------------

void UiRenderer::EnableScissorRegion(bool enable)
{
	mScissorEnabled = enable;
	ApplyScissor();
}

void UiRenderer::SetScissorRegion(Rml::Rectanglei region)
{
	mScissor = region;
	ApplyScissor();
}

void UiRenderer::SetTransform(const Rml::Matrix4f* transform)
{
	mHasTransform = transform != nullptr;
	if (transform) mTransform = mProjection * *transform;
	mTransformOffset = ~0u;   // pushed to the frame's data when something draws with it
}

// ---- Clip mask -----------------------------------------------------------------------------
// The reference renderer's stencil counting, in a texture: Set draws 1 where the geometry is,
// SetInverse 0 there over 1 everywhere, Intersect adds 1; a pixel is drawn where the count is
// the number required.

void UiRenderer::EnableClipMask(bool enable)
{
	if (enable == mClipEnabled) return;
	mClipEnabled = enable;
	if (enable && mCmd)
	{
		EnsureTargets();
		ResumeTopLayer();   // makes the mask readable
	}
}

void UiRenderer::RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle handle, Rml::Vector2f translation)
{
	if (!mCmd || !handle) return;
	EnsureTargets();
	Geometry& geometry = *reinterpret_cast<Geometry*>(handle);
	switch (operation)
	{
	case Rml::ClipMaskOperation::Set:
		BeginPass(*mClipMask, {}, true, 0.0f);
		DrawGeometry(geometry, "ui_mask_write", CHERRY_INVALID_INDEX, translation, 0, 1.0f, false);
		mClipRequired = 1.0f;
		break;
	case Rml::ClipMaskOperation::SetInverse:
	{
		BeginPass(*mClipMask, {}, true, 0.0f);
		Pass fill;
		fill.pipeline = "ui_mask_fill";
		fill.color[0] = 1.0f;
		SetScissorRect(Rml::Rectanglei::FromSize(Rml::Vector2i(static_cast<int>(mWidth), static_cast<int>(mHeight))));
		DrawPass(fill);
		ApplyScissor();
		DrawGeometry(geometry, "ui_mask_write", CHERRY_INVALID_INDEX, translation, 0, 0.0f, false);
		mClipRequired = 1.0f;
		break;
	}
	case Rml::ClipMaskOperation::Intersect:
		BeginPass(*mClipMask, {});
		DrawGeometry(geometry, "ui_mask_add", CHERRY_INVALID_INDEX, translation, 0, 1.0f, false);
		mClipRequired += 1.0f;
		break;
	}
	ResumeTopLayer();
}

// ---- Layers --------------------------------------------------------------------------------

Rml::LayerHandle UiRenderer::PushLayer()
{
	if (!mCmd) return 0;
	mLayerCount++;
	EnsureTargets();
	BeginPass(*TopLayer(), { mClipEnabled ? mClipMask.get() : nullptr }, true, 0.0f);
	return static_cast<Rml::LayerHandle>(mLayerCount - 1);
}

void UiRenderer::PopLayer()
{
	if (!mCmd || mLayerCount <= 1) return;
	mLayerCount--;
	ResumeTopLayer();
}

void UiRenderer::CopyToPrimary(rhi::Texture& source)
{
	EnsureTargets();
	// Cleared first: the filters sample past the scissor's edge (bilinear), which must read clear,
	// not whatever an earlier effect left there.
	BeginPass(*mPost[0], { &source }, true, 0.0f);
	Pass copy;
	copy.source = &source;
	DrawPass(copy);
}

void UiRenderer::CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blendMode,
	Rml::Span<const Rml::CompiledFilterHandle> filters)
{
	if (!mCmd) return;
	CopyToPrimary(LayerTexture(source));
	ApplyFilters(filters);
	BeginPass(LayerTexture(destination), { mPost[0].get(), mClipEnabled ? mClipMask.get() : nullptr });
	Pass composite;
	composite.pipeline = blendMode == Rml::BlendMode::Replace ? "ui_copy" : "ui_composite";
	composite.source = mPost[0].get();
	composite.clipped = true;
	DrawPass(composite);
	if (destination != static_cast<Rml::LayerHandle>(mLayerCount - 1)) ResumeTopLayer();
}

// The layer's pixels inside the scissor, as a texture of that size (box-shadow keeps one).
Rml::TextureHandle UiRenderer::SaveLayerAsTexture()
{
	if (!mCmd) return 0;
	const Rml::Rectanglei bounds = ScissorRect();
	if (bounds.Width() <= 0 || bounds.Height() <= 0) return 0;
	rhi::TextureDesc desc;
	desc.width = static_cast<uint32_t>(bounds.Width());
	desc.height = static_cast<uint32_t>(bounds.Height());
	desc.format = mFormat;
	desc.usage = rhi::TextureUsage_RenderTarget | rhi::TextureUsage_ShaderResource;
	desc.clearColor[3] = 0.0f;
	desc.initialState = rhi::ResourceState::ShaderResource;
	desc.name = "UiSavedLayer";
	auto* texture = new Texture{ mDevice->CreateTexture(desc) };
	if (!texture->texture)
	{
		delete texture;
		return 0;
	}
	rhi::Texture* layer = TopLayer();
	BeginPass(*texture->texture, { layer });
	SetScissorRect(Rml::Rectanglei::FromSize(bounds.Size()));
	Pass copy;
	copy.source = layer;
	copy.uvScale[0] = bounds.Width() / static_cast<float>(mWidth);
	copy.uvScale[1] = bounds.Height() / static_cast<float>(mHeight);
	copy.uvOffset[0] = bounds.Left() / static_cast<float>(mWidth);
	copy.uvOffset[1] = bounds.Top() / static_cast<float>(mHeight);
	DrawPass(copy);
	EndPass();
	texture->texture->Transition(*mCmd, rhi::ResourceState::ShaderResource);
	ResumeTopLayer();
	++mLiveTextures;
	return reinterpret_cast<Rml::TextureHandle>(texture);
}

Rml::CompiledFilterHandle UiRenderer::SaveLayerAsMaskImage()
{
	if (!mCmd) return 0;
	EnsureTargets();
	rhi::Texture* layer = TopLayer();
	BeginPass(*mBlendMask, { layer });
	Pass copy;
	copy.source = layer;
	DrawPass(copy);
	ResumeTopLayer();
	auto* filter = new Filter();
	filter->type = FilterType::MaskImage;
	return reinterpret_cast<Rml::CompiledFilterHandle>(filter);
}

// ---- Filters -------------------------------------------------------------------------------

Rml::CompiledFilterHandle UiRenderer::CompileFilter(const Rml::String& name, const Rml::Dictionary& parameters)
{
	Filter filter;
	auto matrixFromRows = [](const Rml::Vector4f& r0, const Rml::Vector4f& r1, const Rml::Vector4f& r2)
	{
		return Rml::Matrix4f::FromRows(r0, r1, r2, Rml::Vector4f(0, 0, 0, 1));
	};
	if (name == "opacity")
	{
		filter.type = FilterType::Opacity;
		filter.value = Rml::Get(parameters, "value", 1.0f);
	}
	else if (name == "blur")
	{
		filter.type = FilterType::Blur;
		filter.sigma = Rml::Get(parameters, "sigma", 1.0f);
	}
	else if (name == "drop-shadow")
	{
		filter.type = FilterType::DropShadow;
		filter.sigma = Rml::Get(parameters, "sigma", 0.0f);
		filter.color = ToFloat(Rml::Get(parameters, "color", Rml::Colourb()).ToPremultiplied());
		filter.offset = Rml::Get(parameters, "offset", Rml::Vector2f(0.0f));
	}
	else if (name == "brightness")
	{
		filter.type = FilterType::ColorMatrix;
		const float value = Rml::Get(parameters, "value", 1.0f);
		filter.matrix = Rml::Matrix4f::Diag(value, value, value, 1.0f);
	}
	else if (name == "contrast")
	{
		filter.type = FilterType::ColorMatrix;
		const float value = Rml::Get(parameters, "value", 1.0f);
		const float grayness = 0.5f - 0.5f * value;
		filter.matrix = Rml::Matrix4f::Diag(value, value, value, 1.0f);
		filter.matrix.SetColumn(3, Rml::Vector4f(grayness, grayness, grayness, 1.0f));
	}
	else if (name == "invert")
	{
		filter.type = FilterType::ColorMatrix;
		const float value = std::clamp(Rml::Get(parameters, "value", 1.0f), 0.0f, 1.0f);
		const float inverted = 1.0f - 2.0f * value;
		filter.matrix = Rml::Matrix4f::Diag(inverted, inverted, inverted, 1.0f);
		filter.matrix.SetColumn(3, Rml::Vector4f(value, value, value, 1.0f));
	}
	else if (name == "grayscale")
	{
		filter.type = FilterType::ColorMatrix;
		const float value = Rml::Get(parameters, "value", 1.0f), rest = 1.0f - value;
		const Rml::Vector3f gray = value * Rml::Vector3f(0.2126f, 0.7152f, 0.0722f);
		filter.matrix = matrixFromRows({ gray.x + rest, gray.y, gray.z, 0 }, { gray.x, gray.y + rest, gray.z, 0 }, { gray.x, gray.y, gray.z + rest, 0 });
	}
	else if (name == "sepia")
	{
		filter.type = FilterType::ColorMatrix;
		const float value = Rml::Get(parameters, "value", 1.0f), rest = 1.0f - value;
		const Rml::Vector3f r = value * Rml::Vector3f(0.393f, 0.769f, 0.189f);
		const Rml::Vector3f g = value * Rml::Vector3f(0.349f, 0.686f, 0.168f);
		const Rml::Vector3f b = value * Rml::Vector3f(0.272f, 0.534f, 0.131f);
		filter.matrix = matrixFromRows({ r.x + rest, r.y, r.z, 0 }, { g.x, g.y + rest, g.z, 0 }, { b.x, b.y, b.z + rest, 0 });
	}
	else if (name == "hue-rotate")
	{
		// https://www.w3.org/TR/filter-effects-1/#attr-valuedef-type-huerotate
		filter.type = FilterType::ColorMatrix;
		const float value = Rml::Get(parameters, "value", 1.0f);
		const float s = std::sin(value), c = std::cos(value);
		filter.matrix = matrixFromRows(
			{ 0.213f + 0.787f * c - 0.213f * s, 0.715f - 0.715f * c - 0.715f * s, 0.072f - 0.072f * c + 0.928f * s, 0 },
			{ 0.213f - 0.213f * c + 0.143f * s, 0.715f + 0.285f * c + 0.140f * s, 0.072f - 0.072f * c - 0.283f * s, 0 },
			{ 0.213f - 0.213f * c - 0.787f * s, 0.715f - 0.715f * c + 0.715f * s, 0.072f + 0.928f * c + 0.072f * s, 0 });
	}
	else if (name == "saturate")
	{
		filter.type = FilterType::ColorMatrix;
		const float v = Rml::Get(parameters, "value", 1.0f);
		filter.matrix = matrixFromRows(
			{ 0.213f + 0.787f * v, 0.715f - 0.715f * v, 0.072f - 0.072f * v, 0 },
			{ 0.213f - 0.213f * v, 0.715f + 0.285f * v, 0.072f - 0.072f * v, 0 },
			{ 0.213f - 0.213f * v, 0.715f - 0.715f * v, 0.072f + 0.928f * v, 0 });
	}
	else
	{
		Rml::Log::Message(Rml::Log::LT_WARNING, "Unsupported filter '%s'.", name.c_str());
		return 0;
	}
	return reinterpret_cast<Rml::CompiledFilterHandle>(new Filter(filter));
}

void UiRenderer::ReleaseFilter(Rml::CompiledFilterHandle filter)
{
	delete reinterpret_cast<Filter*>(filter);
}

// A Gaussian blur of the scissor region `window` of `a` (`b` is scratch), as the reference
// renderer does it but with a chain of half-size textures: halve the image until sigma is at
// most kMaxPassSigma, blur it vertically and horizontally at that size, then draw it back at
// full size. Each level covers the whole view at its own resolution, so every copy maps uv to uv.
void UiRenderer::Blur(float sigma, rhi::Texture& a, rhi::Texture& b, const Rml::Rectanglei& window)
{
	const int passLevel = std::clamp(Rml::Math::Log2(static_cast<int>(sigma * (2.0f / kMaxPassSigma))), 0, 10);
	sigma = std::clamp(sigma / static_cast<float>(1 << passLevel), 0.0f, kMaxPassSigma);

	// Down: each level from the one above, the region halved (rounded outwards).
	rhi::Texture* image = &a;
	rhi::Texture* temp = &b;
	Rml::Rectanglei region = window;
	for (int level = 1; level <= passLevel; ++level)
	{
		BlurLevel& next = BlurLevelAt(level);
		region.p0 = region.p0 / 2;
		region.p1 = Rml::Math::Max((region.p1 + Rml::Vector2i(1)) / 2, region.p0);
		BeginPass(*next.image, { image }, true, 0.0f);
		SetScissorRect(region);
		Pass copy;
		copy.source = image;
		DrawPass(copy);
		image = next.image.get();
		temp = next.temp.get();
	}

	// Weights (centre first) and the region, half a texel in, outside which the image is clear.
	const float w = static_cast<float>(image->Width()), h = static_cast<float>(image->Height());
	float params[16] = {};
	float total = 0.0f;
	for (int i = 0; i < kBlurWeights; ++i)
	{
		params[i] = sigma < 0.1f ? (i == 0 ? 1.0f : 0.0f) : std::exp(-float(i * i) / (2.0f * sigma * sigma));
		total += (i == 0 ? 1.0f : 2.0f) * params[i];
	}
	for (int i = 0; i < kBlurWeights; ++i) params[i] /= total;
	params[12] = (region.Left() + 0.5f) / w;
	params[13] = (region.Top() + 0.5f) / h;
	params[14] = (region.Right() - 0.5f) / w;
	params[15] = (region.Bottom() - 0.5f) / h;
	const uint32_t offset = PushData(params, 4);
	if (offset != ~0u)
	{
		Pass blur;
		blur.pipeline = "ui_blur";
		blur.paramOffset = offset;
		// Vertical: image -> temp.
		BeginPass(*temp, { image }, true, 0.0f);
		SetScissorRect(region);
		blur.source = image;
		blur.uvOffset[0] = 0.0f;
		blur.uvOffset[1] = 1.0f / h;
		DrawPass(blur);
		// Horizontal: temp -> image. A half-size image is cleared first, so that drawing it back
		// at full size fades to clear past the region's edges.
		BeginPass(*image, { temp }, passLevel > 0, 0.0f);
		SetScissorRect(region);
		blur.source = temp;
		blur.uvOffset[0] = 1.0f / w;
		blur.uvOffset[1] = 0.0f;
		DrawPass(blur);
	}

	// Up: back into `a` at full size (bilinear).
	if (image != &a)
	{
		BeginPass(a, { image });
		SetScissorRect(window);
		Pass upscale;
		upscale.source = image;
		DrawPass(upscale);
	}
	ApplyScissor();
}

UiRenderer::BlurLevel& UiRenderer::BlurLevelAt(int level)
{
	if (mBlurLevels.size() < static_cast<size_t>(level)) mBlurLevels.resize(level);
	BlurLevel& entry = mBlurLevels[level - 1];
	if (!entry.image)
	{
		rhi::TextureDesc desc;
		desc.width = std::max(1u, mTargetWidth >> level);
		desc.height = std::max(1u, mTargetHeight >> level);
		desc.format = mFormat;
		desc.usage = rhi::TextureUsage_RenderTarget | rhi::TextureUsage_ShaderResource;
		desc.clearColor[3] = 0.0f;
		desc.initialState = rhi::ResourceState::ShaderResource;
		desc.name = "UiBlurLevel";
		entry.image = mDevice->CreateTexture(desc);
		entry.temp = mDevice->CreateTexture(desc);
	}
	return entry;
}

// The filters, in order, on the post-processing primary target (the result ends up there).
void UiRenderer::ApplyFilters(Rml::Span<const Rml::CompiledFilterHandle> filters)
{
	for (Rml::CompiledFilterHandle handle : filters)
	{
		if (!handle) continue;
		const Filter& filter = *reinterpret_cast<const Filter*>(handle);
		const Rml::Rectanglei window = ScissorRect();
		switch (filter.type)
		{
		case FilterType::Opacity:
		{
			BeginPass(*mPost[1], { mPost[0].get() }, true, 0.0f);
			Pass fade;
			fade.source = mPost[0].get();
			std::fill(std::begin(fade.color), std::end(fade.color), filter.value);
			DrawPass(fade);
			SwapPostprocess();
			break;
		}
		case FilterType::Blur:
			Blur(filter.sigma * mScale, *mPost[0], *mPost[1], window);
			break;
		case FilterType::DropShadow:
		{
			const float w = static_cast<float>(mWidth), h = static_cast<float>(mHeight);
			const float limits[4] = { (window.Left() + 0.5f) / w, (window.Top() + 0.5f) / h, (window.Right() - 0.5f) / w, (window.Bottom() - 0.5f) / h };
			const uint32_t offset = PushData(limits, 1);
			if (offset == ~0u) break;
			BeginPass(*mPost[1], { mPost[0].get() }, true, 0.0f);
			Pass shadow;
			shadow.pipeline = "ui_drop_shadow";
			shadow.source = mPost[0].get();
			shadow.paramOffset = offset;
			shadow.color[0] = filter.color.red;
			shadow.color[1] = filter.color.green;
			shadow.color[2] = filter.color.blue;
			shadow.color[3] = filter.color.alpha;
			shadow.uvOffset[0] = -filter.offset.x * mScale / w;
			shadow.uvOffset[1] = -filter.offset.y * mScale / h;
			DrawPass(shadow);
			if (filter.sigma >= 0.5f) Blur(filter.sigma * mScale, *mPost[1], *mPost[2], window);
			// The element over its shadow.
			BeginPass(*mPost[1], { mPost[0].get() });
			Pass over;
			over.pipeline = "ui_composite";
			over.source = mPost[0].get();
			DrawPass(over);
			SwapPostprocess();
			break;
		}
		case FilterType::ColorMatrix:
		{
			const uint32_t offset = PushData(filter.matrix.data(), 4);   // its four columns
			if (offset == ~0u) break;
			BeginPass(*mPost[1], { mPost[0].get() }, true, 0.0f);
			Pass matrix;
			matrix.pipeline = "ui_color_matrix";
			matrix.source = mPost[0].get();
			matrix.paramOffset = offset;
			DrawPass(matrix);
			SwapPostprocess();
			break;
		}
		case FilterType::MaskImage:
		{
			BeginPass(*mPost[1], { mPost[0].get(), mBlendMask.get() }, true, 0.0f);
			Pass mask;
			mask.pipeline = "ui_blend_mask";
			mask.source = mPost[0].get();
			mask.paramOffset = mBlendMask->SrvIndex();
			DrawPass(mask);
			SwapPostprocess();
			break;
		}
		}
	}
}

// ---- Shaders: gradients --------------------------------------------------------------------

Rml::CompiledShaderHandle UiRenderer::CompileShader(const Rml::String& name, const Rml::Dictionary& parameters)
{
	GradientFunction function;
	Rml::Vector2f p, v;
	const bool repeating = Rml::Get(parameters, "repeating", false);
	if (name == "linear-gradient")
	{
		function = repeating ? GradientFunction::RepeatingLinear : GradientFunction::Linear;
		p = Rml::Get(parameters, "p0", Rml::Vector2f(0.0f));
		v = Rml::Get(parameters, "p1", Rml::Vector2f(0.0f)) - p;
	}
	else if (name == "radial-gradient")
	{
		function = repeating ? GradientFunction::RepeatingRadial : GradientFunction::Radial;
		p = Rml::Get(parameters, "center", Rml::Vector2f(0.0f));
		v = Rml::Vector2f(1.0f) / Rml::Get(parameters, "radius", Rml::Vector2f(1.0f));
	}
	else if (name == "conic-gradient")
	{
		function = repeating ? GradientFunction::RepeatingConic : GradientFunction::Conic;
		p = Rml::Get(parameters, "center", Rml::Vector2f(0.0f));
		const float angle = Rml::Get(parameters, "angle", 0.0f);
		v = { std::cos(angle), std::sin(angle) };
	}
	else
	{
		Rml::Log::Message(Rml::Log::LT_WARNING, "Unsupported shader '%s'.", name.c_str());
		return 0;
	}
	auto stopsIt = parameters.find("color_stop_list");
	if (stopsIt == parameters.end() || stopsIt->second.GetType() != Rml::Variant::COLORSTOPLIST) return 0;
	const Rml::ColorStopList& stops = stopsIt->second.GetReference<Rml::ColorStopList>();
	const int count = std::min(static_cast<int>(stops.size()), kMaxGradientStops);

	auto* shader = new Shader();
	float* d = shader->data.data();
	d[0] = static_cast<float>(function);
	d[1] = static_cast<float>(count);
	d[4] = p.x;
	d[5] = p.y;
	d[6] = v.x;
	d[7] = v.y;
	for (int i = 0; i < count; ++i)
	{
		d[8 + i] = stops[i].position.number;   // 4 float4s of positions
		const Rml::Colourf color = ToFloat(stops[i].color);
		float* c = d + 4 * (6 + i);
		c[0] = color.red;
		c[1] = color.green;
		c[2] = color.blue;
		c[3] = color.alpha;
	}
	return reinterpret_cast<Rml::CompiledShaderHandle>(shader);
}

void UiRenderer::RenderShader(Rml::CompiledShaderHandle shaderHandle, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
	Rml::TextureHandle)
{
	if (!mCmd || !shaderHandle || !geometry) return;
	const Shader& shader = *reinterpret_cast<const Shader*>(shaderHandle);
	const uint32_t offset = PushData(shader.data.data(), static_cast<uint32_t>(shader.data.size() / 4));
	DrawGeometry(*reinterpret_cast<Geometry*>(geometry), "ui_gradient", CHERRY_INVALID_INDEX, translation, offset, 0.0f, true);
}

void UiRenderer::ReleaseShader(Rml::CompiledShaderHandle shader)
{
	delete reinterpret_cast<Shader*>(shader);
}
