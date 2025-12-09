#include "Entity.h"
#include "Export.h"
#include <memory>
#include <d3d12.h>
#include <d3dUtil.h>
#include <Geometry.h>
#include "Graphics/Buffer.h"
#include "Graphics/DescriptorHeap.h"
#include <Graphics/RenderTexture.h>
#include <UserInterface\ViewportWindow.h>
#include <queue>


template <typename T>
struct ConstantBufferWrapper
{
    ConstantBufferWrapper(std::unique_ptr<ConstantBuffer<T>> buffer, descriptor_handle handle)
    {
        constantBuffer = std::move(buffer);
        descriptorHandle = handle;
    }
    std::unique_ptr<ConstantBuffer<T>> constantBuffer;
	descriptor_handle descriptorHandle;
};

struct Renderable
{
	Material* material = nullptr;
	Geometry* geometry = nullptr;
};

struct RenderSystemParams
{
	HWND hwnd = nullptr;
	Entity camera = -1;
};

class CHERRYENGINE_API RenderSystem : public System
{
public:
	RenderSystem() {}
	void Init(RenderSystemParams renderSystemParams);
	virtual void OnEntityAdded(Entity entity);
	virtual void OnEntityRemoved(Entity entity);
	void UpdateCbs(float dt);
	void UpdateEntityCbs(float dt);
	void UpdateMaterialCbs(float dt);
	void Update(float dt);

	ComPtr<ID3D12GraphicsCommandList4> GetCommandList() { return mCommandList; }
	ComPtr<ID3D12Device5> GetDevice() { return mDevice; }
	void ResetCommandList();
	void FlushCommandQueue();
	void CmdListCloseAndExecute();
	void CreateMaterial(std::string name, XMFLOAT4 diffuseAlbedo, XMFLOAT3 fresnelR0, float roughness, int diffuseSrvHeapIndex = 0);
	Material* GetMaterial(std::string name);
	int CreateTexture(const std::string& name, const std::wstring& file);

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
	void CreateConstantBuffers();
	void InitializeImgui();
	
	std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> GetStaticSamplers();



	D3D12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferView() const;
	//D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView() const;
	D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView() const;

	bool mRaster = true;
	int mCurrBackBuffer = 0;
	HWND mHwnd = nullptr;

	ComPtr<ID3D12CommandAllocator> mDirectCmdListAlloc = nullptr;
	ComPtr<ID3D12GraphicsCommandList4> mCommandList = nullptr;
	ComPtr<ID3D12PipelineState> mPSO = nullptr;
	ComPtr<ID3D12Resource> mSwapChainBuffer[kSwapChainBufferCount];
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
	std::unique_ptr<ConstantBuffer<MaterialConstants>> mMaterialConstantsBuffer;
	std::unordered_map<std::string, std::unique_ptr<Material>> mMaterials;
	std::unordered_map<std::string, std::unique_ptr<Texture>> mTextures;
	std::unordered_map<int, Texture*> mIdToTexture;
	std::unordered_map<int, D3D12_GPU_VIRTUAL_ADDRESS> mTextureIdToGpuAddress;

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

	RenderSystemParams mRenderSystemParams;

	DescriptorHeap mRtvDescHeap{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV };
	DescriptorHeap mDsvDescHeap{ D3D12_DESCRIPTOR_HEAP_TYPE_DSV };;
	DescriptorHeap mSrvDescHeap{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV };

	std::unique_ptr<ConstantBufferWrapper<class PassConstant>> mMainPassCbWrapper;
	std::unique_ptr<ConstantBuffer<class ObjectConstants>> mObjectConstantsBuffer;
	std::unordered_map<Entity, descriptor_handle> mEntityToDescriptorHandleMap;
	std::unordered_map<Entity, UINT16> mEntityToCbIndexMap;

	std::unique_ptr<RenderTexture> mRenderTexture;
	std::unique_ptr<ViewportWindow> mViewportWindow;
	std::unordered_map<UINT, descriptor_handle> mBackBufferIdToDescriptorHandle;
	std::unordered_map<std::string, descriptor_handle> mRenderTextureNameToDescriptorHandle;
	D3D12_CPU_DESCRIPTOR_HANDLE mRenderTextureDescHandle;
	// Free list for constant buffer indices. When an entity is removed its
	// CB index is returned to this queue and reused for future entities.
	std::queue<UINT16> mFreeCbIndices;
	// Next CB index to allocate when free list is empty.
	UINT16 mNextCbIndex = 0;
};