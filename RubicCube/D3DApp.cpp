#include "D3DApp.h"
#include <WindowsX.h>
#include "GeometryGenerator.h"
#include <iostream>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"
#include "nv_helpers_dx12\DXRHelper.h"
#include "nv_helpers_dx12\BottomLevelASGenerator.h"
#include "nv_helpers_dx12\RaytracingPipelineGenerator.h"
#include "nv_helpers_dx12\RootSignatureGenerator.h"
#include <openxr\OpenXrManager.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);


LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam))
		return true;
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
	if (mDevice != nullptr)
		FlushCommandQueue();
}

void D3DApp::InitDirectX()
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

	HRESULT hardwareResult = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&mDevice));

	if (FAILED(hardwareResult))
	{
		Microsoft::WRL::ComPtr<IDXGIAdapter> pWarpAdapter;
		ThrowIfFailed(mdxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&pWarpAdapter)));
		ThrowIfFailed(D3D12CreateDevice(
			pWarpAdapter.Get(),
			D3D_FEATURE_LEVEL_11_0,
			IID_PPV_ARGS(&mDevice)));
	}

	mDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mFence));
	mRtvDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	mDsvDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	mCbvSrvDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msQualityLevels;
	msQualityLevels.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	msQualityLevels.SampleCount = 4;
	msQualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
	msQualityLevels.NumQualityLevels = 0;
	mDevice->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &msQualityLevels, sizeof(msQualityLevels));
	m4xMsaaQuality = msQualityLevels.NumQualityLevels;
	assert(m4xMsaaQuality > 0 && "Unexpected MSAA quality level.");



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
	CheckRaytracingSupport();
	CreateSwapChain();
	CreateRtvAndDsvDescriptorHeaps();
	CreateRenderTargetResources();
	CreateDepthStencilResource();
	CreateViewport();
	VertexInputLayout();
	CreateVertexAndIndexBuffer();
	CreateMaterials();
	CreateTextures();
	CreateRenderObjects();
	CreateConstantBufferViewsForRenderObjects();
	InitImgui();
	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);
	// Wait until initialization is complete.
	FlushCommandQueue();

	//ray trace thing

	CreateAccelerationStructures();

	ThrowIfFailed(mCommandList->Close());

	CreateRaytracingPipeline();
	CreateRaytracingOutputBuffer();
	CreateShaderResourceHeap();
	CreateShaderBindingTable();

	//vr
	InitVrHeadset();
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

void D3DApp::InitVrHeadset()
{
	openXrManager = new OpenXrManager(mDevice, mCommandQueue.Get());
	openXrManager->Run();
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
	sd.BufferCount = kSwapChainBufferCount;
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

	ThrowIfFailed(mDevice->CreateDescriptorHeap(
		&rtvHeapDesc, IID_PPV_ARGS(mRtvHeap.GetAddressOf()
		)));
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
	dsvHeapDesc.NumDescriptors = 1;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	dsvHeapDesc.NodeMask = 0;
	ThrowIfFailed(mDevice->CreateDescriptorHeap(
		&dsvHeapDesc, IID_PPV_ARGS(mDsvHeap.GetAddressOf()
		)));

}

