#include "D3DApp.h"
#include <WindowsX.h>

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
	return D3DApp::GetApp()->MsgProc(hwnd, message, wparam, lparam);
}


D3DApp::D3DApp()
{
	assert(mApp == nullptr);
	mApp = this;
}

D3DApp* D3DApp::GetApp()
{
	return mApp;
}
D3DApp* D3DApp::mApp = nullptr;
D3DApp::~D3DApp()
{
	if (m_device != nullptr)
		FlushCommandQueue();
}

void D3DApp::Init()
{
#define DEBUG
#if defined(DEBUG) || defined(_DEBUG)
	{
		Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
		ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)));
		debugController->EnableDebugLayer();
	}
#endif
	ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&mdxgiFactory)));

	HRESULT hardwareResult = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device));

	if (FAILED(hardwareResult))
	{
		Microsoft::WRL::ComPtr<IDXGIAdapter> pWarpAdapter;
		ThrowIfFailed(mdxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&pWarpAdapter)));
		ThrowIfFailed(D3D12CreateDevice(
			pWarpAdapter.Get(),
			D3D_FEATURE_LEVEL_11_0,
			IID_PPV_ARGS(&m_device)));
	}

	m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
	m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	m_dsvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	m_cbvSrvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msQualityLevels;
	msQualityLevels.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	msQualityLevels.SampleCount = 4;
	msQualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
	msQualityLevels.NumQualityLevels = 0;
	m_device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &msQualityLevels, sizeof(msQualityLevels));
	m4xMsaaQuality = msQualityLevels.NumQualityLevels;
	assert(m4xMsaaQuality > 0 && "Unexpected MSAA quality level.");



	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

	ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&mCommandQueue)));

	ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
		IID_PPV_ARGS(mDirectCmdListAlloc.GetAddressOf())));

	ThrowIfFailed(m_device->CreateCommandList(0,
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		mDirectCmdListAlloc.Get(),
		nullptr,
		IID_PPV_ARGS(mCommandList.GetAddressOf())));

	mCommandList->Close();
	ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(),
		nullptr));
	CreateSwapChain();
	CreateRtvAndDsvDescriptorHeaps();
	CreateRenderTargetResources();
	CreateDepthStencilResource();
	CreateViewport();
	VertexInputLayout();
	CreateVertexAndIndexBuffer();
	SeperatePosAndColorBuffer();
	CreateMaterials();
	CreateRenderObjects();
	CreateConstantBufferViewsForRenderObjects();

	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);
	// Wait until initialization is complete.
	FlushCommandQueue();
}

bool D3DApp::InitWindow()
{
	WNDCLASS wc;
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = WndProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = nullptr; //todo;
	wc.hIcon = LoadIcon(0, IDI_APPLICATION);
	wc.hCursor = LoadCursor(0, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
	wc.lpszMenuName = 0;
	wc.lpszClassName = L"MainWnd";

	if (!RegisterClass(&wc))
	{
		MessageBox(0, L"RegisterClass Failed.", 0, 0);
		return false;
	}

	RECT R = { 0,0,mClientWidth, mClientHeight };
	AdjustWindowRect(&R, WS_OVERLAPPEDWINDOW, false);
	int width = R.right - R.left;
	int height = R.bottom - R.top;
	mhMainWnd = CreateWindow(L"MainWnd", mMainWndCaption.c_str(),
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, width, height, 0, 0, nullptr, 0);

	if (!mhMainWnd)
	{
		MessageBox(0, L"CreateWindow Failed.", 0, 0);
		return false;
	}

	ShowWindow(mhMainWnd, SW_SHOW);
	UpdateWindow(mhMainWnd);
	return true;
}

void D3DApp::CreateSwapChain()
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
	sd.OutputWindow = mhMainWnd; //todo create window;
	sd.Windowed = true;
	sd.BufferCount = SwapChainBufferCount;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

	ThrowIfFailed(mdxgiFactory->CreateSwapChain(mCommandQueue.Get(), &sd,
		mSwapChain.GetAddressOf()));

}

void D3DApp::CreateRtvAndDsvDescriptorHeaps()
{
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc;
	rtvHeapDesc.NumDescriptors = 2;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	rtvHeapDesc.NodeMask = 0;

	ThrowIfFailed(m_device->CreateDescriptorHeap(
		&rtvHeapDesc, IID_PPV_ARGS(mRtvHeap.GetAddressOf()
		)));
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
	dsvHeapDesc.NumDescriptors = 1;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	dsvHeapDesc.NodeMask = 0;
	ThrowIfFailed(m_device->CreateDescriptorHeap(
		&dsvHeapDesc, IID_PPV_ARGS(mDsvHeap.GetAddressOf()
		)));

}

