#pragma once
#include <d3d12.h>
#include "d3dUtil.h"
#include "GameTimer.h"
#include "Geometry.h"
#include "RenderObject.h"
#include "PassConstant.h"
#include "Common.h"
#pragma comment(lib,"d3dcompiler.lib")
#pragma comment(lib, "D3D12.lib")
#pragma comment(lib, "dxgi.lib")

#include "Graphics/Buffer.h"
using Microsoft::WRL::ComPtr;
class D3DApp
{
public: 
	static const int SwapChainBufferCount = 2;
public:
	D3DApp();
	static D3DApp* GetApp();
	virtual ~D3DApp();
	void Init();
	bool InitWindow();
	void CreateSwapChain();
	void CreateRtvAndDsvDescriptorHeaps();
	void CreateRenderTargetResources();
	void CreateDepthStencilResource();
	void CreateViewport();
	
	//getting ready to draw
	void VertexInputLayout();
	void CreateVertexAndIndexBuffer();
	void SeperatePosAndColorBuffer();
	void CreateRenderObjects();
	void CreateConstantBufferViewsForRenderObjects();
	void CreateMaterials();
	void CreateTextures();
	D3D12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferView() const;
	D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView() const;
	int Run();
	virtual LRESULT MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	void FlushCommandQueue();

	void CalculateFrameStats();
	virtual void Update(GameTimer& mTimer);
	virtual void Draw(GameTimer& mTimer);
	void UpdateMainPasCb(const GameTimer& mTimer);
	void UpdateMaterialCb(const GameTimer& mTimer);
	virtual void OnMouseDown(WPARAM btnState, int x, int y);
	virtual void OnMouseUp(WPARAM btnState, int x, int y);
	virtual void OnMouseMove(WPARAM btnState, int x, int y);

	void Pick(int sx, int sy);

	std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> GetStaticSamplers();
private:
	DirectX::XMFLOAT3 mEyePos;
	float mTheta = 1.5f * DirectX::XM_PI;
	float mPhi = DirectX::XM_PIDIV4;
	float mRadius = 50.0f;
	
	POINT mLastMousePos;
	DirectX::XMFLOAT4X4 mView = MathHelper::Identity4x4();
	BYTE* mMappedData = nullptr;
	BYTE* mMappedPassData = nullptr;
	DirectX::XMFLOAT4X4 mWorld = MathHelper::Identity4x4();
	DirectX::XMFLOAT4X4 mProj = MathHelper::Identity4x4();
	RubikCube mRubikCube;
public:
	std::unordered_map<std::string, std::unique_ptr<Geometry>> Geometries;
	std::vector<std::unique_ptr<RenderObject>> AllRenderObjects;
	std::vector<RenderObject*> OpaqueObjects;
	std::vector<RenderObject*> SkyObjects;
	std::unordered_map<std::string, std::unique_ptr<Material>> mMaterials;
	std::unordered_map<std::string, std::unique_ptr<Texture>> mTextures;
	std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> mPSOs;
	std::unordered_map<std::string, ComPtr<ID3DBlob>> mShaders;
private:
	bool m4xMsaaState = false;    // 4X MSAA enabled
	UINT64 mCurrentFence = 0;
	bool mAppPaused = false;
	GameTimer mTimer;
	HWND mhMainWnd;
	std::wstring mMainWndCaption = L"d3d App";
	HINSTANCE mhAppInst = nullptr; 

	ID3D12Device *m_device = nullptr;
	ComPtr<ID3D12Fence> m_fence;
	UINT m_rtvDescriptorSize;
	UINT m_dsvDescriptorSize;
	UINT m_cbvSrvDescriptorSize;
	ComPtr<ID3D12CommandQueue> mCommandQueue;
	ComPtr<ID3D12CommandAllocator> mDirectCmdListAlloc;
	ComPtr<ID3D12GraphicsCommandList> mCommandList;
	ComPtr<IDXGISwapChain> mSwapChain;
	UINT mClientWidth = 800;
	UINT mClientHeight = 600;
	const DXGI_FORMAT mBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	const DXGI_FORMAT mDepthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	UINT m4xMsaaQuality;
	ComPtr<IDXGIFactory4> mdxgiFactory;
	ComPtr<ID3D12DescriptorHeap> mRtvHeap;
	ComPtr<ID3D12DescriptorHeap> mDsvHeap;
	int mCurrBackBuffer = 0;
	ComPtr<ID3D12Resource> mSwapChainBuffer[SwapChainBufferCount];
	ComPtr<ID3D12Resource> mDepthStencilBuffer;
	D3D12_VIEWPORT mScreenViewport;
	D3D12_RECT mScissorRect;
	float mSunTheta = 1.25f * XM_PI;
	float mSunPhi = XM_PIDIV4;

	std::vector<D3D12_INPUT_ELEMENT_DESC> mVertexDesc;
	std::vector<D3D12_INPUT_ELEMENT_DESC> mSkyVertex;
	ComPtr<ID3D12DescriptorHeap> mCbvHeap;
	ComPtr<ID3D12DescriptorHeap> mSrvDescriptorHeap;
	ComPtr<ID3D12RootSignature> mRootSignature = nullptr;
	ComPtr<ID3D12Resource> IndexBufferGPU = nullptr;
	ComPtr<ID3D12Resource> VertexBufferGPU = nullptr;
	D3D12_VERTEX_BUFFER_VIEW vbv = {};
	D3D12_INDEX_BUFFER_VIEW ibv;
	ComPtr<ID3D12PipelineState> mPSO;
	ComPtr<ID3D12PipelineState> mPSOSky;
	ComPtr<ID3DBlob> mvsByteCode = nullptr;//d3dUtil::LoadBinary(L"Shaders/VertexShader.cso");
	ComPtr<ID3DBlob> mpsByteCode = nullptr;// d3dUtil::LoadBinary(L"Shaders/PixelShader.cso");

	ComPtr<ID3D12Resource> VertexBufferUploader = nullptr;
	ComPtr<ID3D12Resource> IndexBufferUploader = nullptr;

	ConstantBuffer<PassConstant>* mMainPassConstantBuffer;
	ConstantBuffer<ObjectConstants>* mObjectConstantsBuffer;
	ConstantBuffer<MaterialConstants>* mMaterialConstantsBuffer;
	//practice
	ComPtr<ID3D12Resource> VertexPosBuffer = nullptr;
	ComPtr<ID3D12Resource> VertexColorBuffer = nullptr;
	D3D12_VERTEX_BUFFER_VIEW PosBufferView;
	D3D12_VERTEX_BUFFER_VIEW ColorBufferView;
	ComPtr<ID3D12Resource> VertexColorBufferUploader = nullptr;
	ComPtr<ID3D12Resource> VertexPosBufferUploader = nullptr;


	PassConstant mMainPassCb;
	UINT mPassCbOffset;
	UINT mTextureOffset;
protected:
	static D3DApp* mApp;

};