void D3DApp::CreateRenderTargetResources()
{
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHeapHandle(
		mRtvHeap->GetCPUDescriptorHandleForHeapStart()
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
		{"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT,0,12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
	};

	D3D12_INPUT_ELEMENT_DESC VertexDesc2[] = {
		{"POSITION",0, DXGI_FORMAT_R32G32B32_FLOAT, 0,0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"TANGENT",0, DXGI_FORMAT_R32G32B32_FLOAT, 0,12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"NORMAL",0, DXGI_FORMAT_R32G32B32_FLOAT, 0,24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"TEXTURE",0, DXGI_FORMAT_R32G32_FLOAT, 0,36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"TEXTURE",1, DXGI_FORMAT_R32G32_FLOAT, 0,44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"COLOR",0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,52, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
	};

	mSkyVertex = {
		{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
	};
}

void D3DApp::CreateVertexAndIndexBuffer()
{

	Vertex vertices[] = {
	//right
	{XMFLOAT3(-1.0f,-1.0f,-1.0f), XMFLOAT4(Colors::Red), XMFLOAT3(-1.0f, 0.0f, 0.0f), XMFLOAT2(0.0f, 0.25f)}, //bottom right
	{XMFLOAT3(-1.0f,-1.0f, 1.0f), XMFLOAT4(Colors::Red), XMFLOAT3(-1.0f, 0.0f, 0.0f), XMFLOAT2(0.0f, 0.0f)}, //top right
	{XMFLOAT3(-1.0f, 1.0f, 1.0f), XMFLOAT4(Colors::Red), XMFLOAT3(-1.0f, 0.0f, 0.0f), XMFLOAT2(0.25f, 0.0f)}, //top left
	{XMFLOAT3(-1.0f, 1.0f,-1.0f), XMFLOAT4(Colors::Red), XMFLOAT3(-1.0f, 0.0f, 0.0f), XMFLOAT2(0.25f, 0.25f)}, //bottom left
	//front
	{XMFLOAT3(1.0f, 1.0f,-1.0f),  XMFLOAT4(Colors::Green), XMFLOAT3(0.0f, 0.0f, -1.0f), XMFLOAT2(0.5f, 0.0f)}, //top left
	{XMFLOAT3(-1.0f,-1.0f,-1.0f), XMFLOAT4(Colors::Green), XMFLOAT3(0.0f, 0.0f, -1.0f), XMFLOAT2(0.25f, 0.25f)}, //bottom right
	{XMFLOAT3(-1.0f, 1.0f,-1.0f), XMFLOAT4(Colors::Green), XMFLOAT3(0.0f, 0.0f, -1.0f), XMFLOAT2(0.25f, 0.0f)}, //top right
	{XMFLOAT3(1.0f,-1.0f,-1.0f),  XMFLOAT4(Colors::Green), XMFLOAT3(0.0f, 0.0f, -1.0f), XMFLOAT2(0.5f, 0.25f)}, //bottom left
	//bottom
	{XMFLOAT3(1.0f,-1.0f, 1.0f),  XMFLOAT4(Colors::Yellow), XMFLOAT3(0.0f, -1.0f, 0.0f), XMFLOAT2(0.5f, 0.25f)}, //bottom right
	{XMFLOAT3(-1.0f,-1.0f,-1.0f), XMFLOAT4(Colors::Yellow), XMFLOAT3(0.0f, -1.0f, 0.0f), XMFLOAT2(0.75f, 0.0f)}, //top left
	{XMFLOAT3(1.0f,-1.0f,-1.0f),  XMFLOAT4(Colors::Yellow), XMFLOAT3(0.0f, -1.0f, 0.0f), XMFLOAT2(0.5f, 0.0f)}, //top right
	{XMFLOAT3(-1.0f,-1.0f, 1.0f), XMFLOAT4(Colors::Yellow), XMFLOAT3(0.0f, -1.0f, 0.0f), XMFLOAT2(0.75f, 0.25f)}, //bottom left
	//left
	{XMFLOAT3(1.0f, 1.0f, 1.0f),  XMFLOAT4(Colors::Orange), XMFLOAT3(1.0f, 0.0f, 0.0f), XMFLOAT2(0.75f, 0.25f)}, //bottom right
	{XMFLOAT3(1.0f,-1.0f,-1.0f),  XMFLOAT4(Colors::Orange), XMFLOAT3(1.0f, 0.0f, 0.0f), XMFLOAT2(1.0f, 0.0f)}, //top left
	{XMFLOAT3(1.0f, 1.0f,-1.0f),  XMFLOAT4(Colors::Orange), XMFLOAT3(1.0f, 0.0f, 0.0f), XMFLOAT2(0.75f, 0.0f)}, //top right
	{XMFLOAT3(1.0f,-1.0f, 1.0f),  XMFLOAT4(Colors::Orange), XMFLOAT3(1.0f, 0.0f, 0.0f), XMFLOAT2(1.0f, 0.25f)}, //bottom left
	//top
	{XMFLOAT3(1.0f, 1.0f, 1.0f),  XMFLOAT4(Colors::White), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(0.0f, 0.25f)}, //top left
	{XMFLOAT3(1.0f, 1.0f,-1.0f),  XMFLOAT4(Colors::White), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(0.0f, 0.5f)}, //bottom left
	{XMFLOAT3(-1.0f, 1.0f,-1.0f), XMFLOAT4(Colors::White), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(0.25f, 0.5f)}, //bottom right
	{XMFLOAT3(-1.0f, 1.0f, 1.0f), XMFLOAT4(Colors::White), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(0.25f, 0.25f)}, //top right
	//back
	{XMFLOAT3(-1.0f, 1.0f, 1.0f), XMFLOAT4(Colors::Blue), XMFLOAT3(0.0f, 0.0f, 1.0f), XMFLOAT2(0.25f, 0.5f)}, //bottom left
	{XMFLOAT3(-1.0f,-1.0f, 1.0f), XMFLOAT4(Colors::Blue), XMFLOAT3(0.0f, 0.0f, 1.0f), XMFLOAT2(0.25f, 0.25f)}, //top left
	{XMFLOAT3(1.0f,-1.0f, 1.0f),  XMFLOAT4(Colors::Blue), XMFLOAT3(0.0f, 0.0f, 1.0f), XMFLOAT2(0.5f, 0.25f)}, //top right
	{XMFLOAT3(1.0f, 1.0f, 1.0f),  XMFLOAT4(Colors::Blue), XMFLOAT3(0.0f, 0.0f, 1.0f), XMFLOAT2(0.5f, 0.5f)}, //bottom right
	};

	const UINT64 vbByteSize = 24 * sizeof(Vertex);

	//indices
	std::uint32_t indices[] = {
		// right face
		0, 1, 2,
		0, 2, 3,
		// front face
		4, 6, 5,
		4, 5, 7,
		// bottom face
		8, 10, 9,
		8, 9, 11,
		// left face
		12, 14, 13,
		12, 13, 15,
		// top face
		19, 18, 16,
		16, 18, 17,
		// back face
		20, 22, 21,
		20, 23, 22
	};

	const UINT ibByteSize = 36 * sizeof(std::uint32_t);

	std::unique_ptr<Geometry> Cube(new Geometry());
	Cube->vertexBuffer = d3dUtil::CreateDefaultBuffer(mDevice, mCommandList.Get(), vertices, vbByteSize, Cube->vertexUploadBuffer);
	Cube->indexBuffer = d3dUtil::CreateDefaultBuffer(mDevice, mCommandList.Get(), indices, ibByteSize, Cube->indexUploadBuffer);
	Cube->ibFormat = DXGI_FORMAT_R32_UINT;
	Cube->ibByteSize = ibByteSize;
	Cube->strideInBytes = sizeof(Vertex);
	Cube->vbByteSize = vbByteSize;
	Cube->indexCount = 36;

	BoundingBox bounds;
	XMFLOAT3 vMinf3(-1.0f, -1.0f, -1.0f);
	XMFLOAT3 vMaxf3(1.0f, 1.0f, 1.0f);
	XMVECTOR vMin = XMLoadFloat3(&vMinf3);
	XMVECTOR vMax = XMLoadFloat3(&vMaxf3);
	XMStoreFloat3(&bounds.Center, 0.5f * (vMin + vMax));
	XMStoreFloat3(&bounds.Extents, 0.5f * (vMax - vMin));

	Cube->bounds = bounds;
	geometries.emplace("Cube", std::move(Cube));
	GeometryGenerator geometryGenerator;
	GeometryGenerator::MeshData sphereMesh = geometryGenerator.CreateSphere(100, 5, 5);

	std::unique_ptr<Geometry> sphereGeo = std::make_unique<Geometry>();
	std::vector<Vertex> sphereVertices(sphereMesh.Vertices.size());
	for (int i = 0; i != sphereVertices.size(); i++)
	{
		sphereVertices[i].Pos = sphereMesh.Vertices[i].Position;
		sphereVertices[i].Normal = sphereMesh.Vertices[i].Normal;
		sphereVertices[i].Color = XMFLOAT4(Colors::White);
		sphereVertices[i].TexC = sphereMesh.Vertices[i].TexC;
	}

	float sphereVbByteSize = sphereMesh.Vertices.size() * sizeof(Vertex);
	float sphereIbByteSize = sphereMesh.GetIndices16().size() * sizeof(std::uint16_t);

	sphereGeo->vertexBuffer = d3dUtil::CreateDefaultBuffer(mDevice, mCommandList.Get(), sphereVertices.data(), sphereVbByteSize, sphereGeo->vertexUploadBuffer);
	sphereGeo->indexBuffer = d3dUtil::CreateDefaultBuffer(mDevice, mCommandList.Get(), sphereMesh.GetIndices16().data(), sphereIbByteSize, sphereGeo->indexUploadBuffer);
	sphereGeo->ibFormat = DXGI_FORMAT_R16_UINT;
	sphereGeo->ibByteSize = sphereIbByteSize;
	sphereGeo->strideInBytes = sizeof(Vertex);
	sphereGeo->vbByteSize = sphereVbByteSize;
	sphereGeo->indexCount = sphereMesh.GetIndices16().size();
	geometries.emplace("Sphere", std::move(sphereGeo));

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



	mvsByteCode = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr,
		"VS", "vs_5_0");
	mpsByteCode = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr,
		"PS", "ps_5_0");

	mShaders["SkyVS"] = d3dUtil::CompileShader(L"Shaders\\Sky.hlsl", nullptr, "VS", "vs_5_0");
	mShaders["SkyPS"] = d3dUtil::CompileShader(L"Shaders\\Sky.hlsl", nullptr, "PS", "ps_5_0");

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

	ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPSO)));
	
	mPSOs["opaque"] = mPSO;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC skyPsoDesc = psoDesc;
	skyPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	skyPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	skyPsoDesc.pRootSignature = mRootSignature.Get();
	skyPsoDesc.InputLayout = { mSkyVertex.data(), (UINT)mSkyVertex.size() };
	skyPsoDesc.VS = { reinterpret_cast<BYTE*>(mShaders["SkyVS"]->GetBufferPointer()), mShaders["SkyVS"]->GetBufferSize() };
	skyPsoDesc.PS = { reinterpret_cast<BYTE*>(mShaders["SkyPS"]->GetBufferPointer()), mShaders["SkyPS"]->GetBufferSize() };

	ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&skyPsoDesc, IID_PPV_ARGS(&mPSOSky)));

	mPSOs["sky"] = mPSOSky;
}

