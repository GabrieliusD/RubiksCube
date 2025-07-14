#pragma once
#include "Core.h"
#include <d3d12.h>
#include "d3dUtil.h"
#include "GameTimer.h"
#include "Geometry.h"
#include "RenderObject.h"
#include "PassConstant.h"
#include "Common.h"

#include "Graphics/Buffer.h"
#include <utility>
#include <dxcapi.h>
#include "nv_helpers_dx12\TopLevelASGenerator.h"
#include "nv_helpers_dx12\ShaderBindingTableGenerator.h"
#define XR_USE_GRAPHICS_API_D3D12

#if defined(XR_USE_GRAPHICS_API_D3D12)
#include <d3d12.h>
#include <dxgi1_6.h>
#endif

// OpenXR Helper
#include <OpenXRHelper.h>
#include <Graphics\D3DCore.h>
#include <Transform.h>
#pragma comment(lib,"d3dcompiler.lib")
#pragma comment(lib,"dxcompiler.lib")
#pragma comment(lib, "D3D12.lib")
#pragma comment(lib, "dxgi.lib")
using Microsoft::WRL::ComPtr;



struct AccelerationStructureBuffers
{
	ComPtr<ID3D12Resource> pScratch;
	ComPtr<ID3D12Resource> pResult;
	ComPtr<ID3D12Resource> pInstanceDesc;
};

extern class CHERRY_ENGINE_API D3DApp* CreateApplication();
class CHERRY_ENGINE_API D3DApp
{

public: 
	static const int kSwapChainBufferCount = 2;
	std::unordered_map<std::string, std::unique_ptr<Geometry>> geometries;
	std::unordered_map<std::string, std::unique_ptr<Material>> materials;
	auto GetRenderSystem() { return mRenderSystem; }

public:
	D3DApp();
	static D3DApp* GetApp();
	virtual ~D3DApp();
	void CreateObject(std::string meshName, const Transform& transform);
	void InitDirectX();
	bool InitWindow();
	void InitVrHeadset();
	void InitECS();
	virtual void OnAppInitialized() {}
	
	void InitImgui();
	void RenderImgui();
	void VictoryScreen(bool Enable);

	int Run();
	virtual LRESULT MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	void CalculateFrameStats();
	virtual void Update(GameTimer& timer);
	void UpdateRubikCubeInstances();
	virtual void OnMouseDown(WPARAM btnState, int x, int y);
	virtual void OnMouseUp(WPARAM btnState, int x, int y);
	virtual void OnMouseMove(WPARAM btnState, int x, int y);
	virtual void OnKeyDown(WPARAM btnState);
	void Pick(int sx, int sy);

	inline int GetWidth() { return mClientWidth; }
	inline int GetHeight() { return mClientHeight; }
	enum Hand : uint8_t
	{
		left,
		right
	};
	enum class SwapchainType : uint8_t {
		COLOR,
		DEPTH
	};
	std::unordered_map<XrSwapchain, std::pair<SwapchainType, std::vector<XrSwapchainImageD3D12KHR>>> swapchainImagesMap{};
	const DXGI_FORMAT mBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	const DXGI_FORMAT mDepthStencilFormat = DXGI_FORMAT_D16_UNORM; //DXGI_FORMAT_D24_UNORM_S8_UINT;

	XMFLOAT3 GetEyePosition() { return mEyePos; }
private:
	D3DCore mD3DCore;
	DirectX::XMFLOAT3 mEyePos;
	float mTheta = 1.5f * DirectX::XM_PI;
	float mPhi = DirectX::XM_PIDIV4;
	float mRadius = 50;
	
	POINT mLastMousePos;
	DirectX::XMFLOAT4X4 mView = MathHelper::Identity4x4();
	BYTE* mMappedData = nullptr;
	BYTE* mMappedPassData = nullptr;
	DirectX::XMFLOAT4X4 mWorld = MathHelper::Identity4x4();
	DirectX::XMFLOAT4X4 mProj = MathHelper::Identity4x4();
	RubikCube mRubikCube;

