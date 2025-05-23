#pragma once
#include <vector>
#include <string>
#include <DirectXMath.h>

#define XR_USE_GRAPHICS_API_D3D12

#if defined(XR_USE_GRAPHICS_API_D3D12)
#include <d3d12.h>
#include <dxgi1_6.h>
#endif

// OpenXR Helper
#include <OpenXRHelper.h>

class OpenXrManager
{
	struct RenderLayerInfo {
		XrTime predictedDisplayTime;
		std::vector<XrCompositionLayerBaseHeader* > layers;
		XrCompositionLayerProjection layerProjection = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
		std::vector<XrCompositionLayerProjectionView> layerProjectionViews;
	};
public:
	OpenXrManager(ID3D12Device* device, ID3D12CommandQueue* commandQueue) : 
		mDevice(device), mCommandQueue(commandQueue){}
	void Run();
	void Update();
	void StartFrame();
	void SetCurrentSwapchainImage(ID3D12Resource* colorImage, ID3D12Resource* depthImage, ID3D12GraphicsCommandList* commandList);
	void EndFrame();
	void AcquireSwapchainImages();
	void PollSystemEvents();
	void PollEvents();
	bool HasSession() { return mSessionRunning; }
	DirectX::XMMATRIX GetViewMatrix(int Index) { return ViewMatrix[Index]; }

private:
	void CreateInstance();
	void CreateDebugMessenger();

	void CreateSession();
	void DestroySession();

	void GetInstanceProperties();
	void GetSystemID();

	void DestroyDebugMessenger();
	void DestroyInstance();



	void GetViewConfigurationViews();
	void CreateSwapChains();
	void DestroySwapChains();
	void GetEnvironmentBlendModes();
	void CreateReferenceSpace();
	void RenderFrame();
	void DestroyReferenceSpace();
	bool RenderLayer(RenderLayerInfo& renderLayerInfo);

	void CreateActionSet();
	void SuggestBindigs();
	void RecordCurrentBindings();
	void CreateActionPoses();
	void AttachActionSet();

	void PollActions(XrTime predictedTime);
	void BlockInteraction();


	XrPath CreateXrPath(const char* pathString)
	{
		XrPath xrPath;
		OPENXR_CHECK(xrStringToPath(m_xrInstance, pathString, &xrPath), "Failed to create XrPath from string");
		return xrPath;
	}

	std::string FromXrPath(XrPath path) {
		uint32_t strl;
		char text[XR_MAX_PATH_LENGTH];
		XrResult res;
		res = xrPathToString(m_xrInstance, path, XR_MAX_PATH_LENGTH, &strl, text);
		std::string str;
		if (res == XR_SUCCESS) {
			str = text;
		}
		else {
			OPENXR_CHECK(res, "Failed to retrieve path.");
		}
		return str;
	}
private:
	std::vector<DirectX::XMMATRIX> ViewMatrix;
	bool mEnableVr = false;
	XrInstance m_xrInstance = XR_NULL_HANDLE;
	std::vector<const char*> mActiveApiLayers = {};
	std::vector<const char*> mActiveInstanceExtensions = {};
	std::vector<std::string> mApiLayers = {};
	std::vector<std::string> mInstanceExtensions = {};

	XrDebugUtilsMessengerEXT mDebugUtilsMessenger = {};
	XrFormFactor mFormFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId mSystemId = {};

	XrSystemProperties mSystemProperties = { XR_TYPE_SYSTEM_PROPERTIES };

	XrSession mSession = XR_NULL_HANDLE;
	XrSessionState mSessionState = XR_SESSION_STATE_UNKNOWN;
	PFN_xrGetD3D12GraphicsRequirementsKHR xrGetD3D12GraphicsRequirementsKHR = nullptr;
	ID3D12Device* mDevice = nullptr;
	ID3D12CommandQueue* mCommandQueue = nullptr;
	XrGraphicsBindingD3D12KHR graphicsBinding{};

	std::vector<XrViewConfigurationType> mApplicationViewConfigurations = { XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO };
	std::vector<XrViewConfigurationType> mViewConfigurations;
	XrViewConfigurationType mViewConfiguration = XR_VIEW_CONFIGURATION_TYPE_MAX_ENUM;
	std::vector<XrViewConfigurationView> mViewConfigurationViews;

	struct SwapchainInfo {
		XrSwapchain swapchain = XR_NULL_HANDLE;
		int64_t swapchainFormat = 0;
		std::vector<void*> imageViews;
	};

	std::vector<SwapchainInfo> mColorSwapchainInfos = {};
	std::vector<SwapchainInfo> mDepthSwapchainInfos = {};

	std::vector<XrEnvironmentBlendMode> mApplicationEnvironmentBlendModes = { XR_ENVIRONMENT_BLEND_MODE_OPAQUE, XR_ENVIRONMENT_BLEND_MODE_ADDITIVE };
	std::vector<XrEnvironmentBlendMode> mEnvironmentBlendModes = {};

	XrEnvironmentBlendMode mEnvironmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_MAX_ENUM;

	XrSpace mLocalSpace = XR_NULL_HANDLE;

	XrFrameState mFrameState;
	RenderLayerInfo mRenderLayerInfo;

	std::vector<ID3D12Resource*> mCurrentAcquiredSwapchainColorImages;
	std::vector<ID3D12Resource*> mCurrentAcquiredSwapchainDepthImages;

	bool mApplicationRunning = true;
	bool mSessionRunning = false;

	RenderLayerInfo renderLayerInfo;

	// Interaction stuff

	XrActionSet mActionSet;
	XrAction mGrabCubeAction, mSpawnCubeAction, mChangeColorAction;

	XrActionStateFloat mGrabState[2] = { {XR_TYPE_ACTION_STATE_FLOAT}, {XR_TYPE_ACTION_STATE_FLOAT} };
	XrActionStateBoolean mChangeColorState[2] = { {XR_TYPE_ACTION_STATE_BOOLEAN}, {XR_TYPE_ACTION_STATE_BOOLEAN} };
	XrActionStateBoolean mSpawnCubeState = { XR_TYPE_ACTION_STATE_BOOLEAN };

	XrAction mBuzzAction;
	float mBuzz[2] = { 0, 0 };

	XrAction mPalmPoseAction;
	XrPath mHandPaths[2] = { 0, 0 };

	XrSpace mHandPoseSpace[2];
	XrActionStatePose mHandPoseState[2] = { {XR_TYPE_ACTION_STATE_POSE}, {XR_TYPE_ACTION_STATE_POSE} };

	XrPosef m_handPose[2] = {
		{{1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.5f}},
		{{1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.5f}} 
	};
};