void D3DApp::CreateRenderObjects()
{
	UINT cbIndex = 0;

	std::unique_ptr<RenderObject> renderObject = std::make_unique<RenderObject>();
	renderObject->constantBufferIndex = cbIndex;
	renderObject->geometry = geometries["Cube"].get();
	renderObject->indexCount = geometries["Cube"]->indexCount;
	renderObject->material = materials["grass"].get();
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


	XMStoreFloat4x4(&renderObject->world,
		world);
	renderObject->constantBufferIndex = cbIndex;

	mRubikCube.Initialize(cbIndex);
	//mRubikCube.Scramble("U1F2B'3");
	std::vector<std::unique_ptr<RenderObject>> Cubes = mRubikCube.GetRenderObjects();
	for (int i = 0; i < Cubes.size(); i++)
	{
		mOpaqueObjects.push_back(Cubes[i].get());
		mAllRenderObjects.push_back(std::move(Cubes[i]));
	}

	// controller meshes

	renderObject = std::make_unique<RenderObject>();

	renderObject->constantBufferIndex = cbIndex;
	renderObject->geometry = geometries["Cube"].get();
	renderObject->indexCount = geometries["Cube"]->indexCount;
	renderObject->material = materials["grass"].get();
	renderObject->bounds = renderObject->geometry->bounds;
	renderObject->name = "Left Controller";

	world = XMMatrixTranslation(10, 10, 0) * XMMatrixScaling(0.1f, 0.1f, 0.1f);
	XMStoreFloat4x4(&renderObject->world, world);
	mOpaqueObjects.push_back(renderObject.get());
	mLeftController = renderObject.get();
	mAllRenderObjects.push_back(std::move(renderObject));

	cbIndex++;

	renderObject = std::make_unique<RenderObject>();

	renderObject->constantBufferIndex = cbIndex;
	renderObject->geometry = geometries["Cube"].get();
	renderObject->indexCount = geometries["Cube"]->indexCount;
	renderObject->material = materials["grass"].get();
	renderObject->bounds = renderObject->geometry->bounds;
	renderObject->name = "Right Controller";

	world = XMMatrixTranslation(5, 10, -10) * XMMatrixScaling(0.1f, 0.1f, 0.1f);
	XMStoreFloat4x4(&renderObject->world, world);
	mOpaqueObjects.push_back(renderObject.get());
	mRightController = renderObject.get();
	mAllRenderObjects.push_back(std::move(renderObject));

	//sphere
	cbIndex++;

	renderObject = std::make_unique<RenderObject>();

	renderObject->geometry = geometries["Sphere"].get();
	renderObject->indexCount = geometries["Sphere"]->indexCount;
	renderObject->constantBufferIndex = cbIndex;
	renderObject->material = materials["grass"].get();
	//renderObject.World = MathHelper::Identity4x4();
	world = XMMatrixTranslation(0, 0, 0) * XMMatrixScaling(0.5f, 0.5f, 0.5f);
	XMStoreFloat4x4(&renderObject->world,
		world);
	mSkyObjects.push_back(renderObject.get());
	mAllRenderObjects.push_back(std::move(renderObject));

}

void D3DApp::CreateConstantBufferViewsForRenderObjects()
{
	mObjectConstantsBuffer = new ConstantBuffer<ObjectConstants>(mDevice, mAllRenderObjects.size() );
	mMainPassConstantBuffer = new ConstantBuffer<PassConstant>(mDevice, 1);
	mMaterialConstantsBuffer = new ConstantBuffer<MaterialConstants>(mDevice, materials.size());
	UINT elementByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
	UINT passByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstant));
	UINT materialByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(MaterialConstants));


	D3D12_DESCRIPTOR_HEAP_DESC cbvHeapDesc;
	cbvHeapDesc.NumDescriptors = mAllRenderObjects.size() + 1 + materials.size() + mTextures.size() + 1 /*for imgui*/;
	cbvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	cbvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	cbvHeapDesc.NodeMask = 0;
	mPassCbOffset = mAllRenderObjects.size();
	mTextureOffset = mAllRenderObjects.size() + 1 + materials.size();
	mImGuiOffset = mAllRenderObjects.size() + 1 + materials.size() + mTextures.size();

	mDevice->CreateDescriptorHeap(&cbvHeapDesc, IID_PPV_ARGS(&mCbvHeap));
	//mUploadCBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedData));
	//mUploadPassBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedPassData));
	for (int i = 0; i != mAllRenderObjects.size(); i++)
	{
		ObjectConstants objConstants;
		objConstants.World = mAllRenderObjects[i]->world;
		//ObjectConstantsBuffer->CopyData(i, objConstants);
		//memcpy(&mMappedData[elementByteSize*i], &objConstants, elementByteSize);
		
		D3D12_GPU_VIRTUAL_ADDRESS cbAddress = mObjectConstantsBuffer->GetBuffer()->GetGPUVirtualAddress();
		cbAddress += i* elementByteSize;
		D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc;
		cbvDesc.BufferLocation = cbAddress;
		cbvDesc.SizeInBytes = elementByteSize;
		auto handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(mCbvHeap->GetCPUDescriptorHandleForHeapStart());
		handle.Offset(i, mCbvSrvDescriptorSize);

		mDevice->CreateConstantBufferView(
			&cbvDesc,
			handle
		);
	}
		
	auto handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(mCbvHeap->GetCPUDescriptorHandleForHeapStart());
	handle.Offset(mPassCbOffset, mCbvSrvDescriptorSize);
	D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc;
	cbvDesc.BufferLocation = mMainPassConstantBuffer->GetBuffer()->GetGPUVirtualAddress();
	cbvDesc.SizeInBytes = passByteSize;

	mDevice->CreateConstantBufferView(
		&cbvDesc,
		handle
	);

	CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(
		mCbvHeap->GetCPUDescriptorHandleForHeapStart()
	);

	auto woodCrateTex = mTextures["woodCrateTex"].get();
	hDescriptor.Offset(mTextureOffset, mCbvSrvDescriptorSize);
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = woodCrateTex->Resource->GetDesc().Format;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = woodCrateTex->Resource->GetDesc().MipLevels;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
	mDevice->CreateShaderResourceView(woodCrateTex->Resource.Get(), &srvDesc, hDescriptor);

	hDescriptor.Offset(1, mCbvSrvDescriptorSize);
	Texture* skyTex = mTextures["skyTex"].get();
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
	srvDesc.TextureCube.MostDetailedMip = 0;
	srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
	srvDesc.TextureCube.MipLevels = skyTex->Resource->GetDesc().MipLevels;
	srvDesc.Format = skyTex->Resource->GetDesc().Format;
	mDevice->CreateShaderResourceView(skyTex->Resource.Get(), &srvDesc, hDescriptor);
}

