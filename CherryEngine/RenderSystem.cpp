#include "RenderSystem.h"
#include <Graphics\D3DCore.h>
#include <PassConstant.h>
#include <Camera.h>
#include "thirdparty/imgui/imgui.h"
#include "thirdparty/imgui/imgui_impl_win32.h"
#include "thirdparty/imgui/imgui_impl_dx12.h"

extern Coordinator gCoordinator;
void RenderSystem::Init(RenderSystemParams renderSystemParams)
{
	mRenderSystemParams = renderSystemParams;
	mHwnd = renderSystemParams.hwnd;
	CreateD3D12Device();
	CreateFence();
	InitDescriptorSize();
	CheckQualitySupport();
	CreateCommandList();
	CreateSwapChain();
	CreateRtvAndDsvDescriptorHeaps();
	CreateRenderTargetResource();
	CreateDepthStencilResource();
	CreateViewport();
	CreateVertexInputLayout();
	CreateRootSignature();
	CreatePSO();
	CreateConstantBuffers();
	InitializeImgui();
}

void RenderSystem::OnEntityAdded(Entity entity)
{
	descriptor_handle handle = mSrvDescHeap.allocate();
	mEntityToDescriptorHandleMap.emplace(entity, handle);

	D3D12_GPU_VIRTUAL_ADDRESS cbAddress = mObjectConstantsBuffer->GetBuffer()->GetGPUVirtualAddress();

	// Allocate a CB index either from the free list or from the next counter.
	UINT16 index;
	if (!mFreeCbIndices.empty()) {
		index = mFreeCbIndices.front();
		mFreeCbIndices.pop();
	} else {
		index = mNextCbIndex++;
	}
	mEntityToCbIndexMap.emplace(entity, index);
	UINT elementByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
	cbAddress += index * elementByteSize;

	D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc;
	cbvDesc.BufferLocation = cbAddress;
	cbvDesc.SizeInBytes = elementByteSize;

	mDevice->CreateConstantBufferView(
		&cbvDesc,
		handle.cpu
	);

	Renderable renderable = gCoordinator.GetComponent<Renderable>(entity);
	renderable.material;
}

void RenderSystem::OnEntityRemoved(Entity entity)
{
	if (mEntityToDescriptorHandleMap.find(entity) == mEntityToDescriptorHandleMap.end())
	{
		return;
	}
	descriptor_handle handle = mEntityToDescriptorHandleMap[entity];
	mSrvDescHeap.free(handle);

	// Reclaim constant buffer index for reuse
	auto it = mEntityToCbIndexMap.find(entity);
	if (it != mEntityToCbIndexMap.end()) {
		UINT16 index = it->second;
		mFreeCbIndices.push(index);
		mEntityToCbIndexMap.erase(it);
	}

	// Remove descriptor mapping
	mEntityToDescriptorHandleMap.erase(entity);
}


void RenderSystem::CreateD3D12Device()
{
	mDevice = D3DCore::Device();
}

void RenderSystem::CreateFence()
{
	mDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mFence));
}

void RenderSystem::InitDescriptorSize()
{
	mRtvDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	mDsvDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	mCbvSrvDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void RenderSystem::CheckQualitySupport()
{
	D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msQualityLevels;
	msQualityLevels.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	msQualityLevels.SampleCount = 4;
	msQualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
	msQualityLevels.NumQualityLevels = 0;
	mDevice->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &msQualityLevels, sizeof(msQualityLevels));
	m4xMsaaQuality = msQualityLevels.NumQualityLevels;
	assert(m4xMsaaQuality > 0 && "Unexpected MSAA quality level.");
}

void RenderSystem::CreateCommandList()
{
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

	ThrowIfFailed(mDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&mCommandQueue)));

	ThrowIfFailed(mDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
		IID_PPV_ARGS(mDirectCmdListAlloc.GetAddressOf())));

	ThrowIfFailed(mDevice->CreateCommandList(0,
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		mDirectCmdListAlloc.Get(),
		nullptr,
		IID_PPV_ARGS(mCommandList.GetAddressOf())));

	mCommandList->Close();
	ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(),
		nullptr));
}

