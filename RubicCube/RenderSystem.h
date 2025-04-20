#include "Entity.h"
#include <d3d12.h>
#include <d3dUtil.h>
#include <Geometry.h>
#include "Graphics/Buffer.h"

struct Renderable
{
	Material* material = nullptr;
	Geometry* geometry = nullptr;
};

struct RenderSystemParams
{
	HWND hwnd = nullptr;
};

class RenderSystem : public System
{
public:
	void Init(RenderSystemParams renderSystemParams);

	void Update(float dt);

	ComPtr<ID3D12GraphicsCommandList4> GetCommandList() { return mCommandList; }
	ComPtr<ID3D12Device5> GetDevice() { return mDevice; }
	void FlushCommandQueue();
	void CmdListCloseAndExecute();

	static const int kSwapChainBufferCount = 2;

private:
	void CreateD3D12Device();
	void CreateFence();
	void InitDescriptorSize();
	void CheckQualitySupport();
	void CreateCommandList();
	void CreateSwapChain();
	void CreateRtvAndDsvDescriptorHeaps();
	void CreateRenderTargetResource();
	void CreateDepthStencilResource();
	void CreateViewport();
	void CreateVertexInputLayout();
	void CreateRootSignature();
	void CreatePSO();
	
	std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> GetStaticSamplers();



	D3D12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferView() const;
	D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView() const;

	bool mRaster = true;
	int mCurrBackBuffer = 0;
	HWND mHwnd = nullptr;

	ComPtr<ID3D12CommandAllocator> mDirectCmdListAlloc = nullptr;
	ComPtr<ID3D12GraphicsCommandList4> mCommandList = nullptr;
	ComPtr<ID3D12PipelineState> mPSO = nullptr;
	ComPtr<ID3D12Resource> mSwapChainBuffer[kSwapChainBufferCount];
	ComPtr<ID3D12DescriptorHeap> mCbvHeap = nullptr;
	ComPtr<ID3D12RootSignature> mRootSignature = nullptr;
	ComPtr<IDXGIFactory4> mdxgiFactory = nullptr;
	UINT mPassCbOffset = -1;
	UINT mRtvDescriptorSize = -1;
	UINT mDsvDescriptorSize = -1;
	UINT mCbvSrvDescriptorSize = -1;
	UINT m4xMsaaQuality = -1;
	UINT mClientWidth = 800;
	UINT mClientHeight = 600;
	bool m4xMsaaState = false;    // 4X MSAA enabled
	ConstantBuffer<MaterialConstants>* mMaterialConstantsBuffer = nullptr;
	ComPtr<ID3D12DescriptorHeap> mRtvHeap = nullptr;
	ComPtr<ID3D12DescriptorHeap> mDsvHeap = nullptr;

	D3D12_VIEWPORT mScreenViewport = D3D12_VIEWPORT();
	D3D12_RECT mScissorRect = D3D12_RECT();
	UINT mTextureOffset = -1;
	ComPtr<ID3D12Device5> mDevice = nullptr;
	ComPtr<ID3D12Fence> mFence = nullptr;
	ComPtr<ID3D12CommandQueue> mCommandQueue = nullptr;
	ComPtr<IDXGISwapChain> mSwapChain = nullptr;

	const DXGI_FORMAT mBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	const DXGI_FORMAT mDepthStencilFormat = DXGI_FORMAT_D16_UNORM;

	std::vector<D3D12_INPUT_ELEMENT_DESC> mVertexDesc;

	ComPtr<ID3DBlob> mvsByteCode = nullptr;
	ComPtr<ID3DBlob> mpsByteCode = nullptr;
	std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> mPSOs;
	ComPtr<ID3D12Resource> mDepthStencilBuffer;
	UINT64 mCurrentFence = 0;

};