#include "Renderer/ImGuiRenderer.h"
#include "RHI/Pipeline.h"
#include "imgui.h"
#include <algorithm>
#include <vector>

namespace
{
	struct ImGuiConstants
	{
		float scale[2];
		float translate[2];
		uint32_t textureIndex;
	};
}

void ImGuiRenderer::Initialize(rhi::Device& device, rhi::PipelineLibrary& pipelines)
{
	mDevice = &device;
	mPipelines = &pipelines;
	rhi::GraphicsPipelineDesc desc;
	desc.shaderFile = "ImGui.hlsl";
	desc.vertexLayout = rhi::VertexLayout::ImGui;
	desc.colorFormats = { device.SwapChainFormat() };
	desc.depthTest = false;
	desc.depthWrite = false;
	desc.cullMode = rhi::CullMode::None;
	desc.alphaBlend = true;
	pipelines.AddGraphics("imgui", desc);

	ImGuiIO& io = ImGui::GetIO();
	io.BackendRendererName = "CherryEngine RHI";
	io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
}

void ImGuiRenderer::Shutdown()
{
	if (ImGui::GetCurrentContext())
	{
		for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
		{
			if (auto* owned = static_cast<rhi::Texture*>(texture->BackendUserData))
			{
				delete owned;   // its GPU memory goes once the frames using it are done
				texture->BackendUserData = nullptr;
				texture->SetTexID(ImTextureID_Invalid);
				texture->SetStatus(ImTextureStatus_Destroyed);
			}
		}
	}
	for (auto& buffer : mVertices) buffer.reset();
	for (auto& buffer : mIndices) buffer.reset();
}

void ImGuiRenderer::UpdateTextures(ImDrawData* data)
{
	if (!data->Textures) return;
	for (ImTextureData* texture : *data->Textures)
	{
		if (texture->Status == ImTextureStatus_OK) continue;
		if (texture->Status == ImTextureStatus_WantCreate)
		{
			rhi::TextureDesc desc;
			desc.width = static_cast<uint32_t>(texture->Width);
			desc.height = static_cast<uint32_t>(texture->Height);
			desc.format = rhi::Format::RGBA8Unorm;
			desc.name = "ImGuiTexture";
			std::unique_ptr<rhi::Texture> created = mDevice->CreateTexture(desc);
			const uint32_t pitch = static_cast<uint32_t>(texture->GetPitch());
			rhi::SubresourceData pixels{ texture->GetPixels(), pitch, pitch * static_cast<uint32_t>(texture->Height) };
			created->Upload(&pixels, 1, rhi::ResourceState::ShaderResource);
			texture->SetTexID(static_cast<ImTextureID>(TextureId(created->SrvIndex())));
			texture->BackendUserData = created.release();
			texture->SetStatus(ImTextureStatus_OK);
		}
		else if (texture->Status == ImTextureStatus_WantUpdates)
		{
			if (auto* owned = static_cast<rhi::Texture*>(texture->BackendUserData))
			{
				const ImTextureRect& r = texture->UpdateRect;
				std::vector<uint8_t> packed(static_cast<size_t>(r.w) * r.h * 4);
				for (int y = 0; y < r.h; ++y)
					std::memcpy(packed.data() + static_cast<size_t>(y) * r.w * 4, texture->GetPixelsAt(r.x, r.y + y), static_cast<size_t>(r.w) * 4);
				owned->UploadRegion(r.x, r.y, 0, r.w, r.h, 1, packed.data(), 4);
			}
			texture->SetStatus(ImTextureStatus_OK);
		}
		else if (texture->Status == ImTextureStatus_WantDestroy && texture->UnusedFrames >= static_cast<int>(rhi::kFramesInFlight))
		{
			delete static_cast<rhi::Texture*>(texture->BackendUserData);
			texture->BackendUserData = nullptr;
			texture->SetTexID(ImTextureID_Invalid);
			texture->SetStatus(ImTextureStatus_Destroyed);
		}
	}
}

