#include "RHI/Pipeline.h"
#include "RHI/Device.h"
#include "Core/Log.h"
#include <windows.h>

namespace rhi
{
	ComPtr<ID3D12RootSignature> CreateBindlessRootSignature(ID3D12Device* device)
	{
		D3D12_ROOT_PARAMETER1 params[3]{};

		params[RootSlot_DrawConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[RootSlot_DrawConstants].Constants.ShaderRegister = 0;
		params[RootSlot_DrawConstants].Constants.Num32BitValues = kDrawConstantCount;
		params[RootSlot_DrawConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		params[RootSlot_ViewConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		params[RootSlot_ViewConstants].Descriptor.ShaderRegister = 1;
		params[RootSlot_ViewConstants].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
		params[RootSlot_ViewConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		params[RootSlot_FrameConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		params[RootSlot_FrameConstants].Descriptor.ShaderRegister = 2;
		params[RootSlot_FrameConstants].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
		params[RootSlot_FrameConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		auto sampler = [](UINT reg, D3D12_FILTER filter, D3D12_TEXTURE_ADDRESS_MODE mode)
		{
			D3D12_STATIC_SAMPLER_DESC s{};
			s.ShaderRegister = reg;
			s.Filter = filter;
			s.AddressU = s.AddressV = s.AddressW = mode;
			s.MaxAnisotropy = 8;
			s.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
			s.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
			s.MaxLOD = D3D12_FLOAT32_MAX;
			s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			return s;
		};
		const D3D12_STATIC_SAMPLER_DESC samplers[] =
		{
			sampler(0, D3D12_FILTER_MIN_MAG_MIP_POINT,  D3D12_TEXTURE_ADDRESS_MODE_WRAP),
			sampler(1, D3D12_FILTER_MIN_MAG_MIP_POINT,  D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
			sampler(2, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP),
			sampler(3, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
			sampler(4, D3D12_FILTER_ANISOTROPIC,        D3D12_TEXTURE_ADDRESS_MODE_WRAP),
			sampler(5, D3D12_FILTER_ANISOTROPIC,        D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
		};

		D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
		desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
		desc.Desc_1_1.NumParameters = _countof(params);
		desc.Desc_1_1.pParameters = params;
		desc.Desc_1_1.NumStaticSamplers = _countof(samplers);
		desc.Desc_1_1.pStaticSamplers = samplers;
		desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
			| D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

		ComPtr<ID3DBlob> blob;
		ComPtr<ID3DBlob> error;
		HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &blob, &error);
		if (FAILED(hr))
		{
			const char* message = error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown error";
			throw std::runtime_error(std::string("Root signature serialization failed: ") + message);
		}

		ComPtr<ID3D12RootSignature> rootSignature;
		DX_CHECK(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSignature)));
		SetDebugName(rootSignature.Get(), "BindlessRootSignature");
		return rootSignature;
	}

	// ---------------------------------------------------------------------------

	void PipelineLibrary::Initialize(Device& device, ID3D12RootSignature* rootSignature, const std::filesystem::path& shaderRoot)
	{
		mDevice = &device;
		mRootSignature = rootSignature;
		mShaderRoot = shaderRoot;
		mCompiler.Initialize();
		mShaderSignature = ShaderTreeSignature();
		CHERRY_LOG("Shader root: %s", mShaderRoot.string().c_str());
	}

	void PipelineLibrary::Shutdown()
	{
		mEntries.clear();
	}

	void PipelineLibrary::AddGraphics(const std::string& name, const GraphicsPipelineDesc& desc)
	{
		Entry entry;
		entry.compute = false;
		entry.graphics = desc;
		if (!Build(name, entry))
		{
			throw std::runtime_error("Failed to build pipeline '" + name + "':\n" + mLastError);
		}
		mEntries[name] = std::move(entry);
	}

	void PipelineLibrary::AddCompute(const std::string& name, const ComputePipelineDesc& desc)
	{
		Entry entry;
		entry.compute = true;
		entry.computeDesc = desc;
		if (!Build(name, entry))
		{
			throw std::runtime_error("Failed to build pipeline '" + name + "':\n" + mLastError);
		}
		mEntries[name] = std::move(entry);
	}

	ID3D12PipelineState* PipelineLibrary::Get(const std::string& name) const
	{
		auto it = mEntries.find(name);
		return it != mEntries.end() ? it->second.pso.Get() : nullptr;
	}

	bool PipelineLibrary::Build(const std::string& name, Entry& entry)
	{
		ID3D12Device* d3d = mDevice->D3D();
		auto compile = [&](const std::string& file, const std::string& entryPoint, ShaderType type, const std::vector<std::string>& defines, CompiledShader& out)
		{
			ShaderDesc sd;
			sd.type = type;
			sd.entryPoint = entryPoint;
			sd.filePath = mShaderRoot / file;
			sd.defines = defines;
			sd.includeDirs.push_back(mShaderRoot);
			out = mCompiler.Compile(sd);
			if (!out.success)
			{
				mLastError = "[" + name + "] " + file + " (" + entryPoint + "):\n" + out.errors;
				CHERRY_ERROR("Shader compile failed: %s", mLastError.c_str());
				return false;
			}
			return true;
		};

		ComPtr<ID3D12PipelineState> pso;
		if (entry.compute)
		{
			const ComputePipelineDesc& desc = entry.computeDesc;
			CompiledShader cs;
			if (!compile(desc.shaderFile, desc.entry, ShaderType::Compute, desc.defines, cs)) return false;

			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
			psoDesc.pRootSignature = mRootSignature;
			psoDesc.CS = { cs.bytecode.data(), cs.bytecode.size() };
			HRESULT hr = d3d->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso));
			if (FAILED(hr))
			{
				mLastError = "[" + name + "] CreateComputePipelineState failed: " + HResultToString(hr);
				return false;
			}
		}
		else
		{
			const GraphicsPipelineDesc& desc = entry.graphics;
			CompiledShader vs, ps;
			if (!compile(desc.shaderFile, desc.vsEntry, ShaderType::Vertex, desc.defines, vs)) return false;
			const bool hasPixelShader = !desc.psEntry.empty();
			if (hasPixelShader && !compile(desc.shaderFile, desc.psEntry, ShaderType::Pixel, desc.defines, ps)) return false;

			static const D3D12_INPUT_ELEMENT_DESC kMeshLayout[] =
			{
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			};

			D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
			psoDesc.pRootSignature = mRootSignature;
			psoDesc.VS = { vs.bytecode.data(), vs.bytecode.size() };
			if (hasPixelShader) psoDesc.PS = { ps.bytecode.data(), ps.bytecode.size() };
			if (desc.vertexLayout == VertexLayout::Mesh)
			{
				psoDesc.InputLayout = { kMeshLayout, _countof(kMeshLayout) };
			}

			psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
			psoDesc.RasterizerState.CullMode = desc.cullMode;
			psoDesc.RasterizerState.FrontCounterClockwise = FALSE;

			psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
			if (desc.alphaBlend)
			{
				auto& rt = psoDesc.BlendState.RenderTarget[0];
				rt.BlendEnable = TRUE;
				rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
				rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
				rt.BlendOp = D3D12_BLEND_OP_ADD;
				rt.SrcBlendAlpha = D3D12_BLEND_ONE;
				rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
				rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			}

			psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
			psoDesc.DepthStencilState.DepthEnable = desc.depthTest ? TRUE : FALSE;
			psoDesc.DepthStencilState.DepthWriteMask = desc.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
			psoDesc.DepthStencilState.DepthFunc = desc.depthFunc;
			psoDesc.DSVFormat = desc.dsvFormat;

			psoDesc.SampleMask = UINT_MAX;
			psoDesc.PrimitiveTopologyType = desc.topology;
			psoDesc.NumRenderTargets = static_cast<UINT>(desc.rtvFormats.size());
			for (size_t i = 0; i < desc.rtvFormats.size(); ++i)
			{
				psoDesc.RTVFormats[i] = desc.rtvFormats[i];
			}
			psoDesc.SampleDesc.Count = 1;

			HRESULT hr = d3d->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso));
			if (FAILED(hr))
			{
				mLastError = "[" + name + "] CreateGraphicsPipelineState failed: " + HResultToString(hr);
				CHERRY_ERROR("%s", mLastError.c_str());
				return false;
			}
		}

		SetDebugName(pso.Get(), name.c_str());
		if (entry.pso)
		{
			// The old PSO may still be referenced by in-flight frames.
			mDevice->DeferRelease(entry.pso);
		}
		entry.pso = pso;
		return true;
	}

	uint64_t PipelineLibrary::ShaderTreeSignature() const
	{
		// Hash of every file's path and timestamp: catches edits, new/deleted files and
		// files restored with an older timestamp.
		uint64_t hash = 1469598103934665603ull;
		auto mix = [&hash](uint64_t value)
		{
			hash ^= value;
			hash *= 1099511628211ull;
		};
		std::error_code ec;
		for (auto it = std::filesystem::recursive_directory_iterator(mShaderRoot, ec);
			!ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
		{
			if (it->is_regular_file(ec))
			{
				mix(std::hash<std::wstring>{}(it->path().wstring()));
				mix(static_cast<uint64_t>(it->last_write_time(ec).time_since_epoch().count()));
			}
		}
		return hash;
	}

	bool PipelineLibrary::PollHotReload()
	{
		const uint64_t now = GetTickCount64();
		if (now - mLastPollTick < 500)
		{
			return false;
		}
		mLastPollTick = now;

		const uint64_t signature = ShaderTreeSignature();
		if (signature == mShaderSignature)
		{
			return false;
		}
		mShaderSignature = signature;
		ReloadAll();
		return true;
	}

	void PipelineLibrary::ReloadAll()
	{
		mLastError.clear();
		uint32_t failed = 0;
		for (auto& [name, entry] : mEntries)
		{
			if (!Build(name, entry)) ++failed;
		}
		++mReloadCount;
		if (failed == 0)
		{
			CHERRY_LOG("Shaders reloaded (%zu pipelines)", mEntries.size());
		}
		else
		{
			CHERRY_WARN("Shader reload: %u pipeline(s) failed, keeping previous versions", failed);
		}
	}
}