void D3DApp::CreateRenderTargetResources()
{
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHeapHandle(
		mRtvHeap->GetCPUDescriptorHandleForHeapStart()
	);
	for (UINT i = 0; i < SwapChainBufferCount; i++)
	{
		ThrowIfFailed(mSwapChain->GetBuffer(i, IID_PPV_ARGS(mSwapChainBuffer[i].GetAddressOf())));
		m_device->CreateRenderTargetView(
			mSwapChainBuffer[i].Get(), nullptr, rtvHeapHandle
		);
		rtvHeapHandle.Offset(1, m_rtvDescriptorSize);
	}
}

void D3DApp::CreateDepthStencilResource()
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
	ThrowIfFailed(m_device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&depthStencilDesc,
		D3D12_RESOURCE_STATE_COMMON,
		&optClear,
		IID_PPV_ARGS(mDepthStencilBuffer.GetAddressOf())
	));

	m_device->CreateDepthStencilView(
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

void D3DApp::CreateViewport()
{
	mScreenViewport.TopLeftX = 0.0f;
	mScreenViewport.TopLeftY = 0.0f;
	mScreenViewport.Width = static_cast<float>(mClientWidth);
	mScreenViewport.Height = static_cast<float>(mClientHeight);
	mScreenViewport.MaxDepth = 1.0f;
	mScreenViewport.MinDepth = 0.0f;

	mScissorRect = { 0, 0, (long)mClientWidth, (long)mClientHeight };
}

void D3DApp::VertexInputLayout()
{
	mVertexDesc =
	{
		{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,0,0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT,0,12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
	};

	D3D12_INPUT_ELEMENT_DESC VertexDesc2[] = {
		{"POSITION",0, DXGI_FORMAT_R32G32B32_FLOAT, 0,0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"TANGENT",0, DXGI_FORMAT_R32G32B32_FLOAT, 0,12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"NORMAL",0, DXGI_FORMAT_R32G32B32_FLOAT, 0,24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"TEXTURE",0, DXGI_FORMAT_R32G32_FLOAT, 0,36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"TEXTURE",1, DXGI_FORMAT_R32G32_FLOAT, 0,44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"COLOR",0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,52, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
	};

	mVertexDescSlots = {
		{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
	};
}

void D3DApp::CreateVertexAndIndexBuffer()
{

	//Vertex vertices[] =
	//{
	//	{ XMFLOAT3(-1.0f, -1.0f, -1.0f), XMFLOAT4(Colors::White) },
	//	{ XMFLOAT3(-1.0f, +1.0f, -1.0f), XMFLOAT4(Colors::Black) },
	//	{ XMFLOAT3(+1.0f, +1.0f, -1.0f), XMFLOAT4(Colors::Red) },
	//	{ XMFLOAT3(+1.0f, -1.0f, -1.0f), XMFLOAT4(Colors::Green) },
	//	{ XMFLOAT3(-1.0f, -1.0f, +1.0f), XMFLOAT4(Colors::Blue) },
	//	{ XMFLOAT3(-1.0f, +1.0f, +1.0f), XMFLOAT4(Colors::Yellow) },
	//	{ XMFLOAT3(+1.0f, +1.0f, +1.0f), XMFLOAT4(Colors::Cyan) },
	//	{ XMFLOAT3(+1.0f, -1.0f, +1.0f), XMFLOAT4(Colors::Magenta) }
	//};

	Vertex vertices[] = {
	//right
	{XMFLOAT3(-1.0f,-1.0f,-1.0f), XMFLOAT4(Colors::Red)},
	{XMFLOAT3(-1.0f,-1.0f, 1.0f), XMFLOAT4(Colors::Red)},
	{XMFLOAT3(-1.0f, 1.0f, 1.0f), XMFLOAT4(Colors::Red)},
	{XMFLOAT3(-1.0f,-1.0f,-1.0f), XMFLOAT4(Colors::Red)},
	{XMFLOAT3(-1.0f, 1.0f, 1.0f), XMFLOAT4(Colors::Red)},
	{XMFLOAT3(-1.0f, 1.0f,-1.0f), XMFLOAT4(Colors::Red)},
	//front
	{XMFLOAT3(1.0f, 1.0f,-1.0f),  XMFLOAT4(Colors::DarkGreen)},
	{XMFLOAT3(-1.0f,-1.0f,-1.0f), XMFLOAT4(Colors::DarkGreen)},
	{XMFLOAT3(-1.0f, 1.0f,-1.0f), XMFLOAT4(Colors::DarkGreen)},
	{XMFLOAT3(1.0f, 1.0f,-1.0f),  XMFLOAT4(Colors::DarkGreen)},
	{XMFLOAT3(1.0f,-1.0f,-1.0f),  XMFLOAT4(Colors::DarkGreen)},
	{XMFLOAT3(-1.0f,-1.0f,-1.0f), XMFLOAT4(Colors::DarkGreen)},
	//bottom
	{XMFLOAT3(1.0f,-1.0f, 1.0f),  XMFLOAT4(Colors::Purple)},
	{XMFLOAT3(-1.0f,-1.0f,-1.0f), XMFLOAT4(Colors::Purple)},
	{XMFLOAT3(1.0f,-1.0f,-1.0f),  XMFLOAT4(Colors::Purple)},
	{XMFLOAT3(1.0f,-1.0f, 1.0f),  XMFLOAT4(Colors::Purple)},
	{XMFLOAT3(-1.0f,-1.0f, 1.0f), XMFLOAT4(Colors::Purple)},
	{XMFLOAT3(-1.0f,-1.0f,-1.0f), XMFLOAT4(Colors::Purple)},
	//left
	{XMFLOAT3(1.0f, 1.0f, 1.0f),  XMFLOAT4(Colors::Blue)},
	{XMFLOAT3(1.0f,-1.0f,-1.0f),  XMFLOAT4(Colors::Blue)},
	{XMFLOAT3(1.0f, 1.0f,-1.0f),  XMFLOAT4(Colors::Blue)},
	{XMFLOAT3(1.0f,-1.0f,-1.0f),  XMFLOAT4(Colors::Blue)},
	{XMFLOAT3(1.0f, 1.0f, 1.0f),  XMFLOAT4(Colors::Blue)},
	{XMFLOAT3(1.0f,-1.0f, 1.0f),  XMFLOAT4(Colors::Blue)},
	//top
	{XMFLOAT3(1.0f, 1.0f, 1.0f),  XMFLOAT4(Colors::Yellow)},
	{XMFLOAT3(1.0f, 1.0f,-1.0f),  XMFLOAT4(Colors::Yellow)},
	{XMFLOAT3(-1.0f, 1.0f,-1.0f), XMFLOAT4(Colors::Yellow)},
	{XMFLOAT3(1.0f, 1.0f, 1.0f),  XMFLOAT4(Colors::Yellow)},
	{XMFLOAT3(-1.0f, 1.0f,-1.0f), XMFLOAT4(Colors::Yellow)},
	{XMFLOAT3(-1.0f, 1.0f, 1.0f), XMFLOAT4(Colors::Yellow)},
	//back
	{XMFLOAT3(-1.0f, 1.0f, 1.0f), XMFLOAT4(Colors::Black)},
	{XMFLOAT3(-1.0f,-1.0f, 1.0f), XMFLOAT4(Colors::Black)},
	{XMFLOAT3(1.0f,-1.0f, 1.0f),  XMFLOAT4(Colors::Black)},
	{XMFLOAT3(1.0f, 1.0f, 1.0f),  XMFLOAT4(Colors::Black)},
	{XMFLOAT3(-1.0f, 1.0f, 1.0f), XMFLOAT4(Colors::Black)},
	{XMFLOAT3(1.0f,-1.0f, 1.0f),  XMFLOAT4(Colors::Black)}
	};


	const UINT64 vbByteSize = 36 * sizeof(Vertex);

	VertexBufferGPU = d3dUtil::CreateDefaultBuffer(m_device, mCommandList.Get(), vertices, vbByteSize, VertexBufferUploader);


	//create vertex buffer view
	vbv.BufferLocation = VertexBufferGPU->GetGPUVirtualAddress();
	vbv.SizeInBytes = vbByteSize;
	vbv.StrideInBytes = sizeof(Vertex);

	//indices
	std::uint16_t indices[] = {
		// front face
		0, 1, 2,
		0, 2, 3,
		// back face
		4, 6, 5,
		4, 7, 6,
		// left face
		4, 5, 1,
		4, 1, 0,
		// right face
		3, 2, 6,
		3, 6, 7,
		// top face
		1, 5, 6,
		1, 6, 2,
		// bottom face
		4, 0, 3,
		4, 3, 7
	};

	const UINT ibByteSize = 36 * sizeof(std::uint16_t);

	IndexBufferGPU = d3dUtil::CreateDefaultBuffer(m_device, mCommandList.Get(), indices, ibByteSize, IndexBufferUploader);

	ibv.BufferLocation = IndexBufferGPU->GetGPUVirtualAddress();
	ibv.Format = DXGI_FORMAT_R16_UINT;
	ibv.SizeInBytes = ibByteSize;

	std::unique_ptr<Geometry> Cube(new Geometry());
	Cube->mVertexBuffer = d3dUtil::CreateDefaultBuffer(m_device, mCommandList.Get(), vertices, vbByteSize, Cube->mVertexUploadBuffer);
	Cube->mIndexBuffer = d3dUtil::CreateDefaultBuffer(m_device, mCommandList.Get(), indices, ibByteSize, Cube->mIndexUploadBuffer);
	Cube->mIbFormat = DXGI_FORMAT_R16_UINT;
	Cube->mIbByteSize = ibByteSize;
	Cube->mStrideInBytes = sizeof(Vertex);
	Cube->mVbByteSize = vbByteSize;
	Cube->mIndexCount = 36;
	Geometries.emplace("Cube", std::move(Cube));

	//root signature
	CD3DX12_ROOT_PARAMETER slotRootParameter[3];
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
	slotRootParameter[0].InitAsDescriptorTable(1, &cbvTable);
	slotRootParameter[1].InitAsDescriptorTable(1, &cbvTable1);
	slotRootParameter[2].InitAsConstantBufferView(2);
	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, slotRootParameter, 0, nullptr,
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

	ThrowIfFailed(m_device->CreateRootSignature(0, serializedRootSig->GetBufferPointer(), serializedRootSig->GetBufferSize(), IID_PPV_ARGS(&mRootSignature)));



	mvsByteCode = d3dUtil::CompileShader(L"Shaders\\VertexShader.hlsl", nullptr,
		"VS", "vs_5_0");
	mpsByteCode = d3dUtil::CompileShader(L"Shaders\\PixelShader.hlsl", nullptr,
		"PS", "ps_5_0");

	CD3DX12_RASTERIZER_DESC rsDesc(D3D12_DEFAULT);
	rsDesc.FillMode = D3D12_FILL_MODE_SOLID;
	rsDesc.CullMode = D3D12_CULL_MODE_NONE;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc;
	ZeroMemory(&psoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	psoDesc.InputLayout = { mVertexDesc.data(), (UINT)mVertexDesc.size() };
	psoDesc.pRootSignature = mRootSignature.Get();
	psoDesc.VS = { reinterpret_cast<BYTE*>(mvsByteCode->GetBufferPointer()), mvsByteCode->GetBufferSize()};
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

	ThrowIfFailed(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPSO)));

}

void D3DApp::SeperatePosAndColorBuffer()
{

	VPosData posData[] = {
		{ XMFLOAT3(-1.0f, -1.0f, -1.0f)},
		{ XMFLOAT3(-1.0f, +1.0f, -1.0f)},
		{ XMFLOAT3(+1.0f, +1.0f, -1.0f)},
		{ XMFLOAT3(+1.0f, -1.0f, -1.0f)},
		{ XMFLOAT3(-1.0f, -1.0f, +1.0f)},
		{ XMFLOAT3(-1.0f, +1.0f, +1.0f)},
		{ XMFLOAT3(+1.0f, +1.0f, +1.0f)},
		{ XMFLOAT3(+1.0f, -1.0f, +1.0f)}
	};

	VColorData colorData[] = {
		{XMFLOAT4(Colors::Green) },
		{XMFLOAT4(Colors::Green) },
		{XMFLOAT4(Colors::Green) },
		{XMFLOAT4(Colors::Green) },
		{XMFLOAT4(Colors::Green) },
		{XMFLOAT4(Colors::Green) },
		{XMFLOAT4(Colors::Green) },
		{XMFLOAT4(Colors::Green) }
	};
	
	const UINT64 posDataSize = 8 * sizeof(VPosData);
	const UINT64 colorDataSize = 8 * sizeof(VColorData);

	VertexPosBuffer = d3dUtil::CreateDefaultBuffer(m_device, mCommandList.Get(), posData, posDataSize, VertexPosBufferUploader);

	VertexColorBuffer = d3dUtil::CreateDefaultBuffer(m_device, mCommandList.Get(), colorData, colorDataSize, VertexColorBufferUploader);

	PosBufferView.BufferLocation = VertexPosBuffer->GetGPUVirtualAddress();
	PosBufferView.SizeInBytes = posDataSize;
	PosBufferView.StrideInBytes = sizeof(VPosData);

	ColorBufferView.BufferLocation = VertexColorBuffer->GetGPUVirtualAddress();
	ColorBufferView.SizeInBytes = colorDataSize;
	ColorBufferView.StrideInBytes = sizeof(VColorData);

}

void D3DApp::CreateRenderObjects()
{
	UINT cbIndex = 0;

	RenderObject renderObject;
	renderObject.ConstantBufferIndex = cbIndex;
	renderObject.Geometry = Geometries["Cube"].get();
	renderObject.IndexCount = Geometries["Cube"]->mIndexCount;
	renderObject.Mat = mMaterials["grass"].get();
	//cbIndex++;
	XMVECTOR pos = XMVectorSet(0, 0, -10, 1.0f);
	XMVECTOR target = XMVectorZero();
	XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

	XMMATRIX view = XMMatrixLookAtLH(pos, target, up);
	XMStoreFloat4x4(&mView, view);

	XMVECTOR translate = XMVectorSet(1, 5, 0, 1.0f);
	XMVECTOR scale = XMVectorSet(1, 1, 1, 1);
	mWorld = MathHelper::Identity4x4();
	XMStoreFloat4x4(&mProj, XMMatrixPerspectiveFovLH(0.25f * MathHelper::Pi, 800.0f / 600.0f, 1.0f, 1000.0f));

	XMMATRIX proj = XMLoadFloat4x4(&mProj);
	XMMATRIX world = XMLoadFloat4x4(&mWorld);


	XMStoreFloat4x4(&renderObject.World,
		world);
	renderObject.ConstantBufferIndex = cbIndex;

	for (int x = -1; x <= 1; x++)
	{
		for (int y = -1; y <= 1; y++)
		{
			for (int z = -1; z <= 1; z++)
			{
				world = XMMatrixTranslation(x * 2, y * 2, z*2);
				XMStoreFloat4x4(&renderObject.World,
					world);
				renderObject.ConstantBufferIndex = cbIndex;
				RenderObjects.push_back(renderObject);
				cbIndex++;
			}
		}
	}
}

void D3DApp::CreateConstantBufferViewsForRenderObjects()
{
	mObjectConstantsBuffer = new ConstantBuffer<ObjectConstants>(m_device, RenderObjects.size());
	mMainPassConstantBuffer = new ConstantBuffer<PassConstant>(m_device, 1);
	mMaterialConstantsBuffer = new ConstantBuffer<MaterialConstants>(m_device, mMaterials.size());
	UINT elementByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
	UINT passByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstant));
	UINT materialByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(MaterialConstants));
	//m_device->CreateCommittedResource(
	//	&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
	//	D3D12_HEAP_FLAG_NONE,
	//	&CD3DX12_RESOURCE_DESC::Buffer(elementByteSize * RenderObjects.size()),
	//	D3D12_RESOURCE_STATE_GENERIC_READ,
	//	nullptr,
	//	IID_PPV_ARGS(&mUploadCBuffer)
	//);

	//m_device->CreateCommittedResource(
	//	&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
	//	D3D12_HEAP_FLAG_NONE,
	//	&CD3DX12_RESOURCE_DESC::Buffer(passByteSize * 1),
	//	D3D12_RESOURCE_STATE_GENERIC_READ,
	//	nullptr,
	//	IID_PPV_ARGS(&mUploadPassBuffer)
	//);

	D3D12_DESCRIPTOR_HEAP_DESC cbvHeapDesc;
	cbvHeapDesc.NumDescriptors = RenderObjects.size() + 1 + mMaterials.size();
	cbvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	cbvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	cbvHeapDesc.NodeMask = 0;
	mPassCbOffset = RenderObjects.size();
	m_device->CreateDescriptorHeap(&cbvHeapDesc, IID_PPV_ARGS(&mCbvHeap));
	//mUploadCBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedData));
	//mUploadPassBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedPassData));
	for (int i = 0; i != RenderObjects.size(); i++)
	{
		ObjectConstants objConstants;
		objConstants.World = RenderObjects[i].World;
		//ObjectConstantsBuffer->CopyData(i, objConstants);
		//memcpy(&mMappedData[elementByteSize*i], &objConstants, elementByteSize);
		
		D3D12_GPU_VIRTUAL_ADDRESS cbAddress = mObjectConstantsBuffer->GetBuffer()->GetGPUVirtualAddress();
		cbAddress += i* elementByteSize;
		D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc;
		cbvDesc.BufferLocation = cbAddress;
		cbvDesc.SizeInBytes = elementByteSize;
		auto handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(mCbvHeap->GetCPUDescriptorHandleForHeapStart());
		handle.Offset(i, m_cbvSrvDescriptorSize);

		m_device->CreateConstantBufferView(
			&cbvDesc,
			handle
		);
	}
		
	auto handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(mCbvHeap->GetCPUDescriptorHandleForHeapStart());
	handle.Offset(mPassCbOffset, m_cbvSrvDescriptorSize);
	D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc;
	cbvDesc.BufferLocation = mMainPassConstantBuffer->GetBuffer()->GetGPUVirtualAddress();
	cbvDesc.SizeInBytes = passByteSize;

	m_device->CreateConstantBufferView(
		&cbvDesc,
		handle
	);
}