void D3DApp::CreateMaterials()
{
	std::unique_ptr<Material> grass = std::make_unique<Material>();
	grass->Name = "grass";
	grass->MatCBIndex = 0;
	grass->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	grass->FresnelR0 = XMFLOAT3(0.05f, 0.05f, 0.05f);
	grass->Roughness = 0.3f;
	grass->DiffuseSrvHeapIndex = 0;

	std::unique_ptr<Material> water = std::make_unique<Material>();
	water->Name = "water";
	water->MatCBIndex = 1;
	water->DiffuseAlbedo = XMFLOAT4(0.0f, 0.2f, 0.6f, 1.0f);
	water->FresnelR0 = XMFLOAT3(0.1f, 0.1f, 0.1f);
	water->DiffuseSrvHeapIndex = 0;

	std::unique_ptr<Material> sky = std::make_unique<Material>();
	sky->Name = "sky";
	sky->MatCBIndex = 2;
	sky->DiffuseAlbedo = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
	sky->FresnelR0 = XMFLOAT3(0.1f, 0.1f, 0.1f);
	sky->DiffuseSrvHeapIndex = 0;

	std::unique_ptr<Material> selectedCube = std::make_unique<Material>();
	selectedCube->Name = "selectedCube";
	selectedCube->MatCBIndex = 3;
	selectedCube->DiffuseAlbedo = XMFLOAT4(0.5f, 0.5f, 0.5f, 1.0f);
	selectedCube->FresnelR0 = XMFLOAT3(0.1f, 0.1f, 0.1f);
	selectedCube->DiffuseSrvHeapIndex = 0;


	materials["grass"] = std::move(grass);
	materials["water"] = std::move(water);
	materials["sky"] = std::move(sky);
	materials["selectedCube"] = std::move(selectedCube);
}

void D3DApp::CreateTextures()
{
	std::unique_ptr<Texture> woodCrateTex = std::make_unique<Texture>();
	woodCrateTex->Name = "woodCrateTex";
	woodCrateTex->Filename = L"Textures/rubicPallet.dds";
	ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(
		mDevice, mCommandList.Get(), woodCrateTex->Filename.c_str(),
		woodCrateTex->Resource, woodCrateTex->UploadHeap));

	std::unique_ptr<Texture> skyTex = std::make_unique<Texture>();
	skyTex->Name = "skyTex";
	skyTex->Filename = L"Textures/grasscube1024.dds";
	ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(
		mDevice, mCommandList.Get(), skyTex->Filename.c_str(),
		skyTex->Resource, skyTex->UploadHeap));

	mTextures[woodCrateTex->Name] = std::move(woodCrateTex);
	mTextures[skyTex->Name] = std::move(skyTex);
}

void D3DApp::InitImgui()
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

	ImGui::StyleColorsDark();

	ImGui_ImplWin32_Init(mhMainWnd);

	auto handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(mCbvHeap->GetCPUDescriptorHandleForHeapStart());
	handle.Offset(mImGuiOffset, mCbvSrvDescriptorSize);
	auto gpuHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(mCbvHeap->GetGPUDescriptorHandleForHeapStart());
	gpuHandle.Offset(mImGuiOffset, mCbvSrvDescriptorSize);
	ImGui_ImplDX12_Init(mDevice, 1,
		DXGI_FORMAT_R8G8B8A8_UNORM, mCbvHeap.Get(),
		handle,
		gpuHandle);
}

void D3DApp::RenderImgui()
{
	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	// 1. Show the big demo window (Most of the sample code is in ImGui::ShowDemoWindow()! You can browse its code to learn more about Dear ImGui!).
		ImGui::ShowDemoWindow();

	// 2. Show a simple window that we create ourselves. We use a Begin/End pair to create a named window.
	{
		static float f = 0.0f;
		static int counter = 0;

		ImGui::Begin("Rubik Controls", nullptr, 
			ImGuiWindowFlags_MenuBar | 
			ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoBringToFrontOnFocus);                          // Create a window called "Hello, world!" and append into it.

		ImGui::Text("[Left Mouse Button] Rotate Camera");
		ImGui::Text("[Right Mouse Button] Select Face");
		ImGui::Text("[Q] Rotate face anticlockwise");               
		ImGui::Text("[E] Rotate face clockwise");      
		if(ImGui::Button("Scramble"))
		{
			std::string randomScramble = mRubikCube.GenerateRandomScrambleString(20);
			std::cout << randomScramble << std::endl;
			mRubikCube.Scramble(randomScramble);
		}
		VictoryScreen(mRubikCube.GetVictory());

		ImGui::End();
	}

	// Rendering
	ImGui::Render();
}

void D3DApp::VictoryScreen(bool Enable)
{
	if (!Enable)
	{
		if (ImGui::IsPopupOpen("Victory!!!"))
			ImGui::CloseCurrentPopup();
		return;
	}
	ImGui::OpenPopup("Victory!!!");
	ImGui::BeginPopupModal("Victory!!!", NULL, ImGuiWindowFlags_AlwaysAutoResize);
	ImGui::Text("You have completed the Rubik cube");
	if (ImGui::Button("Restart"))
	{
		ImGui::CloseCurrentPopup();
		mRubikCube.SetVictory(false);
	}
	ImGui::SameLine(0, 140.0f);
	ImGui::Button("Quit");
	ImGui::EndPopup();
}


