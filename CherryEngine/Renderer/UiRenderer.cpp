#include "Renderer/UiRenderer.h"
#include "RHI/Pipeline.h"
#include "Core/Log.h"
#include "Shaders/Shared/ShaderInterop.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Matrix4.h>
#include <stb_image.h>
#include <algorithm>
#include <chrono>
#include <cstring>

namespace
{
	constexpr uint32_t kVertexBytes = sizeof(Rml::Vertex);   // 20
	static_assert(sizeof(Rml::Vertex) == 20, "VertexLayout::RmlUi expects 20-byte vertices");
	constexpr uint64_t kInitialStreamBytes = 256 * 1024;
	constexpr uint32_t kInitialTransforms = 256;
	constexpr uint64_t AlignUp(uint64_t value, uint64_t alignment) { return (value + alignment - 1) / alignment * alignment; }
}

void UiRenderer::Initialize(rhi::Device& device, rhi::PipelineLibrary& pipelines, rhi::Format targetFormat)
{
	mDevice = &device;
	mPipelines = &pipelines;
	rhi::GraphicsPipelineDesc desc;
	desc.shaderFile = "Ui.hlsl";
	desc.vertexLayout = rhi::VertexLayout::RmlUi;
	desc.colorFormats = { targetFormat };
	desc.depthTest = false;
	desc.depthWrite = false;
	desc.cullMode = rhi::CullMode::None;
	desc.premultipliedBlend = true;
	pipelines.AddGraphics("ui", desc);
}

void UiRenderer::Shutdown()
{
	for (FrameBuffers& frame : mFrames)
	{
		frame.stream.reset();
		frame.transforms.reset();
	}
	if (mLiveGeometries || mLiveTextures)
		CHERRY_WARN("UI: %u geometries and %u textures were never released", mLiveGeometries, mLiveTextures);
}