	std::vector<std::unique_ptr<RenderObject>> mAllRenderObjects;
	std::vector<RenderObject*> mOpaqueObjects;
	std::vector<RenderObject*> mSkyObjects;
	RenderObject* mLeftController;
	RenderObject* mRightController;
	std::unordered_map<std::string, std::unique_ptr<Texture>> mTextures;
	std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> mPSOs;
	std::unordered_map<std::string, ComPtr<ID3DBlob>> mShaders;
	bool m4xMsaaState = false;    // 4X MSAA enabled
	UINT64 mCurrentFence = 0;
	bool mAppPaused = false;
	GameTimer mTimer;
	HWND mhMainWnd;
	std::wstring mMainWndCaption = L"d3d App";
	HINSTANCE mhAppInst = nullptr; 

	ID3D12Device5 *mDevice = nullptr;
	ComPtr<ID3D12Fence> mFence;
	UINT mRtvDescriptorSize;
	UINT mDsvDescriptorSize;
	UINT mCbvSrvDescriptorSize;
	ComPtr<ID3D12CommandQueue> mCommandQueue;
	ComPtr<ID3D12CommandAllocator> mDirectCmdListAlloc;
	ComPtr<ID3D12GraphicsCommandList4> mCommandList;
	ComPtr<IDXGISwapChain> mSwapChain;
	UINT mClientWidth = 800;
	UINT mClientHeight = 600;
	UINT m4xMsaaQuality;
	ComPtr<IDXGIFactory4> mdxgiFactory;
	ComPtr<ID3D12DescriptorHeap> mRtvHeap;
	ComPtr<ID3D12DescriptorHeap> mDsvHeap;
	int mCurrBackBuffer = 0;
	ComPtr<ID3D12Resource> mSwapChainBuffer[kSwapChainBufferCount];
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
	ComPtr<ID3D12PipelineState> mPSO;
	ComPtr<ID3D12PipelineState> mPSOSky;
	ComPtr<ID3DBlob> mvsByteCode = nullptr;
	ComPtr<ID3DBlob> mpsByteCode = nullptr;

	ConstantBuffer<PassConstant>* mMainPassConstantBuffer;
	ConstantBuffer<ObjectConstants>* mObjectConstantsBuffer;
	ConstantBuffer<MaterialConstants>* mMaterialConstantsBuffer;

	PassConstant mMainPassCb;
	UINT mPassCbOffset;
	UINT mTextureOffset;
	UINT mImGuiOffset;

	//ray tracing
	bool mRaster = true;
	ComPtr<ID3D12Resource> mBottomLevelAS;
	nv_helpers_dx12::TopLevelASGenerator mTopLevelAsGenerator;
	AccelerationStructureBuffers mTopLevelASBuffers;
	std::vector<std::pair<ComPtr<ID3D12Resource>, DirectX::XMMATRIX>> mInstances;

	ComPtr<IDxcBlob> mRayGenLibrary;
	ComPtr<IDxcBlob> mHitLibrary;
	ComPtr<IDxcBlob> mMissLibrary;

	ComPtr<ID3D12RootSignature> mRayGenSignature;
	ComPtr<ID3D12RootSignature> mHitSignature;
	ComPtr<ID3D12RootSignature> mMissSignature;

	ComPtr<ID3D12StateObject> mRtStateObject;
	ComPtr<ID3D12StateObjectProperties> mRtStateObjectProps;

	ComPtr<ID3D12Resource> mOutputResource;
	//Attempt to reuse the mCbvHeap
	ComPtr<ID3D12DescriptorHeap> mSrvUavHeap;

	ComPtr<ID3D12Resource> mSbtStorage;
	nv_helpers_dx12::ShaderBindingTableGenerator mSbtHelper;

	class OpenXrManager* openXrManager;
private:
	bool mSupportsRaytracing = false;

protected:
	static D3DApp* mApp;
	std::shared_ptr<class RenderSystem> mRenderSystem;
	std::shared_ptr<class CameraSystem> mCameraSystem;

};

