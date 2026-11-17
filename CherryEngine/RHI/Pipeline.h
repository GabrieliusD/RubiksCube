#pragma once
#include "RHI/D3DCommon.h"
#include "ShaderSystem/ShaderCompiler.h"
#include <filesystem>
#include <unordered_map>
#include <vector>
#include <memory>

namespace rhi
{
	class Device;

	// Root parameter slots of the single, global bindless root signature.
	// Every graphics and compute pipeline uses this layout:
	//   b0 : 16 root constants (DrawConstants: per-draw / per-dispatch indices)
	//   b1 : root CBV, ViewConstants
	//   b2 : root CBV, FrameConstants
	//   s0..s5 : static samplers (point/linear/aniso x wrap/clamp)
	// Everything else is fetched through ResourceDescriptorHeap[] (SM 6.6).
	enum RootSlot : uint32_t
	{
		RootSlot_DrawConstants = 0,
		RootSlot_ViewConstants = 1,
		RootSlot_FrameConstants = 2,
	};
	constexpr uint32_t kDrawConstantCount = 16;

	ComPtr<ID3D12RootSignature> CreateBindlessRootSignature(ID3D12Device* device);

	enum class VertexLayout { None, Mesh };

	struct GraphicsPipelineDesc
	{
		std::string shaderFile;            // relative to the shader root
		std::string vsEntry = "VSMain";
		std::string psEntry = "PSMain";
		std::vector<std::string> defines;
		VertexLayout vertexLayout = VertexLayout::Mesh;
		D3D12_PRIMITIVE_TOPOLOGY_TYPE topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		std::vector<DXGI_FORMAT> rtvFormats;
		DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
		bool depthTest = true;
		bool depthWrite = true;
		D3D12_COMPARISON_FUNC depthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		D3D12_CULL_MODE cullMode = D3D12_CULL_MODE_BACK;
		bool alphaBlend = false;
	};

	struct ComputePipelineDesc
	{
		std::string shaderFile;
		std::string entry = "CSMain";
		std::vector<std::string> defines;
	};

	// Named PSO cache with shader hot reload. Pipelines are looked up by name;
	// when any file under the shader root changes, every pipeline is rebuilt and
	// pipelines whose shaders fail to compile keep their previous PSO.
	class PipelineLibrary
	{
	public:
		void Initialize(Device& device, ID3D12RootSignature* rootSignature, const std::filesystem::path& shaderRoot);
		void Shutdown();

		// Compiles immediately. Throws if the very first compile fails.
		void AddGraphics(const std::string& name, const GraphicsPipelineDesc& desc);
		void AddCompute(const std::string& name, const ComputePipelineDesc& desc);
		ID3D12PipelineState* Get(const std::string& name) const;

		// Polls shader timestamps (cheap; rate-limited). Returns true if pipelines were rebuilt.
		bool PollHotReload();
		void ReloadAll();

		const std::filesystem::path& ShaderRoot() const { return mShaderRoot; }
		const std::string& LastError() const { return mLastError; }
		uint32_t ReloadCount() const { return mReloadCount; }

	private:
		struct Entry
		{
			bool compute = false;
			GraphicsPipelineDesc graphics;
			ComputePipelineDesc computeDesc;
			ComPtr<ID3D12PipelineState> pso;
		};

		bool Build(const std::string& name, Entry& entry);
		uint64_t ShaderTreeSignature() const;

		Device* mDevice = nullptr;
		ID3D12RootSignature* mRootSignature = nullptr;
		ShaderCompiler mCompiler;
		std::filesystem::path mShaderRoot;
		std::unordered_map<std::string, Entry> mEntries;
		uint64_t mShaderSignature = 0;
		uint64_t mLastPollTick = 0;
		std::string mLastError;
		uint32_t mReloadCount = 0;
	};
}