D3D12_CPU_DESCRIPTOR_HANDLE D3DApp::CurrentBackBufferView() const
{
	return CD3DX12_CPU_DESCRIPTOR_HANDLE(
		mRtvHeap->GetCPUDescriptorHandleForHeapStart(),
		mCurrBackBuffer,
		mRtvDescriptorSize
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
				openXrManager->Update();
				CalculateFrameStats();
				openXrManager->StartFrame();
				openXrManager->AcquireSwapchainImages();
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
	case WM_KEYDOWN:
		OnKeyDown(wParam);
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
	//XMVECTOR pos = XMVectorSet(mEyePos.x, mEyePos.y, mEyePos.z, 1.0f);
	XMVECTOR pos = XMVectorSet(0, 0, 0, 1.0f);
	XMVECTOR target = XMVectorZero();
	XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
	XMMATRIX view;
	if (openXrManager->HasSession())
	{
		XMStoreFloat4x4(&mProj, XMMatrixPerspectiveFovRH(0.6f * MathHelper::Pi, 800.0f / 600.0f, 0.05f, 1000.0f));

		XMVECTOR XRPos;
		XMVECTOR temp;
		XMMatrixDecompose(&temp, &temp, &XRPos, openXrManager->GetViewMatrix(0));
		XMStoreFloat3(&mEyePos, XRPos);
		view = openXrManager->GetViewMatrix(1);
		view = XMMatrixInverse(&XMMatrixDeterminant(view), view);
		//view = XMMatrixMultiply(view, XMMatrixLookAtLH(pos, target, up));
	}
	else
	{
		pos = XMVectorSet(mEyePos.x, mEyePos.y, mEyePos.z, 1.0f);

		view = XMMatrixLookAtLH(pos, target, up);
	}
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
	UpdateRubikCubeInstances();

	UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
	for (int i = 0; mAllRenderObjects.size() != i; i++)
	{
		ObjectConstants objConstants;
		RenderObject& ro = *mAllRenderObjects[i];
		XMMATRIX objWorld = XMLoadFloat4x4(&ro.world);
		XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(objWorld));
		mObjectConstantsBuffer->CopyData(i, objConstants);
	}

	for (int i = 0; mSkyObjects.size() != i; i++)
	{
		ObjectConstants objConstants;
		RenderObject& ro = *mSkyObjects[i];
		XMMATRIX objWorld = XMLoadFloat4x4(&ro.world);
		XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(objWorld));
		mObjectConstantsBuffer->CopyData(mAllRenderObjects.size() + i, objConstants);
	}

	if (d3dUtil::IsKeyDown('W'))
	{
		for (int i = 0; i != mAllRenderObjects.size(); i++)
		{
			RenderObject& ro = *mAllRenderObjects[i];
			XMMATRIX tempWorld = XMLoadFloat4x4(&ro.world);
			XMVECTOR trans;
			XMVECTOR scale;
			XMVECTOR rot;
			XMMatrixDecompose(&scale, &rot, &trans, tempWorld);
			XMFLOAT3 transStored;
			XMStoreFloat3(&transStored, trans);
			
			if ( MathHelper::AlmostSame(transStored.z,2.0f))
			{
				tempWorld = tempWorld * XMMatrixRotationAxis(XMVectorSet(0, 0.0f, 1.0f, 1.0f), DirectX::XM_PIDIV2 * 0.001f);
				XMStoreFloat4x4(&ro.world, tempWorld);
			}
		}
	}
	if (d3dUtil::IsKeyDown('A'))
	{
		for (int i = 0; i != mAllRenderObjects.size(); i++)
		{
			RenderObject& ro = *mAllRenderObjects[i];
			XMMATRIX tempWorld = XMLoadFloat4x4(&ro.world);
			XMVECTOR trans;
			XMVECTOR scale;
			XMVECTOR rot;
			XMMatrixDecompose(&scale, &rot, &trans, tempWorld);
			XMFLOAT3 transStored;
			XMStoreFloat3(&transStored, trans);
			if (MathHelper::AlmostSame(transStored.x, 2.0f))
			{
				tempWorld = tempWorld * XMMatrixRotationAxis(XMVectorSet(1.0f, 0.0f, 0.0f, 1.0f), DirectX::XM_PIDIV2);
				XMStoreFloat4x4(&ro.world, tempWorld);
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
	
	if (mRaster)
	{
		mCommandList->ClearRenderTargetView(CurrentBackBufferView(), DirectX::Colors::Tomato, 0, nullptr);
		mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
		mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());
		ID3D12DescriptorHeap* DescriptorHeaps[] = { mCbvHeap.Get()};
		mCommandList->SetDescriptorHeaps(_countof(DescriptorHeaps), DescriptorHeaps);
		mCommandList->SetGraphicsRootSignature(mRootSignature.Get());
		auto passCbvHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(mCbvHeap->GetGPUDescriptorHandleForHeapStart());
		passCbvHandle.Offset(mPassCbOffset, mCbvSrvDescriptorSize);
		mCommandList->SetGraphicsRootDescriptorTable(1, passCbvHandle);
		//mCommandList->SetPipelineState(mPSOs["sky"].Get());

		UINT matCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(MaterialConstants));
		for (int i = 0; i != mOpaqueObjects.size(); i++)
		{
			RenderObject* ro = mOpaqueObjects[i];
			mCommandList->IASetVertexBuffers(0, 1, &ro->geometry->GetVertexBufferView());
			mCommandList->IASetIndexBuffer(&ro->geometry->GetIndexBufferView());
			mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			auto CbvHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(mCbvHeap->GetGPUDescriptorHandleForHeapStart());
			CbvHandle.Offset(i, mCbvSrvDescriptorSize);
			D3D12_GPU_VIRTUAL_ADDRESS matCBAddress = mMaterialConstantsBuffer->GetBuffer()->GetGPUVirtualAddress()
				+ ro->GetMatIndex() * matCBByteSize;
			mCommandList->SetGraphicsRootDescriptorTable(0, CbvHandle);
			mCommandList->SetGraphicsRootConstantBufferView(2, matCBAddress);

			CD3DX12_GPU_DESCRIPTOR_HANDLE tex(
				mCbvHeap->GetGPUDescriptorHandleForHeapStart());
			tex.Offset(mTextureOffset + ro->material->DiffuseSrvHeapIndex, mCbvSrvDescriptorSize);
			mCommandList->SetGraphicsRootDescriptorTable(3, tex);
			mCommandList->DrawIndexedInstanced(ro->indexCount, 1, 0, 0, 0);
		}

		mCommandList->SetPipelineState(mPSOs["sky"].Get());

		RenderObject& sky = *mSkyObjects[0];
		mCommandList->IASetVertexBuffers(0, 1, &sky.geometry->GetVertexBufferView());
		mCommandList->IASetIndexBuffer(&sky.geometry->GetIndexBufferView());
		mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		auto CbvHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(mCbvHeap->GetGPUDescriptorHandleForHeapStart());
		CbvHandle.Offset(mOpaqueObjects.size(), mCbvSrvDescriptorSize);
		D3D12_GPU_VIRTUAL_ADDRESS matCBAddress = mMaterialConstantsBuffer->GetBuffer()->GetGPUVirtualAddress()
			+ sky.material->MatCBIndex * matCBByteSize;
		mCommandList->SetGraphicsRootDescriptorTable(0, CbvHandle);
		mCommandList->SetGraphicsRootConstantBufferView(2, matCBAddress);

		CD3DX12_GPU_DESCRIPTOR_HANDLE tex(
			mCbvHeap->GetGPUDescriptorHandleForHeapStart());
		tex.Offset(mTextureOffset + sky.material->DiffuseSrvHeapIndex, mCbvSrvDescriptorSize);
		mCommandList->SetGraphicsRootDescriptorTable(3, tex);
		CD3DX12_GPU_DESCRIPTOR_HANDLE skyTexDescriptor(mCbvHeap->GetGPUDescriptorHandleForHeapStart());
		skyTexDescriptor.Offset(mTextureOffset + 1, mCbvSrvDescriptorSize);
		mCommandList->SetGraphicsRootDescriptorTable(4, skyTexDescriptor);
		mCommandList->DrawIndexedInstanced(sky.indexCount, 1, 0, 0, 0);
		RenderImgui();
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), mCommandList.Get());
	}
	else
	{
		CreateTopLevelAS(mInstances, true);
		mCommandList->ClearRenderTargetView(CurrentBackBufferView(), DirectX::Colors::Tomato, 0, nullptr);
		mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
		mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());
		ID3D12DescriptorHeap* DescriptorHeaps[] = { mSrvUavHeap.Get() };
		mCommandList->SetDescriptorHeaps(_countof(DescriptorHeaps), DescriptorHeaps);

		CD3DX12_RESOURCE_BARRIER transition = CD3DX12_RESOURCE_BARRIER::Transition(mOutputResource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		mCommandList->ResourceBarrier(1, &transition);

		D3D12_DISPATCH_RAYS_DESC desc = {};
		uint32_t rayGenerationSectionSizeInBytes = mSbtHelper.GetRayGenSectionSize();
		desc.RayGenerationShaderRecord.StartAddress = mSbtStorage->GetGPUVirtualAddress();
		desc.RayGenerationShaderRecord.SizeInBytes = rayGenerationSectionSizeInBytes;

		uint32_t missSectionSizeInBytes = mSbtHelper.GetMissSectionSize();
		desc.MissShaderTable.StartAddress =
			mSbtStorage->GetGPUVirtualAddress() + rayGenerationSectionSizeInBytes;
		desc.MissShaderTable.SizeInBytes = missSectionSizeInBytes;
		desc.MissShaderTable.StrideInBytes = mSbtHelper.GetMissEntrySize();

		uint32_t hitGroupsSectionSize = mSbtHelper.GetHitGroupSectionSize();
		desc.HitGroupTable.StartAddress = mSbtStorage->GetGPUVirtualAddress() +
			rayGenerationSectionSizeInBytes +
			missSectionSizeInBytes; //needs to be 64 alligned to make commandlist happy
		desc.HitGroupTable.SizeInBytes = hitGroupsSectionSize;
		desc.HitGroupTable.StrideInBytes = mSbtHelper.GetHitGroupEntrySize();

		desc.Width = mClientWidth;
		desc.Height = mClientHeight;
		desc.Depth = 1;

		mCommandList->SetPipelineState1(mRtStateObject.Get());
		mCommandList->DispatchRays(&desc);

		transition = CD3DX12_RESOURCE_BARRIER::Transition(
			mOutputResource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
		mCommandList->ResourceBarrier(1, &transition);
		
		transition = CD3DX12_RESOURCE_BARRIER::Transition(
			mSwapChainBuffer[mCurrBackBuffer].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
		mCommandList->ResourceBarrier(1, &transition);

		mCommandList->CopyResource(mSwapChainBuffer[mCurrBackBuffer].Get(), mOutputResource.Get());

		transition = CD3DX12_RESOURCE_BARRIER::Transition(
			mSwapChainBuffer[mCurrBackBuffer].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
		mCommandList->ResourceBarrier(1, &transition);
	}


	openXrManager->SetCurrentSwapchainImage(mSwapChainBuffer[mCurrBackBuffer].Get(), mDepthStencilBuffer.Get(), mCommandList.Get());

	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mSwapChainBuffer[mCurrBackBuffer].Get(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

	ThrowIfFailed(mCommandList->Close());

	ID3D12CommandList* cmdsList[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsList), cmdsList);

	ThrowIfFailed(mSwapChain->Present(0, 0));
	mCurrBackBuffer = (mCurrBackBuffer + 1) % kSwapChainBufferCount;

	FlushCommandQueue();
	
	openXrManager->EndFrame();
}

void D3DApp::UpdateMainPasCb(const GameTimer& mTimer)
{
	XMMATRIX view = XMLoadFloat4x4(&mView);
	XMMATRIX proj = XMLoadFloat4x4(&mProj);
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

	mMainPassCb.eyePosW = mEyePos;
	mMainPassCb.renderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
	mMainPassCb.invRenderTargetSize = XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
	mMainPassCb.nearZ = 1.0f;
	mMainPassCb.farZ = 1000.0f;
	mMainPassCb.totalTime = mTimer.TotalTime();
	mMainPassCb.deltaTime = mTimer.DeltaTime();
	mMainPassCb.ambientLight = { 0.25f, 0.25f, 0.35f, 0.1f };

	XMFLOAT3 myLightDir; XMStoreFloat3(&myLightDir, XMVector3Normalize(-XMLoadFloat3(&mEyePos)));

	mMainPassCb.lights[0].Direction = { 0.57735f, -0.57735f, 0.57735f };
	mMainPassCb.lights[0].Strength = { 0.6f, 0.6f, 0.6f };
	mMainPassCb.lights[1].Direction = { -0.57735f, -0.57735f, 0.57735f };
	mMainPassCb.lights[1].Strength = { 0.3f, 0.3f, 0.3f };
	mMainPassCb.lights[2].Direction = { 0.0f, -0.707f, -0.707f };
	mMainPassCb.lights[2].Strength = { 0.15f, 0.15f, 0.15f };

	mMainPassConstantBuffer->CopyData(0, mMainPassCb);
}

void D3DApp::UpdateMaterialCb(const GameTimer& mTimer)
{
	for (auto& e : materials)
	{
		Material* mat = e.second.get();
		if (mat->NumFramesDirty > 0)
		{
			XMMATRIX matTransform = XMLoadFloat4x4(&mat->MatTransform);
		
			MaterialConstants matConstants;
			matConstants.diffuseAlbedo = mat->DiffuseAlbedo;
			matConstants.fresnelR0 = mat->FresnelR0;
			matConstants.roughness = mat->Roughness;

			mMaterialConstantsBuffer->CopyData(mat->MatCBIndex, matConstants);

			mat->NumFramesDirty--;
		}
	}
}

void D3DApp::UpdateRubikCubeInstances()
{
	std::vector<Cube*> cubes = mRubikCube.GetAllCubies();
	for (int i = 0; i != mInstances.size(); i++)
	{
		mInstances[i].second = XMLoadFloat4x4(&cubes[i]->world);
	}
}

void D3DApp::OnMouseDown(WPARAM btnState, int x, int y)
{
	if ((btnState & MK_LBUTTON) != 0)
	{
		mLastMousePos.x = x;
		mLastMousePos.y = y;

		SetCapture(mhMainWnd);
	}
	else if ((btnState & MK_RBUTTON) != 0)
	{
		Pick(x, y);
	}
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
		mRadius = MathHelper::Clamp(mRadius, 3.0f, 500.0f);
	}

	mLastMousePos.x = x;
	mLastMousePos.y = y;
}

void D3DApp::OnKeyDown(WPARAM btnState)
{
	if (d3dUtil::IsKeyDown('Q'))
	{
		mRubikCube.Rotate(RubikCube::Clockwise);
	}
	else
	if (d3dUtil::IsKeyDown('E'))
	{
		mRubikCube.Rotate(RubikCube::Anticlockwise);
	}
	else
	if (d3dUtil::IsKeyDown('G'))
	{
		mRubikCube.CheckWinCondition();
	}
	else 
	if(d3dUtil::IsKeyDown('R'))
	{
		mRaster = !mRaster;
	}
}

void D3DApp::Pick(int sx, int sy)
{
	mRubikCube.DeselectCubes();
	
	XMFLOAT4X4 P = mProj;

	float vx = (+2.0f * sx / mClientWidth - 1.0f) / P(0, 0);
	float vy = (-2.0f * sy / mClientHeight + 1.0f) / P(1, 1);



	XMMATRIX V = XMLoadFloat4x4(&mView);
	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(V), V);

	RenderObject* closestObject = nullptr;
	float closestDistance = INFINITE;
	float tmin = 0.0f;
	for (auto ri : mOpaqueObjects)
	{
		auto geo = ri->geometry;


		XMMATRIX W = XMLoadFloat4x4(&ri->world);
		XMMATRIX invWorld = XMMatrixInverse(&XMMatrixDeterminant(W), W);

		XMMATRIX toLocal = XMMatrixMultiply(invView, invWorld);
		XMVECTOR rayOrigin = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
		XMVECTOR rayDir = XMVectorSet(vx, vy, 1.0f, 0.0f);
		rayOrigin = XMVector3TransformCoord(rayOrigin, toLocal);
		rayDir = XMVector3TransformNormal(rayDir, toLocal);

		rayDir = XMVector3Normalize(rayDir);

		
		if (ri->bounds.Intersects(rayOrigin, rayDir, tmin))
		{
			if (tmin < closestDistance)
			{
				closestDistance = tmin;
				closestObject = ri;
			}
		}
		
	}

	if (closestObject)
	{
		std::cout << "Hit cube: " << closestObject->name << std::endl;
		auto cub = static_cast<Cube*>(closestObject);
		mRubikCube.GetAdjecantCubes(cub->idx, cub->idy, cub->idz);
	}
}