void D3DApp::CreateMaterials()
{
	std::unique_ptr<Material> grass = std::make_unique<Material>();
	grass->Name = "grass";
	grass->MatCBIndex = 0;
	grass->DiffuseAlbedo = XMFLOAT4(0.2f, 0.6f, 0.6f, 1.0f);
	grass->FresnelR0 = XMFLOAT3(0.01f, 0.01f, 0.01f);
	grass->Roughness = 0.125f;

	std::unique_ptr<Material> water = std::make_unique<Material>();
	water->Name = "water";
	water->MatCBIndex = 1;
	water->DiffuseAlbedo = XMFLOAT4(0.0f, 0.2f, 0.6f, 1.0f);
	water->FresnelR0 = XMFLOAT3(0.1f, 0.1f, 0.1f);

	mMaterials["grass"] = std::move(grass);
	mMaterials["water"] = std::move(water);
}

D3D12_CPU_DESCRIPTOR_HANDLE D3DApp::CurrentBackBufferView() const
{
	return CD3DX12_CPU_DESCRIPTOR_HANDLE(
		mRtvHeap->GetCPUDescriptorHandleForHeapStart(),
		mCurrBackBuffer,
		m_rtvDescriptorSize
	);


}

D3D12_CPU_DESCRIPTOR_HANDLE D3DApp::DepthStencilView() const
{
	return mDsvHeap->GetCPUDescriptorHandleForHeapStart();
}