void RenderSystem::CreateSwapChain()
{
	mSwapChain.Reset();

	DXGI_SWAP_CHAIN_DESC sd;
	sd.BufferDesc.Width = mClientWidth;
	sd.BufferDesc.Height = mClientHeight;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.BufferDesc.Format = mBackBufferFormat;
	sd.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	sd.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
	sd.SampleDesc.Count = m4xMsaaState ? 4 : 1;
	sd.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = mHwnd;
	sd.Windowed = true;
	sd.BufferCount = kSwapChainBufferCount;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

	ThrowIfFailed(D3DCore::Factory()->CreateSwapChain(mCommandQueue.Get(), &sd,
		mSwapChain.GetAddressOf()));
}

void RenderSystem::CreateRtvAndDsvDescriptorHeaps()
{

	bool result = true;

	result &= mRtvDescHeap.initialize(512, false);
	result &= mDsvDescHeap.initialize(512, false);
	result &= mSrvDescHeap.initialize(512, true);
}

void RenderSystem::CreateRenderTargetResource()
{
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHeapHandle(
		mRtvDescHeap.cpu_start()
	);
	for (UINT i = 0; i < kSwapChainBufferCount; i++)
	{
		ThrowIfFailed(mSwapChain->GetBuffer(i, IID_PPV_ARGS(mSwapChainBuffer[i].GetAddressOf())));
		mDevice->CreateRenderTargetView(
			mSwapChainBuffer[i].Get(), nullptr, rtvHeapHandle
		);
		rtvHeapHandle.Offset(1, mRtvDescriptorSize);
	}
}

void RenderSystem::CreateDepthStencilResource()
{
	D3D12_RESOURCE_DESC depthStencilDesc;
	depthStencilDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthStencilDesc.Alignment = 0;
	depthStencilDesc.Width = mClientWidth;
	depthStencilDesc.Height = mClientHeight;
	depthStencilDesc.DepthOrArraySize = 1;
	depthStencilDesc.MipLevels = 1;
	depthStencilDesc.Format = mDepthStencilFormat;
	depthStencilDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
	depthStencilDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
	depthStencilDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	depthStencilDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE optClear;
	optClear.Format = mDepthStencilFormat;
	optClear.DepthStencil.Depth = 1.0f;
	optClear.DepthStencil.Stencil = 0;
	ThrowIfFailed(mDevice->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&depthStencilDesc,
		D3D12_RESOURCE_STATE_COMMON,
		&optClear,
		IID_PPV_ARGS(mDepthStencilBuffer.GetAddressOf())
	));

	mDevice->CreateDepthStencilView(
		mDepthStencilBuffer.Get(),
		nullptr,
		DepthStencilView()
	);

	mCommandList->ResourceBarrier(
		1, &CD3DX12_RESOURCE_BARRIER::Transition(
			mDepthStencilBuffer.Get(),
			D3D12_RESOURCE_STATE_COMMON,
			D3D12_RESOURCE_STATE_DEPTH_WRITE
		)
	);
}

void RenderSystem::CreateViewport()
{
	mScreenViewport.TopLeftX = 0.0f;
	mScreenViewport.TopLeftY = 0.0f;
	mScreenViewport.Width = static_cast<float>(mClientWidth);
	mScreenViewport.Height = static_cast<float>(mClientHeight);
	mScreenViewport.MaxDepth = 1.0f;
	mScreenViewport.MinDepth = 0.0f;

	mScissorRect = { 0, 0, (long)mClientWidth, (long)mClientHeight };
}