std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> D3DApp::GetStaticSamplers()
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

	return {pointWrap, pointClamp, linearWrap, linearClamp, anisotropicWrap, anisotropicClamp};
}

void D3DApp::CheckRaytracingSupport()
{
	D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
	ThrowIfFailed(mDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,
		&options5, sizeof(options5)));
	if (options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0)
	{
		throw std::runtime_error("Raytracing not supported on device");
	}
}

void D3DApp::UpdateHandPosition(Hand hand, XMFLOAT4X4 world)
{
	if (hand == Hand::left)
	{
		mLeftController->world = world;
	}
	else
	{
		mRightController->world = world;
	}
}

void* D3DApp::GetXrSwapchainImage(XrSwapchain swapchain, uint32_t index)
{
	ID3D12Resource* image = swapchainImagesMap[swapchain].second[index].texture;
	D3D12_RESOURCE_STATES state = swapchainImagesMap[swapchain].first == SwapchainType::COLOR ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_DEPTH_WRITE;
	return image;
}

XrSwapchainImageBaseHeader* D3DApp::AllocateSwapchainImageData(XrSwapchain swapchain, SwapchainType type, uint32_t count)
{
	swapchainImagesMap[swapchain].first = type;
	swapchainImagesMap[swapchain].second.resize(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
	return reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchainImagesMap[swapchain].second.data());
}