int D3DApp::Run()
{
	MSG msg = { 0 };
	
	mTimer.Reset();
	while (msg.message != WM_QUIT)
	{
		if (PeekMessage(&msg, 0, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		else
		{
			mTimer.Tick();
			if (!mAppPaused)
			{
				CalculateFrameStats();
				Update(mTimer);
				Draw(mTimer);
			}
			else
			{
				Sleep(100);
			}
		}
	}

	return (int)msg.wParam;
}

LRESULT D3DApp::MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_CHAR: //this is just for a program exit besides window's borders/taskbar
		if (wParam == VK_ESCAPE)
		{
			DestroyWindow(hwnd);
		}
		break;
	case WM_DESTROY:
		PostQuitMessage(0);
		break;
	case WM_LBUTTONDOWN:
	case WM_MBUTTONDOWN:
	case WM_RBUTTONDOWN:
		OnMouseDown(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		return 0;
	case WM_LBUTTONUP:
	case WM_MBUTTONUP:
	case WM_RBUTTONUP:
		OnMouseUp(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		return 0;
	case WM_MOUSEMOVE:
		OnMouseMove(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		return 0;
	default:
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}
}

void D3DApp::FlushCommandQueue()
{
	mCurrentFence++;

	// Add an instruction to the command queue to set a new fence point.  Because we 
	// are on the GPU timeline, the new fence point won't be set until the GPU finishes
	// processing all the commands prior to this Signal().
	ThrowIfFailed(mCommandQueue->Signal(m_fence.Get(), mCurrentFence));

	// Wait until the GPU has completed commands up to this fence point.
	if (m_fence->GetCompletedValue() < mCurrentFence)
	{
		HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);

		// Fire event when GPU hits current fence.  
		ThrowIfFailed(m_fence->SetEventOnCompletion(mCurrentFence, eventHandle));

		// Wait until the GPU hits current fence event is fired.
		WaitForSingleObject(eventHandle, INFINITE);
		CloseHandle(eventHandle);
	}
}

void D3DApp::CalculateFrameStats()
{
	static int frameCnt = 0;
	static float timeElapsed = 0.0f;

	frameCnt++;

	// Compute averages over one second period.
	if ((mTimer.TotalTime() - timeElapsed) >= 1.0f)
	{
		float fps = (float)frameCnt; // fps = frameCnt / 1
		float mspf = 1000.0f / fps;

		std::wstring fpsStr = std::to_wstring(fps);
		std::wstring mspfStr = std::to_wstring(mspf);

		std::wstring windowText = mMainWndCaption +
			L"    fps: " + fpsStr +
			L"   mspf: " + mspfStr;

		SetWindowText(mhMainWnd, windowText.c_str());

		// Reset for next average.
		frameCnt = 0;
		timeElapsed += 1.0f;
	}
}
void D3DApp::Update(GameTimer& mTimer)
{
	mEyePos.x = mRadius * sinf(mPhi) * cosf(mTheta);
	mEyePos.z = mRadius * sinf(mPhi) * sinf(mTheta);
	mEyePos.y = mRadius * cosf(mPhi);

	// Build the view matrix.
	XMVECTOR pos = XMVectorSet(mEyePos.x, mEyePos.y, mEyePos.z, 1.0f);
	XMVECTOR target = XMVectorZero();
	XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

	XMMATRIX view = XMMatrixLookAtLH(pos, target, up);
	XMStoreFloat4x4(&mView, view);

	XMMATRIX world = XMLoadFloat4x4(&mWorld);
	world = XMMatrixRotationAxis(XMVectorSet(0, 1.0f, 0.0f, 1.0f), 0.0005f) * world;
	XMStoreFloat4x4(&mWorld, world);
	XMMATRIX proj = XMLoadFloat4x4(&mProj);
	XMMATRIX worldViewProj = world * view * proj;

	//
	// Update the constant buffer with the latest worldViewProj matrix.
	UpdateMainPasCb(mTimer);
	UpdateMaterialCb(mTimer);


	UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
	for (int i = 0; RenderObjects.size() != i; i++)
	{
		ObjectConstants objConstants;
		RenderObject& ro = RenderObjects[i];
		XMMATRIX objWorld = XMLoadFloat4x4(&ro.World);
		XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(objWorld));
		mObjectConstantsBuffer->CopyData(i, objConstants);
	}

	if (d3dUtil::IsKeyDown('W'))
	{
		for (int i = 0; i != RenderObjects.size(); i++)
		{
			RenderObject& ro = RenderObjects[i];
			XMMATRIX tempWorld = XMLoadFloat4x4(&ro.World);
			XMVECTOR trans;
			XMVECTOR scale;
			XMVECTOR rot;
			XMMatrixDecompose(&scale, &rot, &trans, tempWorld);
			XMFLOAT3 transStored;
			XMStoreFloat3(&transStored, trans);
			
			if ( MathHelper::AlmostSame(transStored.z,2.0f))
			{
				tempWorld = tempWorld * XMMatrixRotationAxis(XMVectorSet(0, 0.0f, 1.0f, 1.0f), DirectX::XM_PIDIV2);
				XMStoreFloat4x4(&ro.World, tempWorld);
			}
		}
	}
	if (d3dUtil::IsKeyDown('A'))
	{
		for (int i = 0; i != RenderObjects.size(); i++)
		{
			RenderObject& ro = RenderObjects[i];
			XMMATRIX tempWorld = XMLoadFloat4x4(&ro.World);
			XMVECTOR trans;
			XMVECTOR scale;
			XMVECTOR rot;
			XMMatrixDecompose(&scale, &rot, &trans, tempWorld);
			XMFLOAT3 transStored;
			XMStoreFloat3(&transStored, trans);
			if (MathHelper::AlmostSame(transStored.x, 2.0f))
			{
				tempWorld = tempWorld * XMMatrixRotationAxis(XMVectorSet(1.0f, 0.0f, 0.0f, 1.0f), DirectX::XM_PIDIV2);
				XMStoreFloat4x4(&ro.World, tempWorld);
			}
		}
	}
}

void D3DApp::Draw(GameTimer& mTimer)
{
	ThrowIfFailed(mDirectCmdListAlloc->Reset());
	ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), mPSO.Get()));

	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mSwapChainBuffer[mCurrBackBuffer].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

	mCommandList->RSSetViewports(1, &mScreenViewport);
	mCommandList->RSSetScissorRects(1, &mScissorRect);

	mCommandList->ClearRenderTargetView(CurrentBackBufferView(), DirectX::Colors::Tomato, 0, nullptr);
	mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
	mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());
	ID3D12DescriptorHeap* DescriptorHeaps[] = { mCbvHeap.Get() };
	mCommandList->SetDescriptorHeaps(_countof(DescriptorHeaps), DescriptorHeaps);
	mCommandList->SetGraphicsRootSignature(mRootSignature.Get());
	auto passCbvHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(mCbvHeap->GetGPUDescriptorHandleForHeapStart());
	passCbvHandle.Offset(mPassCbOffset, m_cbvSrvDescriptorSize);
	mCommandList->SetGraphicsRootDescriptorTable(1, passCbvHandle);
	UINT matCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(MaterialConstants));
	for (int i = 0; i != RenderObjects.size(); i++)
	{
		RenderObject& ro = RenderObjects[i];
		mCommandList->IASetVertexBuffers(0, 1, &ro.Geometry->GetVertexBufferView());
		mCommandList->IASetIndexBuffer(&ro.Geometry->GetIndexBufferView());
		mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		auto CbvHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(mCbvHeap->GetGPUDescriptorHandleForHeapStart());
		CbvHandle.Offset(i, m_cbvSrvDescriptorSize);
		D3D12_GPU_VIRTUAL_ADDRESS matCBAddress = mMaterialConstantsBuffer->GetBuffer()->GetGPUVirtualAddress()
			+ ro.Mat->MatCBIndex * matCBByteSize;
		mCommandList->SetGraphicsRootDescriptorTable(0, CbvHandle);
		mCommandList->SetGraphicsRootConstantBufferView(2, matCBAddress);
		mCommandList->DrawInstanced(
			36,
			1, 0, 0);
	}

	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mSwapChainBuffer[mCurrBackBuffer].Get(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));
	
	ThrowIfFailed(mCommandList->Close());

	ID3D12CommandList* cmdsList[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsList), cmdsList);

	ThrowIfFailed(mSwapChain->Present(0, 0));
	mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

	FlushCommandQueue();
}