void RenderSystem::CreateVertexInputLayout()
{
	mVertexDesc =
	{
		{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,0,0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT,0,12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
	};
}

void RenderSystem::CreateRootSignature()
{
	//root signature
	CD3DX12_ROOT_PARAMETER slotRootParameter[5];
	CD3DX12_DESCRIPTOR_RANGE cbvTable;
	cbvTable.Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
		1,
		0
	);
	CD3DX12_DESCRIPTOR_RANGE cbvTable1;
	cbvTable1.Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
		1,
		1
	);

	CD3DX12_DESCRIPTOR_RANGE texTable;
	texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
	//cube map
	CD3DX12_DESCRIPTOR_RANGE texTable2;
	texTable2.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
	slotRootParameter[0].InitAsDescriptorTable(1, &cbvTable);
	slotRootParameter[1].InitAsDescriptorTable(1, &cbvTable1);
	slotRootParameter[2].InitAsConstantBufferView(2);
	slotRootParameter[3].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_PIXEL);
	slotRootParameter[4].InitAsDescriptorTable(1, &texTable2, D3D12_SHADER_VISIBILITY_PIXEL);

	std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> staticSamplers = GetStaticSamplers();

	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(5, slotRootParameter, staticSamplers.size(), staticSamplers.data(),
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

	ThrowIfFailed(mDevice->CreateRootSignature(0, serializedRootSig->GetBufferPointer(), serializedRootSig->GetBufferSize(), IID_PPV_ARGS(&mRootSignature)));

}

void RenderSystem::CreatePSO()
{
	mvsByteCode = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr,
		"VS", "vs_5_0");
	mpsByteCode = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr,
		"PS", "ps_5_0");

	CD3DX12_RASTERIZER_DESC rsDesc(D3D12_DEFAULT);
	rsDesc.FillMode = D3D12_FILL_MODE_SOLID;
	rsDesc.CullMode = D3D12_CULL_MODE_NONE;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc;
	ZeroMemory(&psoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	psoDesc.InputLayout = { mVertexDesc.data(), (UINT)mVertexDesc.size() };
	psoDesc.pRootSignature = mRootSignature.Get();
	psoDesc.VS = { reinterpret_cast<BYTE*>(mvsByteCode->GetBufferPointer()), mvsByteCode->GetBufferSize() };
	psoDesc.PS = { reinterpret_cast<BYTE*>(mpsByteCode->GetBufferPointer()), mpsByteCode->GetBufferSize() };

	psoDesc.RasterizerState = rsDesc;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = mBackBufferFormat;
	psoDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
	psoDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
	psoDesc.DSVFormat = mDepthStencilFormat;

	ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPSO)));

	mPSOs["opaque"] = mPSO;
}

void RenderSystem::CreateConstantBuffers()
{
    auto mainPassConstantBuffer = std::make_unique<ConstantBuffer<PassConstant>>(mDevice.Get(), 1);
	UINT passByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstant));

	auto descriptorHandle = mSrvDescHeap.allocate();

	D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc;
	cbvDesc.BufferLocation = mainPassConstantBuffer->GetBuffer()->GetGPUVirtualAddress();
	cbvDesc.SizeInBytes = passByteSize;

    mDevice->CreateConstantBufferView(&cbvDesc, descriptorHandle.cpu);

    mMainPassCbWrapper = std::make_unique<ConstantBufferWrapper<PassConstant>>(std::move(mainPassConstantBuffer), descriptorHandle);

    mObjectConstantsBuffer = std::make_unique<ConstantBuffer<ObjectConstants>>(mDevice.Get(), 1024);
    mMaterialConstantsBuffer = std::make_unique<ConstantBuffer<MaterialConstants>>(mDevice.Get(), 1024);
}