AccelerationStructureBuffers D3DApp::CreateBottomLevelAS(std::vector<std::pair<ComPtr<ID3D12Resource>, uint32_t>> vertexBuffers,
	std::vector<std::pair<ComPtr<ID3D12Resource>, uint32_t>> indexBuffers)
{
	nv_helpers_dx12::BottomLevelASGenerator bottomLevelAS;
	for (size_t i = 0; i < vertexBuffers.size(); i++) {
		// for (const auto &buffer : vVertexBuffers) {
		if (i < indexBuffers.size() && indexBuffers[i].second > 0)
			bottomLevelAS.AddVertexBuffer(vertexBuffers[i].first.Get(), 0,
				vertexBuffers[i].second, sizeof(Vertex),
				indexBuffers[i].first.Get(), 0,
				indexBuffers[i].second, nullptr, 0, true);

		else
			bottomLevelAS.AddVertexBuffer(vertexBuffers[i].first.Get(), 0,
				vertexBuffers[i].second, sizeof(Vertex), 0,
				0);
	}

	UINT64 scratchSizeInBytes = 0;
	UINT64 resultSizeInBytes = 0;

	bottomLevelAS.ComputeASBufferSizes(mDevice, false, &scratchSizeInBytes, &resultSizeInBytes);
	AccelerationStructureBuffers buffers;
	buffers.pScratch = nv_helpers_dx12::CreateBuffer(mDevice, scratchSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, nv_helpers_dx12::kDefaultHeapProps);
	buffers.pResult = nv_helpers_dx12::CreateBuffer(mDevice, resultSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nv_helpers_dx12::kDefaultHeapProps);
	bottomLevelAS.Generate(mCommandList.Get(), buffers.pScratch.Get(), buffers.pResult.Get(), false, nullptr);
	return buffers;
}

void D3DApp::CreateTopLevelAS(std::vector<std::pair<ComPtr<ID3D12Resource>, DirectX::XMMATRIX>>& instances, bool updateOnly)
{
	if (!updateOnly)
	{
		for (size_t i = 0; i < instances.size(); i++)
		{
			mTopLevelAsGenerator.AddInstance(instances[i].first.Get(), instances[i].second, static_cast<UINT>(i), static_cast<UINT>(i));
		}

		UINT64 scratchSize, resultSize, instanceDescSize;
		mTopLevelAsGenerator.ComputeASBufferSizes(mDevice, true, &scratchSize, &resultSize, &instanceDescSize);

		mTopLevelASBuffers.pScratch = nv_helpers_dx12::CreateBuffer(
			mDevice, scratchSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			nv_helpers_dx12::kDefaultHeapProps);
		mTopLevelASBuffers.pResult = nv_helpers_dx12::CreateBuffer(
			mDevice, resultSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
			nv_helpers_dx12::kDefaultHeapProps);

		mTopLevelASBuffers.pInstanceDesc = nv_helpers_dx12::CreateBuffer(
			mDevice, instanceDescSize, D3D12_RESOURCE_FLAG_NONE,
			D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);
	}
	mTopLevelAsGenerator.Generate(mCommandList.Get(), mTopLevelASBuffers.pScratch.Get(), mTopLevelASBuffers.pResult.Get(), mTopLevelASBuffers.pInstanceDesc.Get(), updateOnly, mTopLevelASBuffers.pResult.Get());
}

void D3DApp::CreateAccelerationStructures()
{
	ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(),
		nullptr));
	auto vertexBuffer = geometries["Cube"]->vertexBuffer;
	auto indexBuffer = geometries["Cube"]->indexBuffer;
	AccelerationStructureBuffers bottomLevelBuffers = CreateBottomLevelAS({ {vertexBuffer.Get(), 24} }, {{indexBuffer.Get(), 36}});
	std::vector<Cube*> cubes = mRubikCube.GetAllCubies();
	for (int i = 0; i != cubes.size(); i++)
	{
		if (cubes[i] == nullptr) break;
		mInstances.push_back({ bottomLevelBuffers.pResult, XMLoadFloat4x4(&cubes[i]->world) });
	}

	CreateTopLevelAS(mInstances);

	mCommandList->Close();
	ID3D12CommandList* ppCommandLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(1, ppCommandLists);
	
	FlushCommandQueue();

	ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

	mBottomLevelAS = bottomLevelBuffers.pResult;
}

ComPtr<ID3D12RootSignature> D3DApp::CreateRayGenSignature()
{
	nv_helpers_dx12::RootSignatureGenerator rsc;
	rsc.AddHeapRangesParameter(
		{
			{0, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0},
			{0, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1},
			{0, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 2}
		}
	);

	return rsc.Generate(mDevice, true);
}