void ImGuiRenderer::Render(ImDrawData* data, rhi::CommandList& cmd)
{
	if (!data) return;
	UpdateTextures(data);
	if (data->TotalVtxCount == 0 || data->DisplaySize.x <= 0.0f || data->DisplaySize.y <= 0.0f) return;
	rhi::Pipeline* pipeline = mPipelines->Get("imgui");
	if (!pipeline) return;

	// This frame's vertex and index buffers (upload memory), grown when they're too small.
	const uint32_t frame = mDevice->FrameIndex();
	const uint64_t vertexBytes = static_cast<uint64_t>(data->TotalVtxCount) * sizeof(ImDrawVert);
	const uint64_t indexBytes = static_cast<uint64_t>(data->TotalIdxCount) * sizeof(ImDrawIdx);
	auto ensure = [&](std::unique_ptr<rhi::Buffer>& buffer, uint64_t bytes, const char* name)
	{
		if (buffer && buffer->Size() >= bytes) return;
		rhi::BufferDesc desc;
		desc.size = std::max<uint64_t>(bytes * 2, 64 * 1024);
		desc.heap = rhi::HeapKind::Upload;
		desc.name = name;
		buffer = mDevice->CreateBuffer(desc);   // the old one is released once unused
	};
	ensure(mVertices[frame], vertexBytes, "ImGuiVertices");
	ensure(mIndices[frame], indexBytes, "ImGuiIndices");
	auto* vertices = static_cast<ImDrawVert*>(mVertices[frame]->Mapped());
	auto* indices = static_cast<ImDrawIdx*>(mIndices[frame]->Mapped());
	for (const ImDrawList* list : data->CmdLists)
	{
		std::memcpy(vertices, list->VtxBuffer.Data, list->VtxBuffer.Size * sizeof(ImDrawVert));
		std::memcpy(indices, list->IdxBuffer.Data, list->IdxBuffer.Size * sizeof(ImDrawIdx));
		vertices += list->VtxBuffer.Size;
		indices += list->IdxBuffer.Size;
	}

	const float width = static_cast<float>(mDevice->SwapChainWidth()), height = static_cast<float>(mDevice->SwapChainHeight());
	ImGuiConstants constants{};
	constants.scale[0] = 2.0f / data->DisplaySize.x;
	constants.scale[1] = -2.0f / data->DisplaySize.y;
	constants.translate[0] = -1.0f - data->DisplayPos.x * constants.scale[0];
	constants.translate[1] = 1.0f - data->DisplayPos.y * constants.scale[1];
	auto setup = [&]()
	{
		cmd.SetPipeline(pipeline);
		cmd.SetVertexBuffer(*mVertices[frame], sizeof(ImDrawVert));
		cmd.SetIndexBuffer(*mIndices[frame], sizeof(ImDrawIdx) == 2);
		cmd.SetViewport(0.0f, 0.0f, width, height);
	};
	setup();

	// ImGui's coordinates are window pixels; the swap chain may differ in size (scale to it).
	const float scaleX = width / data->DisplaySize.x, scaleY = height / data->DisplaySize.y;
	int32_t vertexOffset = 0;
	uint32_t indexOffset = 0;
	for (const ImDrawList* list : data->CmdLists)
	{
		for (const ImDrawCmd& draw : list->CmdBuffer)
		{
			if (draw.UserCallback)
			{
				if (draw.UserCallback == ImDrawCallback_ResetRenderState) setup();
				else draw.UserCallback(list, &draw);
				continue;
			}
			const int32_t x0 = std::max(0, static_cast<int32_t>((draw.ClipRect.x - data->DisplayPos.x) * scaleX));
			const int32_t y0 = std::max(0, static_cast<int32_t>((draw.ClipRect.y - data->DisplayPos.y) * scaleY));
			const int32_t x1 = std::min(static_cast<int32_t>(width), static_cast<int32_t>((draw.ClipRect.z - data->DisplayPos.x) * scaleX));
			const int32_t y1 = std::min(static_cast<int32_t>(height), static_cast<int32_t>((draw.ClipRect.w - data->DisplayPos.y) * scaleY));
			if (x1 <= x0 || y1 <= y0) continue;
			const ImTextureID id = draw.GetTexID();
			if (id == ImTextureID_Invalid) continue;
			cmd.SetScissor(x0, y0, x1, y1);
			constants.textureIndex = static_cast<uint32_t>(id - 1);
			cmd.SetConstants(constants);
			cmd.DrawIndexed(draw.ElemCount, 1, indexOffset + draw.IdxOffset, vertexOffset + static_cast<int32_t>(draw.VtxOffset), 0);
		}
		vertexOffset += list->VtxBuffer.Size;
		indexOffset += list->IdxBuffer.Size;
	}
}