void D3DApp::UpdateMainPasCb(const GameTimer& mTimer)
{
	XMMATRIX view = XMLoadFloat4x4(&mView);
	XMMATRIX proj = XMLoadFloat4x4(&mProj);
	XMMATRIX viewProj = XMMatrixMultiply(view, proj);
	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
	XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
	XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

	XMStoreFloat4x4(&mMainPassCb.View, XMMatrixTranspose(view));
	XMStoreFloat4x4(&mMainPassCb.InvView, XMMatrixTranspose(invView));
	XMStoreFloat4x4(&mMainPassCb.Proj, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&mMainPassCb.InvProj, XMMatrixTranspose(invProj));
	XMStoreFloat4x4(&mMainPassCb.ViewProj, XMMatrixTranspose(viewProj));
	XMStoreFloat4x4(&mMainPassCb.InvViewProj, XMMatrixTranspose(invViewProj));

	mMainPassCb.EyePosW = mEyePos;
	mMainPassCb.RenderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
	mMainPassCb.InvRenderTargetSize = XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
	mMainPassCb.NearZ = 1.0f;
	mMainPassCb.FarZ = 1000.0f;
	mMainPassCb.TotalTime = mTimer.TotalTime();
	mMainPassCb.DeltaTime = mTimer.DeltaTime();

	mMainPassConstantBuffer->CopyData(0, mMainPassCb);
}