void UiRenderer::Render(rhi::CommandList& cmd, rhi::Texture& target, Rml::Context& context)
{
	rhi::Pipeline* pipeline = mPipelines->Get("ui");
	if (!pipeline) return;
	const auto start = std::chrono::steady_clock::now();

	mCmd = &cmd;
	mFrame = &mFrames[mDevice->FrameIndex()];
	mStreamGeneration++;   // last frame's copies are in another frame slot's buffer
	mStreamUsed = 0;
	mBoundGeneration = ~0ull;
	mWidth = static_cast<float>(target.Width());
	mHeight = static_cast<float>(target.Height());
	mScissorEnabled = false;
	mCounting = {};

	// Pixels (y down) to clip space.
	mProjection = Rml::Matrix4f::ProjectOrtho(0.0f, mWidth, mHeight, 0.0f, -10000.0f, 10000.0f);
	StartTransforms(mFrame->transforms ? static_cast<uint32_t>(mFrame->transforms->Size() / 64) : kInitialTransforms);
	mTransformIndex = 0;

	target.Transition(cmd, rhi::ResourceState::RenderTarget);
	rhi::RenderPassDesc pass;
	pass.color = &target;
	cmd.BeginRendering(pass);
	cmd.BeginEvent("UI");
	cmd.SetPipeline(pipeline);
	cmd.SetViewport(0.0f, 0.0f, mWidth, mHeight);
	ApplyScissor();
	context.Render();
	cmd.EndEvent();
	cmd.EndRendering();
	target.Transition(cmd, rhi::ResourceState::ShaderResource);

	mCmd = nullptr;
	mFrame = nullptr;
	mCounting.transforms = mTransformCount;
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

void UiRenderer::BindStream()
{
	if (mBoundGeneration == mStreamGeneration) return;
	mBoundGeneration = mStreamGeneration;
	mCmd->SetVertexBuffer(*mFrame->stream, kVertexBytes);
	mCmd->SetIndexBuffer(*mFrame->stream, false);
}

// Starts the frame's transforms in a buffer of at least `capacity` (a new one if it's bigger
// than the one there, which then stays alive until the GPU is done with this frame).
void UiRenderer::StartTransforms(uint32_t capacity)
{
	if (!mFrame->transforms || mFrame->transforms->Size() / 64 < capacity)
	{
		rhi::BufferDesc desc;
		desc.size = static_cast<uint64_t>(capacity) * 64;
		desc.stride = 64;
		desc.heap = rhi::HeapKind::Upload;
		desc.createSrv = true;
		desc.name = "UiTransforms";
		mFrame->transforms = mDevice->CreateBuffer(desc);
	}
	std::memcpy(mFrame->transforms->Mapped(), mProjection.data(), 64);
	mTransformCount = 1;
}

uint32_t UiRenderer::PushTransform(const Rml::Matrix4f& transform)
{
	if (mTransformCount >= mFrame->transforms->Size() / 64) StartTransforms(static_cast<uint32_t>(mFrame->transforms->Size() / 64) * 2);
	std::memcpy(static_cast<uint8_t*>(mFrame->transforms->Mapped()) + static_cast<size_t>(mTransformCount) * 64, transform.data(), 64);
	return mTransformCount++;
}

// ---- Geometry ------------------------------------------------------------------------------

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
	if (!mCmd || !handle) return;
	Geometry& geometry = *reinterpret_cast<Geometry*>(handle);
	if (geometry.indices.empty()) return;

	// Copy it into this frame's stream once; drawn again (or the stream replaced since), it's
	// copied again.
	if (geometry.copiedStream != mStreamGeneration)
	{
		const uint64_t vertexBytes = geometry.vertices.size() * kVertexBytes;
		const uint64_t indexBytes = geometry.indices.size() * sizeof(int);
		uint64_t vertexOffset = 0, indexOffset = 0;
		if (!Reserve(vertexBytes, indexBytes, vertexOffset, indexOffset)) return;
		auto* base = static_cast<uint8_t*>(mFrame->stream->Mapped());
		std::memcpy(base + vertexOffset, geometry.vertices.data(), vertexBytes);
		std::memcpy(base + indexOffset, geometry.indices.data(), indexBytes);
		geometry.copiedStream = mStreamGeneration;
		geometry.baseVertex = static_cast<uint32_t>(vertexOffset / kVertexBytes);
		geometry.firstIndex = static_cast<uint32_t>(indexOffset / sizeof(int));
		mCounting.vertices += static_cast<uint32_t>(geometry.vertices.size());
	}
	BindStream();

	gpu::UiConstants constants{};
	constants.translation = { translation.x, translation.y };
	const Texture* texture = reinterpret_cast<const Texture*>(textureHandle);
	constants.textureIndex = texture && texture->texture ? texture->texture->SrvIndex() : CHERRY_INVALID_INDEX;
	constants.transformIndex = mTransformIndex;
	constants.transformBufferIndex = mFrame->transforms->SrvIndex();
	mCmd->SetConstants(constants);
	mCmd->DrawIndexed(static_cast<uint32_t>(geometry.indices.size()), 1, geometry.firstIndex, static_cast<int32_t>(geometry.baseVertex), 0);
	mCounting.draws++;
	mCounting.indices += static_cast<uint32_t>(geometry.indices.size());
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

void UiRenderer::ApplyScissor()
{
	if (!mCmd) return;
	const int32_t width = static_cast<int32_t>(mWidth), height = static_cast<int32_t>(mHeight);
	if (!mScissorEnabled)
	{
		mCmd->SetScissor(0, 0, width, height);
		return;
	}
	const int32_t left = std::clamp(mScissor.Left(), 0, width), top = std::clamp(mScissor.Top(), 0, height);
	const int32_t right = std::clamp(mScissor.Right(), left, width), bottom = std::clamp(mScissor.Bottom(), top, height);
	mCmd->SetScissor(left, top, right, bottom);
}

void UiRenderer::SetTransform(const Rml::Matrix4f* transform)
{
	if (!mFrame) return;
	if (!transform)
	{
		mTransformIndex = 0;   // the plain projection, pushed first each frame
		return;
	}
	mTransformIndex = PushTransform(mProjection * *transform);
}