std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> RenderSystem::GetStaticSamplers()
{
	const CD3DX12_STATIC_SAMPLER_DESC pointWrap(
		0, D3D12_FILTER_MIN_MAG_MIP_POINT,
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,
		D3D12_TEXTURE_ADDRESS_MODE_WRAP
	);

	const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
		1, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP, // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP, // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW
	const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
		2, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP, // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP, // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW
	const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
		3, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP, // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP, // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW
	const CD3DX12_STATIC_SAMPLER_DESC anisotropicWrap(
		4, // shaderRegister
		D3D12_FILTER_ANISOTROPIC, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP, // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP, // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP, // addressW
		0.0f, // mipLODBias
		8); // maxAnisotropy
	const CD3DX12_STATIC_SAMPLER_DESC anisotropicClamp(
		5, // shaderRegister
		D3D12_FILTER_ANISOTROPIC, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP, // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP, // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP, // addressW
		0.0f, // mipLODBias
		8); // maxAnisotropy

	return { pointWrap, pointClamp, linearWrap, linearClamp, anisotropicWrap, anisotropicClamp };
}

void RenderSystem::InitializeImgui() {
	IMGUI_CHECKVERSION();

	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

	ImGui_ImplWin32_Init(mHwnd);


	ImGui_ImplDX12_InitInfo init_info = {};
	init_info.Device = mDevice.Get();
	init_info.CommandQueue = mCommandQueue.Get();
	init_info.NumFramesInFlight = 1;
	init_info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

	init_info.SrvDescriptorHeap = mSrvDescHeap.heap();
	descriptor_handle handle = mSrvDescHeap.allocate();
	init_info.LegacySingleSrvCpuDescriptor = handle.cpu;
	init_info.LegacySingleSrvGpuDescriptor = handle.gpu;

	ImGui_ImplDX12_Init(&init_info);
}

D3D12_CPU_DESCRIPTOR_HANDLE RenderSystem::CurrentBackBufferView() const
{
	return CD3DX12_CPU_DESCRIPTOR_HANDLE(
		mRtvDescHeap.cpu_start(),
		mCurrBackBuffer,
		mRtvDescriptorSize
	);
}

D3D12_CPU_DESCRIPTOR_HANDLE RenderSystem::DepthStencilView() const
{
	return mDsvDescHeap.cpu_start();
}

void RenderSystem::UpdateCbs(float dt)
{
	PassConstant mMainPassCb;
	Entity camera = mRenderSystemParams.camera;
	auto cameraTransform = gCoordinator.GetComponent<Transform>(camera);
	auto cameraData = gCoordinator.GetComponent<Camera>(camera);
	cameraData.CreateViewFromTransform(cameraTransform);

	XMMATRIX view = XMLoadFloat4x4(&cameraData.view);
	XMMATRIX proj = XMLoadFloat4x4(&cameraData.proj);
	XMMATRIX viewProj = XMMatrixMultiply(view, proj);
	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
	XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
	XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

	XMStoreFloat4x4(&mMainPassCb.view, XMMatrixTranspose(view));
	XMStoreFloat4x4(&mMainPassCb.invView, XMMatrixTranspose(invView));
	XMStoreFloat4x4(&mMainPassCb.proj, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&mMainPassCb.invProj, XMMatrixTranspose(invProj));
	XMStoreFloat4x4(&mMainPassCb.viewProj, XMMatrixTranspose(viewProj));
	XMStoreFloat4x4(&mMainPassCb.invViewProj, XMMatrixTranspose(invViewProj));

	mMainPassCb.eyePosW = cameraTransform.position;
	mMainPassCb.renderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
	mMainPassCb.invRenderTargetSize = XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
	mMainPassCb.nearZ = 1.0f;
	mMainPassCb.farZ = 1000.0f;
	mMainPassCb.totalTime = dt;
	mMainPassCb.deltaTime = dt;
	mMainPassCb.ambientLight = { 0.25f, 0.25f, 0.35f, 0.1f };

	mMainPassCb.lights[0].Direction = { 0.57735f, -0.57735f, 0.57735f };
	mMainPassCb.lights[0].Strength = { 0.6f, 0.6f, 0.6f };
	mMainPassCb.lights[1].Direction = { -0.57735f, -0.57735f, 0.57735f };
	mMainPassCb.lights[1].Strength = { 0.3f, 0.3f, 0.3f };
	mMainPassCb.lights[2].Direction = { 0.0f, -0.707f, -0.707f };
	mMainPassCb.lights[2].Strength = { 0.15f, 0.15f, 0.15f };

	mMainPassCbWrapper->constantBuffer->CopyData(0, mMainPassCb);
}