ComPtr<ID3D12RootSignature> D3DApp::CreateMissSignature()
{
	nv_helpers_dx12::RootSignatureGenerator rsc;
	rsc.AddHeapRangesParameter(
		{
			{0, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1}
		}
	);
	return rsc.Generate(mDevice, true);
}

ComPtr<ID3D12RootSignature> D3DApp::CreateHitSignature()
{
	nv_helpers_dx12::RootSignatureGenerator rsc;
	rsc.AddHeapRangesParameter(
		{
			{0, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1},
			{0, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 2}
		}
	);
	rsc.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV, 1);
	rsc.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV, 2);
	rsc.AddHeapRangesParameter(
		{
			{3, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1}
		}
	);
	rsc.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_CBV, 1);
	return rsc.Generate(mDevice, true);
}

void D3DApp::CreateRaytracingPipeline()
{
	nv_helpers_dx12::RayTracingPipelineGenerator pipeline(mDevice);

	mRayGenLibrary = nv_helpers_dx12::CompileShaderLibrary(L"Shaders/Raytrace/RayGen.hlsl");
	mMissLibrary = nv_helpers_dx12::CompileShaderLibrary(L"Shaders/Raytrace/Miss.hlsl");
	mHitLibrary = nv_helpers_dx12::CompileShaderLibrary(L"Shaders/Raytrace/Hit.hlsl");

	pipeline.AddLibrary(mRayGenLibrary.Get(), {L"RayGen"});
	pipeline.AddLibrary(mMissLibrary.Get(), {L"Miss"});
	pipeline.AddLibrary(mHitLibrary.Get(), {L"ClosestHit"});

	mRayGenSignature = CreateRayGenSignature();
	mMissSignature = CreateMissSignature();
	mHitSignature = CreateHitSignature();

	pipeline.AddHitGroup(L"HitGroup", L"ClosestHit");

	pipeline.AddRootSignatureAssociation(mRayGenSignature.Get(), { L"RayGen" });
	pipeline.AddRootSignatureAssociation(mMissSignature.Get(), { L"Miss" });
	pipeline.AddRootSignatureAssociation(mHitSignature.Get(), { L"HitGroup" });

	pipeline.SetMaxPayloadSize(4 * sizeof(float) + sizeof(UINT));
	pipeline.SetMaxAttributeSize(2 * sizeof(float));
	pipeline.SetMaxRecursionDepth(2);

	mRtStateObject = pipeline.Generate();

	ThrowIfFailed(mRtStateObject->QueryInterface(IID_PPV_ARGS(&mRtStateObjectProps)));
}

void D3DApp::CreateRaytracingOutputBuffer()
{
	D3D12_RESOURCE_DESC resDesc = {};
	resDesc.DepthOrArraySize = 1;
	resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	resDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	resDesc.Width = mClientWidth;
	resDesc.Height = mClientHeight;
	resDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	resDesc.MipLevels = 1;
	resDesc.SampleDesc.Count = 1;
	ThrowIfFailed(mDevice->CreateCommittedResource(&nv_helpers_dx12::kDefaultHeapProps, D3D12_HEAP_FLAG_NONE, &resDesc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&mOutputResource)));
}

void D3DApp::CreateShaderResourceHeap()
{
	mSrvUavHeap = nv_helpers_dx12::CreateDescriptorHeap(
		mDevice, 5, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, true);
	D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = mSrvUavHeap->GetCPUDescriptorHandleForHeapStart();
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	mDevice->CreateUnorderedAccessView(mOutputResource.Get(), nullptr, &uavDesc, srvHandle);

	srvHandle.ptr += mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.RaytracingAccelerationStructure.Location = mTopLevelASBuffers.pResult->GetGPUVirtualAddress();
	mDevice->CreateShaderResourceView(nullptr, &srvDesc, srvHandle);

	srvHandle.ptr += mDevice->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
	cbvDesc.BufferLocation = mMainPassConstantBuffer->GetBuffer()->GetGPUVirtualAddress();
	UINT passByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstant));
	cbvDesc.SizeInBytes = passByteSize;
	mDevice->CreateConstantBufferView(&cbvDesc, srvHandle);


	srvHandle.ptr += mDevice->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	auto woodCrateTex = mTextures["woodCrateTex"].get();
	srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = woodCrateTex->Resource->GetDesc().Format;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = woodCrateTex->Resource->GetDesc().MipLevels;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
	mDevice->CreateShaderResourceView(woodCrateTex->Resource.Get(), &srvDesc, srvHandle);

	srvHandle.ptr += mDevice->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	Texture* skyTex = mTextures["skyTex"].get();
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
	srvDesc.TextureCube.MostDetailedMip = 0;
	srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
	srvDesc.TextureCube.MipLevels = skyTex->Resource->GetDesc().MipLevels;
	srvDesc.Format = skyTex->Resource->GetDesc().Format;
	mDevice->CreateShaderResourceView(skyTex->Resource.Get(), &srvDesc, srvHandle);

}

void D3DApp::CreateShaderBindingTable()
{
	mSbtHelper.Reset();

	D3D12_GPU_DESCRIPTOR_HANDLE srvUavHeapHandle = mSrvUavHeap->GetGPUDescriptorHandleForHeapStart();

	auto heapPointer = reinterpret_cast<void*>(srvUavHeapHandle.ptr);

	mSbtHelper.AddRayGenerationProgram(L"RayGen", { heapPointer });

	D3D12_GPU_DESCRIPTOR_HANDLE srvUavHeapHandle2 = mSrvUavHeap->GetGPUDescriptorHandleForHeapStart();
	srvUavHeapHandle2.ptr += mDevice->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) * 3;
	auto heapPointer2 = reinterpret_cast<void*>(srvUavHeapHandle2.ptr);

	mSbtHelper.AddMissProgram(L"Miss", { heapPointer2 });

	srvUavHeapHandle.ptr += mDevice->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) * 2;
	auto texturePointer = reinterpret_cast<void*>(srvUavHeapHandle.ptr);

	std::vector<Cube*> cubes = mRubikCube.GetAllCubies();
	UINT elementByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));

	for (int i = 0; i != cubes.size(); i++)
	{
		D3D12_GPU_VIRTUAL_ADDRESS cbAddress = mObjectConstantsBuffer->GetBuffer()->GetGPUVirtualAddress();
		cbAddress += cubes[i]->constantBufferIndex * elementByteSize;
		mSbtHelper.AddHitGroup(L"HitGroup", { heapPointer, (void*)(geometries["Cube"]->vertexBuffer->GetGPUVirtualAddress()), (void*)(geometries["Cube"]->indexBuffer->GetGPUVirtualAddress()),texturePointer, (void*)cbAddress });
	}


	uint32_t sbtSize = mSbtHelper.ComputeSBTSize();

	mSbtStorage = nv_helpers_dx12::CreateBuffer(mDevice, sbtSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);

	if (!mSbtStorage)
	{
		throw std::logic_error("Could not allocate the shader binding table");
	}

	mSbtHelper.Generate(mSbtStorage.Get(), mRtStateObjectProps.Get());
}