void D3DApp::UpdateMaterialCb(const GameTimer& mTimer)
{
	for (auto& e : mMaterials)
	{
		Material* mat = e.second.get();
		if (mat->NumFramesDirty > 0)
		{
			XMMATRIX matTransform = XMLoadFloat4x4(&mat->MatTransform);
		
			MaterialConstants matConstants;
			matConstants.DiffuseAlbedo = mat->DiffuseAlbedo;
			matConstants.FresnelR0 = mat->FresnelR0;
			matConstants.Roughness = mat->Roughness;

			mMaterialConstantsBuffer->CopyData(mat->MatCBIndex, matConstants);

			mat->NumFramesDirty--;
		}
	}
}

void D3DApp::OnMouseDown(WPARAM btnState, int x, int y)
{
	mLastMousePos.x = x;
	mLastMousePos.y = y;

	SetCapture(mhMainWnd);
}

void D3DApp::OnMouseUp(WPARAM btnState, int x, int y)
{
	ReleaseCapture();
}

void D3DApp::OnMouseMove(WPARAM btnState, int x, int y)
{
	if ((btnState & MK_LBUTTON) != 0)
	{
		// Make each pixel correspond to a quarter of a degree.
		float dx = XMConvertToRadians(0.25f * static_cast<float>(x - mLastMousePos.x));
		float dy = XMConvertToRadians(0.25f * static_cast<float>(y - mLastMousePos.y));

		// Update angles based on input to orbit camera around box.
		mTheta += dx;
		mPhi += dy;

		// Restrict the angle mPhi.
		mPhi = MathHelper::Clamp(mPhi, 0.1f, MathHelper::Pi - 0.1f);
	}
	else if ((btnState & MK_RBUTTON) != 0)
	{
		// Make each pixel correspond to 0.005 unit in the scene.
		float dx = 0.005f * static_cast<float>(x - mLastMousePos.x);
		float dy = 0.005f * static_cast<float>(y - mLastMousePos.y);

		// Update the camera radius based on input.
		mRadius += dx - dy;

		// Restrict the radius.
		mRadius = MathHelper::Clamp(mRadius, 3.0f, 15.0f);
	}

	mLastMousePos.x = x;
	mLastMousePos.y = y;
}