void RenderSystem::UpdateEntityCbs(float dt)
{
	for (Entity entity : mEntities)
	{
		Transform transform = gCoordinator.GetComponent<Transform>(entity);
		XMFLOAT3 position = transform.position;
		XMMATRIX translationMatrix = XMMatrixTranslation(position.x, position.y, position.z);
		XMFLOAT3 rotation = transform.rotation;
		XMMATRIX rotationMatrix = XMMatrixRotationRollPitchYawFromVector(XMVectorSet(rotation.x, rotation.y, rotation.z, 0));
		XMFLOAT3 scale = transform.scale;
		XMMATRIX scaleMatrix = XMMatrixScalingFromVector(XMVectorSet(scale.x, scale.y, scale.z, 0));

		XMMATRIX objWorld = translationMatrix * rotationMatrix * scaleMatrix;
		ObjectConstants objectConstant;
		XMStoreFloat4x4(&objectConstant.World, XMMatrixTranspose(objWorld));

		UINT16 index = mEntityToCbIndexMap[entity];
		mObjectConstantsBuffer->CopyData(index, objectConstant);
	}
}

void RenderSystem::UpdateMaterialCbs(float dt)
{
	for (const auto& pair : mMaterials)
	{
		const auto& material = pair.second;
		MaterialConstants materialConstant;
		materialConstant.diffuseAlbedo = material->DiffuseAlbedo;
		materialConstant.fresnelR0 = material->FresnelR0;
		materialConstant.roughness = material->Roughness;
		materialConstant.matTransform = material->MatTransform;

		mMaterialConstantsBuffer->CopyData(material->MatCBIndex, materialConstant);
	}
}

void RenderSystem::Update(float dt)
{
	UpdateCbs(dt);
	UpdateEntityCbs(dt);
	UpdateMaterialCbs(dt);

	// (Your code process and dispatch Win32 messages)
	// Start the Dear ImGui frame
	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
	ImGui::Begin("Viewport"); // Show demo window! :)
	ImGui::End();

	ThrowIfFailed(mDirectCmdListAlloc->Reset());
	ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), mPSO.Get()));

	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mSwapChainBuffer[mCurrBackBuffer].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));
	mCommandList->RSSetViewports(1, &mScreenViewport);
	mCommandList->RSSetScissorRects(1, &mScissorRect);

	if (mRaster)
	{
		mCommandList->ClearRenderTargetView(CurrentBackBufferView(), DirectX::Colors::Green, 0, nullptr);
		mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
		mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());
		ID3D12DescriptorHeap* DescriptorHeaps[] = { mSrvDescHeap.heap()};
		mCommandList->SetDescriptorHeaps(_countof(DescriptorHeaps), DescriptorHeaps);
		mCommandList->SetGraphicsRootSignature(mRootSignature.Get());
		if (mSrvDescHeap.size() > 0)
		{
			mCommandList->SetGraphicsRootDescriptorTable(1, mMainPassCbWrapper->descriptorHandle.gpu);

			UINT matCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(MaterialConstants));

			for (auto& const entity : mEntities)
			{
				auto& renderable = gCoordinator.GetComponent<Renderable>(entity);
				mCommandList->IASetVertexBuffers(0, 1, &renderable.geometry->GetVertexBufferView());
				mCommandList->IASetIndexBuffer(&renderable.geometry->GetIndexBufferView());
				mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

				//Set the position to draw the entity at
				descriptor_handle handle = mEntityToDescriptorHandleMap[entity];
				mCommandList->SetGraphicsRootDescriptorTable(0, handle.gpu);

				//material
				D3D12_GPU_VIRTUAL_ADDRESS matCbAddress = mMaterialConstantsBuffer->GetBuffer()->GetGPUVirtualAddress()
					+ renderable.material->MatCBIndex;
				mCommandList->SetGraphicsRootConstantBufferView(2, matCbAddress + renderable.material->MatCBIndex * matCBByteSize);

				//texture
				Texture* texture = mIdToTexture[renderable.material->TextureId];
				mCommandList->SetGraphicsRootDescriptorTable(3, texture->DescHandle.gpu);
				mCommandList->DrawIndexedInstanced(renderable.geometry->indexCount, 1, 0, 0, 0);
			}
		}
	}

	// Rendering
// (Your code clears your framebuffer, renders your other stuff etc.)
	ImGui::Render();
	ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), mCommandList.Get());
	// (Your code calls ExecuteCommandLists, swapchain's Present(), etc.)

	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mSwapChainBuffer[mCurrBackBuffer].Get(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

	ThrowIfFailed(mCommandList->Close());

	ID3D12CommandList* cmdsList[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsList), cmdsList);

	ThrowIfFailed(mSwapChain->Present(0, 0));
	mCurrBackBuffer = (mCurrBackBuffer + 1) % kSwapChainBufferCount;

	FlushCommandQueue();
}

void RenderSystem::ResetCommandList()
{
	mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr);
}

void RenderSystem::FlushCommandQueue()
{
	mCurrentFence++;

	// Add an instruction to the command queue to set a new fence point.  Because we 
	// are on the GPU timeline, the new fence point won't be set until the GPU finishes
	// processing all the commands prior to this Signal().
	ThrowIfFailed(mCommandQueue->Signal(mFence.Get(), mCurrentFence));

	// Wait until the GPU has completed commands up to this fence point.
	if (mFence->GetCompletedValue() < mCurrentFence)
	{
		HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);

		// Fire event when GPU hits current fence.  
		ThrowIfFailed(mFence->SetEventOnCompletion(mCurrentFence, eventHandle));

		// Wait until the GPU hits current fence event is fired.
		WaitForSingleObject(eventHandle, INFINITE);
		CloseHandle(eventHandle);
	}
}

void RenderSystem::CmdListCloseAndExecute()
{
	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);
}

void RenderSystem::CreateMaterial(std::string name, XMFLOAT4 diffuseAlbedo, XMFLOAT3 fresnelR0, float roughness, int diffuseSrvHeapIndex)
{
	std::unique_ptr<Material> material = std::make_unique<Material>();
	material->Name = name;
	material->MatCBIndex = mMaterials.size();
	material->DiffuseAlbedo = diffuseAlbedo;
	material->FresnelR0 = fresnelR0;
	material->Roughness = roughness;
	material->DiffuseSrvHeapIndex = diffuseSrvHeapIndex;
	material->TextureId = 0;

	mMaterials[name] = std::move(material);
}

Material* RenderSystem::GetMaterial(std::string name)
{
	if (mMaterials.find(name) != mMaterials.end())
	{
		return mMaterials[name].get();
	}

	return nullptr;
}

int RenderSystem::CreateTexture(const std::string& name, const std::wstring& file)
{
	std::unique_ptr<Texture> texture = std::make_unique<Texture>();
	texture->Name = "woodCrateTex";
	texture->Filename = file;

	ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(
		mDevice.Get(), mCommandList.Get(), texture->Filename.c_str(),
		texture->Resource, texture->UploadHeap));


	descriptor_handle handle = mSrvDescHeap.allocate();
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;
	srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = texture->Resource->GetDesc().Format;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = texture->Resource->GetDesc().MipLevels;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	mDevice->CreateShaderResourceView(texture->Resource.Get(), &srvDesc, handle.cpu);

	texture->DescHandle = handle;
	int id = mTextures.size();
	mIdToTexture[id] = texture.get();
	mTextures[texture->Name] = std::move(texture);

	return id;
}