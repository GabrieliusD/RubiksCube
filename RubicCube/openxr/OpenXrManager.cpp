#include "OpenXrManager.h"
#include "D3DApp.h"
#include "MathLib.h"
void OpenXrManager::Run()
{
	if (!mEnableVr) return;
	CreateInstance();
	CreateDebugMessenger();
	GetInstanceProperties();
	CreateActionSet();
	SuggestBindigs();
	GetSystemID();
	GetViewConfigurationViews();
	GetEnvironmentBlendModes();
	CreateSession();
	CreateActionPoses();
	AttachActionSet();
	CreateReferenceSpace();
	CreateSwapChains();
	//DestroySwapChains();
	//DestroyReferenceSpace();
	//DestroySession();
	//DestroyDebugMessenger();
	//DestroyInstance();
}

void OpenXrManager::Update()
{
	if (!mEnableVr) return;
	PollSystemEvents();
	PollEvents();
	if (mSessionRunning)
	{
		PollActions(mFrameState.predictedDisplayTime);
		BlockInteraction();
	}
}

void OpenXrManager::StartFrame()
{
	if (!HasSession()) return;

	mFrameState = { XR_TYPE_FRAME_STATE };
	XrFrameWaitInfo frameWaitInfo{ XR_TYPE_FRAME_WAIT_INFO };
	OPENXR_CHECK(xrWaitFrame(mSession, &frameWaitInfo, &mFrameState), "Failed to wait for frame");

	XrFrameBeginInfo frameBeginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
	OPENXR_CHECK(xrBeginFrame(mSession, &frameBeginInfo), "Failed to begin the frame");

	renderLayerInfo = {};
	renderLayerInfo.predictedDisplayTime = mFrameState.predictedDisplayTime;
}

void OpenXrManager::SetCurrentSwapchainImage(ID3D12Resource* colorImage, ID3D12Resource* depthImage, ID3D12GraphicsCommandList* commandList)
{
	if (!HasSession()) return;
	commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(colorImage,
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE));
	commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(depthImage, 
		D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE));

	for (const auto& Swapchain : mCurrentAcquiredSwapchainColorImages)
	{
		commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(Swapchain,
			D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST));

		commandList->CopyResource(Swapchain, colorImage);

		commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(Swapchain,
			D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT));
	}

	for (const auto& Depthbuffer : mCurrentAcquiredSwapchainDepthImages)
	{
		Depthbuffer->SetName(L"Depth Buffer");
		commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(Depthbuffer,
			D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_DEST));

		commandList->CopyResource(Depthbuffer, depthImage);

		commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(Depthbuffer,
			D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_DEPTH_WRITE));
	}

	commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(colorImage,
		D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));
	commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(depthImage,
		D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE));

}

void OpenXrManager::EndFrame()
{
	if (!HasSession()) return;
	for (uint32_t i = 0; i < 2; i++)
	{
		SwapchainInfo& colorSwapchainInfo = mColorSwapchainInfos[i];
		SwapchainInfo& depthSwapchainInfo = mDepthSwapchainInfos[i];
		XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		OPENXR_CHECK(xrReleaseSwapchainImage(colorSwapchainInfo.swapchain, &releaseInfo), "Failed to release color swapchain");
		OPENXR_CHECK(xrReleaseSwapchainImage(depthSwapchainInfo.swapchain, &releaseInfo), "Failed to release depth swapchain");
	}
	renderLayerInfo.layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&renderLayerInfo.layerProjection));

	XrFrameEndInfo frameEndInfo{ XR_TYPE_FRAME_END_INFO };
	frameEndInfo.displayTime = mFrameState.predictedDisplayTime;
	frameEndInfo.environmentBlendMode = mEnvironmentBlendMode;
	frameEndInfo.layerCount = static_cast<uint32_t>(renderLayerInfo.layers.size());
	frameEndInfo.layers = renderLayerInfo.layers.data();
	OPENXR_CHECK(xrEndFrame(mSession, &frameEndInfo), "Failed to end XR frame");
}

void OpenXrManager::CreateInstance()
{
	XrApplicationInfo AI;
	AI.apiVersion = XR_CURRENT_API_VERSION;
	strncpy(AI.applicationName, "OpenXR RubicCube", XR_MAX_APPLICATION_NAME_SIZE);
	AI.applicationVersion = 1;
	strncpy(AI.engineName, "OpenXR Engine", XR_MAX_ENGINE_NAME_SIZE);
	AI.engineVersion = 1;

	mInstanceExtensions.push_back(XR_EXT_DEBUG_UTILS_EXTENSION_NAME);
	mInstanceExtensions.push_back(XR_KHR_D3D12_ENABLE_EXTENSION_NAME);


	uint32_t apiLayerCount = 0;
	std::vector<XrApiLayerProperties> apiLayerProperties;
	OPENXR_CHECK(xrEnumerateApiLayerProperties(0, &apiLayerCount, nullptr), "Failed to enumerate ApiLayerProperties.");
	apiLayerProperties.resize(apiLayerCount, { XR_TYPE_API_LAYER_PROPERTIES });
	OPENXR_CHECK(xrEnumerateApiLayerProperties(apiLayerCount, &apiLayerCount, apiLayerProperties.data()), "Failed to enumerate ApiLayerProperties.");

	for (auto& requestLayer : mApiLayers) {
		for (auto& layerProperty : apiLayerProperties) {
			// strcmp returns 0 if the strings match.
			if (strcmp(requestLayer.c_str(), layerProperty.layerName) != 0) {
				continue;
			}
			else {
				mActiveApiLayers.push_back(requestLayer.c_str());
				break;
			}
		}
	}

	uint32_t extensionCount = 0;
	std::vector<XrExtensionProperties> extensionProperties;
	OPENXR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr), "Failed to enumerate InstanceExtensionProperties.");
	extensionProperties.resize(extensionCount, { XR_TYPE_EXTENSION_PROPERTIES });
	OPENXR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensionProperties.data()), "Failed to enumerate InstanceExtensionProperties.");

	// Check the requested Instance Extensions against the ones from the OpenXR runtime.
	// If an extension is found add it to Active Instance Extensions.
	// Log error if the Instance Extension is not found.
	for (auto& requestedInstanceExtension : mInstanceExtensions) {
		bool found = false;
		for (auto& extensionProperty : extensionProperties) {
			// strcmp returns 0 if the strings match.
			if (strcmp(requestedInstanceExtension.c_str(), extensionProperty.extensionName) != 0) {
				continue;
			}
			else {
				mActiveInstanceExtensions.push_back(requestedInstanceExtension.c_str());
				found = true;
				break;
			}
		}
		if (!found) {
			std::cout << "Failed to find OpenXR instance extension: " << requestedInstanceExtension << std::endl;
		}
	}

	XrInstanceCreateInfo instanceCI{ XR_TYPE_INSTANCE_CREATE_INFO };
	instanceCI.createFlags = 0;
	instanceCI.applicationInfo = AI;
	instanceCI.enabledApiLayerCount = static_cast<uint32_t>(mActiveApiLayers.size());
	instanceCI.enabledApiLayerNames = mActiveApiLayers.data();
	instanceCI.enabledExtensionCount = static_cast<uint32_t>(mActiveInstanceExtensions.size());
	instanceCI.enabledExtensionNames = mActiveInstanceExtensions.data();
	OPENXR_CHECK(xrCreateInstance(&instanceCI, &m_xrInstance), "Failed to create Instance.");

}

void OpenXrManager::CreateDebugMessenger()
{
}

void OpenXrManager::CreateSession()
{
	XrSessionCreateInfo sessionCI{ XR_TYPE_SESSION_CREATE_INFO };

	//checck that this headset gonna work lol
	OPENXR_CHECK(xrGetInstanceProcAddr(m_xrInstance, "xrGetD3D12GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&xrGetD3D12GraphicsRequirementsKHR), "Failed to get InstanceProcAddr for xrGetD3D12GraphicsRequirementsKHR.");
	XrGraphicsRequirementsD3D12KHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
	OPENXR_CHECK(xrGetD3D12GraphicsRequirementsKHR(m_xrInstance, mSystemId, &graphicsRequirements), "Failed to get Graphics Requirements for D3D12.");

	graphicsBinding = { XR_TYPE_GRAPHICS_BINDING_D3D12_KHR };
	graphicsBinding.device = mDevice;
	graphicsBinding.queue = mCommandQueue; 

	sessionCI.next = &graphicsBinding;
	sessionCI.createFlags = 0;
	sessionCI.systemId = mSystemId;

	OPENXR_CHECK(xrCreateSession(m_xrInstance, &sessionCI, &mSession), "Failed to create Session");
}

void OpenXrManager::DestroySession()
{
	OPENXR_CHECK(xrDestroySession(mSession), "Failed to destroy session");
}

void OpenXrManager::GetInstanceProperties()
{
	XrInstanceProperties instanceProperties{ XR_TYPE_INSTANCE_PROPERTIES };
	OPENXR_CHECK(xrGetInstanceProperties(m_xrInstance, &instanceProperties), "Failed to get InstanceProperties.");

	std::cout << "OpenXR Runtime: " << instanceProperties.runtimeName << " - "
		<< XR_VERSION_MAJOR(instanceProperties.runtimeVersion) << "."
		<< XR_VERSION_MINOR(instanceProperties.runtimeVersion) << "."
		<< XR_VERSION_PATCH(instanceProperties.runtimeVersion) << std::endl;
}

void OpenXrManager::GetSystemID()
{
	XrSystemGetInfo systemGI{ XR_TYPE_SYSTEM_GET_INFO };
	systemGI.formFactor = mFormFactor;
	OPENXR_CHECK(xrGetSystem(m_xrInstance, &systemGI, &mSystemId), "Failed to get SystemID.");

	// Get the System's properties for some general information about the hardware and the vendor.
	OPENXR_CHECK(xrGetSystemProperties(m_xrInstance, mSystemId, &mSystemProperties), "Failed to get SystemProperties.");
}

void OpenXrManager::DestroyDebugMessenger()
{
}

void OpenXrManager::DestroyInstance()
{
	OPENXR_CHECK(xrDestroyInstance(m_xrInstance), "Failed to destroy Instance.");
}

void OpenXrManager::PollSystemEvents()
{
}

void OpenXrManager::PollEvents()
{
	XrEventDataBuffer eventData{ XR_TYPE_EVENT_DATA_BUFFER };
	auto XrPollEvents = [&]() -> bool {
		eventData = { XR_TYPE_EVENT_DATA_BUFFER };
		return xrPollEvent(m_xrInstance, &eventData) == XR_SUCCESS;
		};

	while (XrPollEvents())
	{
		switch (eventData.type) {
			// Log the number of lost events from the runtime.
		case XR_TYPE_EVENT_DATA_EVENTS_LOST: {
			XrEventDataEventsLost* eventsLost = reinterpret_cast<XrEventDataEventsLost*>(&eventData);
			std::cout << "OPENXR: Events Lost: " << eventsLost->lostEventCount;
			break;
		}
										   // Log that an instance loss is pending and shutdown the application.
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: {
			XrEventDataInstanceLossPending* instanceLossPending = reinterpret_cast<XrEventDataInstanceLossPending*>(&eventData);
			std::cout << "OPENXR: Instance Loss Pending at: " << instanceLossPending->lossTime;
			mSessionRunning = false;
			mApplicationRunning = false;
			break;
		}
													 // Log that the interaction profile has changed.
		case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED: {
			XrEventDataInteractionProfileChanged* interactionProfileChanged = reinterpret_cast<XrEventDataInteractionProfileChanged*>(&eventData);
			std::cout << "OPENXR: Interaction Profile changed for Session: " << interactionProfileChanged->session;
			if (interactionProfileChanged->session != mSession) {
				std::cout << "XrEventDataInteractionProfileChanged for unknown Session";
				break;
			}
				
			RecordCurrentBindings();

			break;
		}
														   // Log that there's a reference space change pending.
		case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
			XrEventDataReferenceSpaceChangePending* referenceSpaceChangePending = reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&eventData);
			std::cout << "OPENXR: Reference Space Change pending for Session: " << referenceSpaceChangePending->session;
			if (referenceSpaceChangePending->session != mSession) {
				std::cout << "XrEventDataReferenceSpaceChangePending for unknown Session";
				break;
			}
			break;
		}
															  // Session State changes:
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
			XrEventDataSessionStateChanged* sessionStateChanged = reinterpret_cast<XrEventDataSessionStateChanged*>(&eventData);
			if (sessionStateChanged->session != mSession) {
				std::cout << "XrEventDataSessionStateChanged for unknown Session";
				break;
			}

			if (sessionStateChanged->state == XR_SESSION_STATE_READY) {
				// SessionState is ready. Begin the XrSession using the XrViewConfigurationType.
				XrSessionBeginInfo sessionBeginInfo{ XR_TYPE_SESSION_BEGIN_INFO };
				sessionBeginInfo.primaryViewConfigurationType = mViewConfiguration;
				OPENXR_CHECK(xrBeginSession(mSession, &sessionBeginInfo), "Failed to begin Session.");
				mSessionRunning = true;
			}
			if (sessionStateChanged->state == XR_SESSION_STATE_STOPPING) {
				// SessionState is stopping. End the XrSession.
				OPENXR_CHECK(xrEndSession(mSession), "Failed to end Session.");
				mSessionRunning = false;
			}
			if (sessionStateChanged->state == XR_SESSION_STATE_EXITING) {
				// SessionState is exiting. Exit the application.
				mSessionRunning = false;
				mApplicationRunning = false;
			}
			if (sessionStateChanged->state == XR_SESSION_STATE_LOSS_PENDING) {
				// SessionState is loss pending. Exit the application.
				// It's possible to try a reestablish an XrInstance and XrSession, but we will simply exit here.
				mSessionRunning = false;
				mApplicationRunning = false;
			}
			// Store state for reference across the application.
			mSessionRunning = sessionStateChanged->state;
			break;
		}
		default: {
			break;
		}
		}
	}
}

void OpenXrManager::GetViewConfigurationViews()
{
	uint32_t viewConfigurationCount = 0;
	OPENXR_CHECK(xrEnumerateViewConfigurations(m_xrInstance, mSystemId, 0, &viewConfigurationCount, nullptr), "Failed to enumerate view configurations");
	mViewConfigurations.resize(viewConfigurationCount);
	OPENXR_CHECK(xrEnumerateViewConfigurations(m_xrInstance, mSystemId, viewConfigurationCount, &viewConfigurationCount, mViewConfigurations.data()), "Failed to enuumerate view configurations");

	for (const XrViewConfigurationType& viewConfiguration : mApplicationViewConfigurations)
	{
		if (std::find(mViewConfigurations.begin(), mViewConfigurations.end(), viewConfiguration) != mViewConfigurations.end())
		{
			mViewConfiguration = viewConfiguration;
			break;
		}
	}
	if (mViewConfiguration == XR_VIEW_CONFIGURATION_TYPE_MAX_ENUM)
	{
		std::cerr << "Failed to find a view configuration type. Defaulting to Stereo" << std::endl;
		mViewConfiguration = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	}
	uint32_t viewConfigurationViewCount = 0;
	OPENXR_CHECK(xrEnumerateViewConfigurationViews(m_xrInstance, mSystemId, mViewConfiguration, 0, &viewConfigurationViewCount, nullptr), "Failed to enumerate ViewConfiguration Views.");
	mViewConfigurationViews.resize(viewConfigurationViewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
	OPENXR_CHECK(xrEnumerateViewConfigurationViews(m_xrInstance, mSystemId, mViewConfiguration, viewConfigurationViewCount, &viewConfigurationViewCount, mViewConfigurationViews.data()), "Failed to enumerate ViewConfiguration Views.");
}

void OpenXrManager::CreateSwapChains()
{
	uint32_t formatCount = 0;
	OPENXR_CHECK(xrEnumerateSwapchainFormats(mSession, 0, &formatCount, nullptr), "Failed to enumerate swapchain formats");
	std::vector<int64_t> formats(formatCount);
	OPENXR_CHECK(xrEnumerateSwapchainFormats(mSession, formatCount, &formatCount, formats.data()), "Failed to enumerate swapchain formats");

	//for now i will assume we got a format that will work


	mColorSwapchainInfos.resize(mViewConfigurationViews.size());
	mDepthSwapchainInfos.resize(mViewConfigurationViews.size());

	for (size_t i = 0; i < mViewConfigurationViews.size(); i++)
	{
		SwapchainInfo& colorSwapChainInfo = mColorSwapchainInfos[i];
		SwapchainInfo& depthSwapChainInfo = mDepthSwapchainInfos[i];

		XrSwapchainCreateInfo swapchainCI{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
		swapchainCI.createFlags = 0;
		swapchainCI.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
		swapchainCI.format = D3DApp::GetApp()->mBackBufferFormat;
		swapchainCI.sampleCount = mViewConfigurationViews[i].recommendedSwapchainSampleCount;
		swapchainCI.width = D3DApp::GetApp()->GetWidth();
		swapchainCI.height = D3DApp::GetApp()->GetHeight();
		swapchainCI.faceCount = 1;
		swapchainCI.arraySize = 1;
		swapchainCI.mipCount = 1;
		OPENXR_CHECK(xrCreateSwapchain(mSession, &swapchainCI, &colorSwapChainInfo.swapchain), "Failed to create color swapchain");
		colorSwapChainInfo.swapchainFormat = swapchainCI.format;

		swapchainCI.createFlags = 0;
		swapchainCI.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		swapchainCI.format = D3DApp::GetApp()->mDepthStencilFormat;
		swapchainCI.sampleCount = mViewConfigurationViews[i].recommendedSwapchainSampleCount;
		swapchainCI.width = D3DApp::GetApp()->GetWidth();
		swapchainCI.height = D3DApp::GetApp()->GetHeight();
		swapchainCI.faceCount = 1;
		swapchainCI.arraySize = 1;
		swapchainCI.mipCount = 1;
		OPENXR_CHECK(xrCreateSwapchain(mSession, &swapchainCI, &depthSwapChainInfo.swapchain), "Failed to create color swapchain");
		depthSwapChainInfo.swapchainFormat = swapchainCI.format;

		uint32_t colorSwapchainImageCount = 0;
		OPENXR_CHECK(xrEnumerateSwapchainImages(colorSwapChainInfo.swapchain, 0, &colorSwapchainImageCount, nullptr), "Failed to enumerate Color swapchain images");
		XrSwapchainImageBaseHeader* colorSwapchainImages = D3DApp::GetApp()->AllocateSwapchainImageData(colorSwapChainInfo.swapchain, D3DApp::SwapchainType::COLOR, colorSwapchainImageCount);
		OPENXR_CHECK(xrEnumerateSwapchainImages(colorSwapChainInfo.swapchain, colorSwapchainImageCount, &colorSwapchainImageCount, colorSwapchainImages), "Failed to enumerate Color Swapchain images");

		uint32_t depthSwapchainImageCount = 0;
		OPENXR_CHECK(xrEnumerateSwapchainImages(depthSwapChainInfo.swapchain, 0, &depthSwapchainImageCount, nullptr), "Failed to enumerate Depth swapchain images");
		XrSwapchainImageBaseHeader* depthSwapchainImages = D3DApp::GetApp()->AllocateSwapchainImageData(depthSwapChainInfo.swapchain, D3DApp::SwapchainType::DEPTH, depthSwapchainImageCount);
		OPENXR_CHECK(xrEnumerateSwapchainImages(depthSwapChainInfo.swapchain, depthSwapchainImageCount, &depthSwapchainImageCount, depthSwapchainImages), "Failed to enumerate depth swapchain images");

		// find out if I need to descriptor view into this resource since im planning to copy into this buffer and not render

	}
}

void OpenXrManager::DestroySwapChains()
{
	for (size_t i = 0; i < mViewConfigurationViews.size(); i++)
	{
		SwapchainInfo& colorSwapchainInfo = mColorSwapchainInfos[i];
		SwapchainInfo& depthSwapchainInfo = mDepthSwapchainInfos[i];

		for (void*& imageView : colorSwapchainInfo.imageViews)
		{

		}

		for (void*& imageView : depthSwapchainInfo.imageViews)
		{

		}

		OPENXR_CHECK(xrDestroySwapchain(colorSwapchainInfo.swapchain), "Failed to release color swapchain");
		OPENXR_CHECK(xrDestroySwapchain(depthSwapchainInfo.swapchain), "Failed to release depth swapchain");
	}
}

void OpenXrManager::GetEnvironmentBlendModes()
{
	uint32_t environmentBlendModeCount = 0;
	OPENXR_CHECK(xrEnumerateEnvironmentBlendModes(m_xrInstance, mSystemId, mViewConfiguration, 0, &environmentBlendModeCount, nullptr), "Failed to enumerate EnvironmentBlend Modes.");
	mEnvironmentBlendModes.resize(environmentBlendModeCount);
	OPENXR_CHECK(xrEnumerateEnvironmentBlendModes(m_xrInstance, mSystemId, mViewConfiguration, environmentBlendModeCount, &environmentBlendModeCount, mEnvironmentBlendModes.data()), "Failed to enumerate EnvironmentBlend Modes.");

	for (const XrEnvironmentBlendMode& environmentBlendMode : mApplicationEnvironmentBlendModes) {
		if (std::find(mEnvironmentBlendModes.begin(), mEnvironmentBlendModes.end(), environmentBlendMode) != mEnvironmentBlendModes.end()) {
			mEnvironmentBlendMode = environmentBlendMode;
			break;
		}
	}
	if (mEnvironmentBlendMode == XR_ENVIRONMENT_BLEND_MODE_MAX_ENUM) {
		std::cout << "Failed to find a compatible blend mode. Defaulting to XR_ENVIRONMENT_BLEND_MODE_OPAQUE.";
		mEnvironmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	}
}

void OpenXrManager::CreateReferenceSpace()
{
	XrReferenceSpaceCreateInfo referenceSpaceCI{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	referenceSpaceCI.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	referenceSpaceCI.poseInReferenceSpace = { {0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f} };
	OPENXR_CHECK(xrCreateReferenceSpace(mSession, &referenceSpaceCI, &mLocalSpace), "Failed to create ReferenceSpace.");
}

void OpenXrManager::RenderFrame()
{
	XrFrameState frameState{ XR_TYPE_FRAME_STATE };
	XrFrameWaitInfo frameWaitInfo{ XR_TYPE_FRAME_WAIT_INFO };
	OPENXR_CHECK(xrWaitFrame(mSession, &frameWaitInfo, &frameState), "Failed to wait for frame");

	XrFrameBeginInfo frameBeginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
	OPENXR_CHECK(xrBeginFrame(mSession, &frameBeginInfo), "Failed to begin the frame");

	bool rendered = false;

	renderLayerInfo = {};
	renderLayerInfo.predictedDisplayTime = frameState.predictedDisplayTime;

	bool sessionActive = (mSessionState == XR_SESSION_STATE_SYNCHRONIZED || mSessionState == XR_SESSION_STATE_VISIBLE || mSessionState == XR_SESSION_STATE_FOCUSED);
	if (sessionActive && frameState.shouldRender)
	{
		rendered = RenderLayer(renderLayerInfo);
		if (rendered)
		{
			renderLayerInfo.layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&renderLayerInfo.layerProjection));
		}
	}

	XrFrameEndInfo frameEndInfo{ XR_TYPE_FRAME_END_INFO };
	frameEndInfo.displayTime = frameState.predictedDisplayTime;
	frameEndInfo.environmentBlendMode = mEnvironmentBlendMode;
	frameEndInfo.layerCount = static_cast<uint32_t>(renderLayerInfo.layers.size());
	frameEndInfo.layers = renderLayerInfo.layers.data();
	OPENXR_CHECK(xrEndFrame(mSession, &frameEndInfo), "Failed to end XR frame");
}

void OpenXrManager::DestroyReferenceSpace()
{
	OPENXR_CHECK(xrDestroySpace(mLocalSpace), "Failed to destroy Space.")
}

bool OpenXrManager::RenderLayer(RenderLayerInfo& renderLayerInfo)
{
	std::vector<XrView> views(mViewConfigurationViews.size(), {XR_TYPE_VIEW});
	XrViewState viewState{ XR_TYPE_VIEW_STATE };
	XrViewLocateInfo viewLocateInfo{ XR_TYPE_VIEW_LOCATE_INFO };
	viewLocateInfo.viewConfigurationType = mViewConfiguration;
	viewLocateInfo.displayTime = renderLayerInfo.predictedDisplayTime;
	viewLocateInfo.space = mLocalSpace;
	uint32_t viewCount = 0;
	XrResult result = xrLocateViews(mSession, &viewLocateInfo, &viewState, static_cast<uint32_t>(views.size()), &viewCount, views.data());
	if (result != XR_SUCCESS)
	{
		std::cout << "Failed to locate views" << std::endl;
		return false;
	}

	renderLayerInfo.layerProjectionViews.resize(viewCount, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW });

	for (uint32_t i = 0; i < viewCount; i++)
	{
		SwapchainInfo& colorSwapchainInfo = mColorSwapchainInfos[i];
		SwapchainInfo& depthSwapchainInfo = mDepthSwapchainInfos[i];

		uint32_t colorImageIndex = 0;
		uint32_t depthImageIndex = 0;

		XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		OPENXR_CHECK(xrAcquireSwapchainImage(colorSwapchainInfo.swapchain, &acquireInfo, &colorImageIndex), "Failed to acquire image from the color swapchain");
		OPENXR_CHECK(xrAcquireSwapchainImage(depthSwapchainInfo.swapchain, &acquireInfo, &depthImageIndex), "Failed to acquire image from the depth swapchain");
		
		XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		waitInfo.timeout = XR_INFINITE_DURATION;
		OPENXR_CHECK(xrWaitSwapchainImage(colorSwapchainInfo.swapchain, &waitInfo), "Failed to wait for color swapchain image");
		OPENXR_CHECK(xrWaitSwapchainImage(depthSwapchainInfo.swapchain, &waitInfo), "Failed to wait for color swapchain image");

		const int width = D3DApp::GetApp()->GetWidth();
		const int height = D3DApp::GetApp()->GetHeight();

		renderLayerInfo.layerProjectionViews[i] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
		renderLayerInfo.layerProjectionViews[i].pose = views[i].pose;
		renderLayerInfo.layerProjectionViews[i].fov = views[i].fov;
		renderLayerInfo.layerProjectionViews[i].subImage.swapchain = colorSwapchainInfo.swapchain;
		renderLayerInfo.layerProjectionViews[i].subImage.imageRect.offset.x = 0;
		renderLayerInfo.layerProjectionViews[i].subImage.imageRect.offset.y = 0;
		renderLayerInfo.layerProjectionViews[i].subImage.imageRect.extent.width = width;
		renderLayerInfo.layerProjectionViews[i].subImage.imageRect.extent.height = height;
		renderLayerInfo.layerProjectionViews[i].subImage.imageArrayIndex = 0;

		// render stuff

		D3DApp::GetApp()->GetXrSwapchainImage(colorSwapchainInfo.swapchain, colorImageIndex);
		D3DApp::GetApp()->GetXrSwapchainImage(colorSwapchainInfo.swapchain, depthImageIndex);

		// done rendering release swapchain

		XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		OPENXR_CHECK(xrReleaseSwapchainImage(colorSwapchainInfo.swapchain, &releaseInfo), "Failed to release color swapchain");
		OPENXR_CHECK(xrReleaseSwapchainImage(depthSwapchainInfo.swapchain, &releaseInfo), "Failed to release depth swapchain");
	}

	renderLayerInfo.layerProjection.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	renderLayerInfo.layerProjection.space = mLocalSpace;
	renderLayerInfo.layerProjection.viewCount = static_cast<uint32_t>(renderLayerInfo.layerProjectionViews.size());
	renderLayerInfo.layerProjection.views = renderLayerInfo.layerProjectionViews.data();

	return true;
}

void OpenXrManager::CreateActionSet()
{
	XrActionSetCreateInfo actionSetCI{ XR_TYPE_ACTION_SET_CREATE_INFO };
	strncpy(actionSetCI.actionSetName, "openxr-actionset", XR_MAX_ACTION_SET_NAME_SIZE);
	strncpy(actionSetCI.localizedActionSetName, "OpenXR ActionSet", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
	OPENXR_CHECK(xrCreateActionSet(m_xrInstance, &actionSetCI, &mActionSet), "Failed to create ActionSet");
	actionSetCI.priority = 0;

	auto CreateAction = [this](XrAction& xrAction, const char* name, XrActionType xrActionType, std::vector<const char*> subactionPaths = {}) -> void {
		XrActionCreateInfo actionCI{ XR_TYPE_ACTION_CREATE_INFO };
		actionCI.actionType = xrActionType;
		std::vector<XrPath> subactionXrpaths;
		for (auto p : subactionPaths)
		{
			subactionXrpaths.push_back(CreateXrPath(p));
		}
		actionCI.countSubactionPaths = (uint32_t)subactionXrpaths.size();
		actionCI.subactionPaths = subactionXrpaths.data();

		strncpy(actionCI.actionName, name, XR_MAX_ACTION_NAME_SIZE);
		strncpy(actionCI.localizedActionName, name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
		OPENXR_CHECK(xrCreateAction(mActionSet, &actionCI, &xrAction), "Failed to create Action.");

		};

	CreateAction(mGrabCubeAction, "grab-cube", XR_ACTION_TYPE_FLOAT_INPUT, { "/user/hand/left", "/user/hand/right" });
	CreateAction(mSpawnCubeAction, "spawn-cube", XR_ACTION_TYPE_BOOLEAN_INPUT);
	CreateAction(mChangeColorAction, "change-color", XR_ACTION_TYPE_BOOLEAN_INPUT, { "/user/hand/left", "/user/hand/right" });
	CreateAction(mPalmPoseAction, "palm-pose", XR_ACTION_TYPE_POSE_INPUT, { "/user/hand/left", "/user/hand/right" });
	CreateAction(mBuzzAction, "buzz", XR_ACTION_TYPE_VIBRATION_OUTPUT, { "/user/hand/left", "/user/hand/right" });
	mHandPaths[0] = CreateXrPath("/user/hand/left");
	mHandPaths[1] = CreateXrPath("/user/hand/right");
}

void OpenXrManager::SuggestBindigs()
{
	auto SuggestBindings = [this](const char* profilePath, std::vector<XrActionSuggestedBinding> bindings) -> bool {
		XrInteractionProfileSuggestedBinding interactionProfileSuggestedBinding{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
		interactionProfileSuggestedBinding.interactionProfile = CreateXrPath(profilePath);
		interactionProfileSuggestedBinding.suggestedBindings = bindings.data();
		interactionProfileSuggestedBinding.countSuggestedBindings = (uint32_t)bindings.size();
		if (xrSuggestInteractionProfileBindings(m_xrInstance, &interactionProfileSuggestedBinding) == XrResult::XR_SUCCESS)
			return true;

		std::cout << "Failed to suggest bindings with " << profilePath << std::endl;
		return false;
		};

	bool anyOk = false;

	anyOk |= SuggestBindings("/interaction_profiles/khr/simple_controller",
		{
			{mChangeColorAction, CreateXrPath("/user/hand/left/input/select/click")},
			{mGrabCubeAction, CreateXrPath("/user/hand/right/input/select/click")},
			{mPalmPoseAction, CreateXrPath("/user/hand/left/input/grip/pose")},
			{mPalmPoseAction, CreateXrPath("/user/hand/right/input/grip/pose")},
			{mBuzzAction, CreateXrPath("/user/hand/left/output/haptic")},
			{mBuzzAction, CreateXrPath("/user/hand/right/output/haptic")} 
		});

	if (!anyOk)
	{
		DEBUG_BREAK;
	}
}

void OpenXrManager::RecordCurrentBindings()
{
	if (mSession)
	{
		XrInteractionProfileState interactionProfile = { XR_TYPE_INTERACTION_PROFILE_STATE, 0, 0 };
		OPENXR_CHECK(xrGetCurrentInteractionProfile(mSession, mHandPaths[0], &interactionProfile), "Failed to get profile");
		if (interactionProfile.interactionProfile)
		{
			std::cout << "user/hand/left Active Profile " << FromXrPath(interactionProfile.interactionProfile).c_str() << std::endl;
		}


		OPENXR_CHECK(xrGetCurrentInteractionProfile(mSession, mHandPaths[1], &interactionProfile), "Failed to get profile");
		if (interactionProfile.interactionProfile)
		{
			std::cout << "user/hand/right Active Profile " << FromXrPath(interactionProfile.interactionProfile).c_str() << std::endl;
		}
	}
}

void OpenXrManager::CreateActionPoses()
{
	auto CreateActionPoseSpace = [this](XrSession session, XrAction xrAction, const char* subactionPath = nullptr) -> XrSpace {
		XrSpace xrSpace;
		const XrPosef xrPoseIdentity = { {0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f} };
		XrActionSpaceCreateInfo actionSpace{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
		actionSpace.action = xrAction;
		actionSpace.poseInActionSpace = xrPoseIdentity;
		if (subactionPath)
		{
			actionSpace.subactionPath = CreateXrPath(subactionPath);
		}
		OPENXR_CHECK(xrCreateActionSpace(session, &actionSpace, &xrSpace), "Failed to create ActionSpace");
		return xrSpace;
		};

	mHandPoseSpace[0] = CreateActionPoseSpace(mSession, mPalmPoseAction, "/user/hand/left");
	mHandPoseSpace[1] = CreateActionPoseSpace(mSession, mPalmPoseAction, "/user/hand/right");
}

void OpenXrManager::AttachActionSet()
{
	XrSessionActionSetsAttachInfo actionSetAttachInfo{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	actionSetAttachInfo.countActionSets = 1;
	actionSetAttachInfo.actionSets = &mActionSet;
	OPENXR_CHECK(xrAttachSessionActionSets(mSession, &actionSetAttachInfo), "Failed to attach ActionSet to Session");
}

void OpenXrManager::PollActions(XrTime predictedTime)
{
	XrActiveActionSet activeActionSet{};
	activeActionSet.actionSet = mActionSet;
	activeActionSet.subactionPath = XR_NULL_PATH;

	XrActionsSyncInfo actionsSyncInfo{ XR_TYPE_ACTIONS_SYNC_INFO };
	actionsSyncInfo.countActiveActionSets = 1;
	actionsSyncInfo.activeActionSets = &activeActionSet;
	OPENXR_CHECK(xrSyncActions(mSession, &actionsSyncInfo), "Failed to sync Actions");

	XrActionStateGetInfo actionStateGetInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
	actionStateGetInfo.action = mPalmPoseAction;
	for (int i = 0; i < 2; i++)
	{
		actionStateGetInfo.subactionPath = mHandPaths[i];
		OPENXR_CHECK(xrGetActionStatePose(mSession, &actionStateGetInfo, &mHandPoseState[i]), "Failed to get pose State");
		if (mHandPoseState[i].isActive)
		{
			XrSpaceLocation spaceLocation{ XR_TYPE_SPACE_LOCATION };
			XrResult res = xrLocateSpace(mHandPoseSpace[i], mLocalSpace, predictedTime, &spaceLocation);
			if (XR_UNQUALIFIED_SUCCESS(res) && (spaceLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
				(spaceLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0)
			{
				m_handPose[i] = spaceLocation.pose;
			}
			else
			{
				mHandPoseState[i].isActive = false;
			}
		}
	}

	for (int i = 0; i < 2; i++) {
		actionStateGetInfo.action = mGrabCubeAction;
		actionStateGetInfo.subactionPath = mHandPaths[i];
		OPENXR_CHECK(xrGetActionStateFloat(mSession, &actionStateGetInfo, &mGrabState[i]), "Failed to get Float State of Grab Cube action.");
	}
	for (int i = 0; i < 2; i++) {
		actionStateGetInfo.action = mChangeColorAction;
		actionStateGetInfo.subactionPath = mHandPaths[i];
		OPENXR_CHECK(xrGetActionStateBoolean(mSession, &actionStateGetInfo, &mChangeColorState[i]), "Failed to get Boolean State of Change Color action.");
	}
	// The Spawn Cube action has no subActionPath:
	{
		actionStateGetInfo.action = mSpawnCubeAction;
		actionStateGetInfo.subactionPath = 0;
		OPENXR_CHECK(xrGetActionStateBoolean(mSession, &actionStateGetInfo, &mSpawnCubeState), "Failed to get Boolean State of Spawn Cube action.");
	}

	for (int i = 0; i < 2; i++) {
		mBuzz[i] *= 0.5f;
		if (mBuzz[i] < 0.01f)
			mBuzz[i] = 0.0f;
		XrHapticVibration vibration{ XR_TYPE_HAPTIC_VIBRATION };
		vibration.amplitude = mBuzz[i];
		vibration.duration = XR_MIN_HAPTIC_DURATION;
		vibration.frequency = XR_FREQUENCY_UNSPECIFIED;

		XrHapticActionInfo hapticActionInfo{ XR_TYPE_HAPTIC_ACTION_INFO };
		hapticActionInfo.action = mBuzzAction;
		hapticActionInfo.subactionPath = mHandPaths[i];
		OPENXR_CHECK(xrApplyHapticFeedback(mSession, &hapticActionInfo, (XrHapticBaseHeader*)&vibration), "Failed to apply haptic feedback.");
	}
}

void OpenXrManager::BlockInteraction()
{
	// Update rotations

	XrVector3f xrLeftHandPos = m_handPose[0].position;
	XrQuaternionf xrLeftHandRot = m_handPose[0].orientation;
	XMFLOAT3 xmLeftHandPos(xrLeftHandPos.x, xrLeftHandPos.y, xrLeftHandPos.z);
	XMFLOAT4 xmLeftHandRot(xrLeftHandRot.x, xrLeftHandRot.y, xrLeftHandRot.z, xrLeftHandRot.w);
	XMVECTOR scale = XMVectorSet(0.1f, 0.1f, 0.1f, 1.0f);
	XMVECTOR zero = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
	XMVECTOR P = XMLoadFloat3(&xmLeftHandPos);
	XMVECTOR Q = XMLoadFloat4(&xmLeftHandRot);
	XMFLOAT3 eyePos = D3DApp::GetApp()->GetEyePosition();
	XMVECTOR EP = XMLoadFloat3(&eyePos);
	XMFLOAT3 myNewPos; XMStoreFloat3(&myNewPos, P);
	std::cout << "Controller Pos: x: " << myNewPos.x << " y: " << myNewPos.y << " z: " << myNewPos.z << std::endl;
	XMFLOAT4X4 M;
	XMStoreFloat4x4(&M, XMMatrixAffineTransformation(scale, zero, Q, P));
	D3DApp::GetApp()->UpdateHandPosition(D3DApp::Hand::left, M);

}

void OpenXrManager::AcquireSwapchainImages()
{
	if (!HasSession()) return;
	std::vector<XrView> views(mViewConfigurationViews.size(), { XR_TYPE_VIEW });
	XrViewState viewState{ XR_TYPE_VIEW_STATE };
	XrViewLocateInfo viewLocateInfo{ XR_TYPE_VIEW_LOCATE_INFO };
	viewLocateInfo.viewConfigurationType = mViewConfiguration;
	viewLocateInfo.displayTime = renderLayerInfo.predictedDisplayTime;
	viewLocateInfo.space = mLocalSpace;
	uint32_t viewCount = 0;
	XrResult result = xrLocateViews(mSession, &viewLocateInfo, &viewState, static_cast<uint32_t>(views.size()), &viewCount, views.data());
	if (result != XR_SUCCESS)
	{
		std::cout << "Failed to locate views" << std::endl;
		return;
	}

	renderLayerInfo.layerProjectionViews.resize(viewCount, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW });

	mCurrentAcquiredSwapchainColorImages.resize(viewCount, nullptr);
	mCurrentAcquiredSwapchainDepthImages.resize(viewCount, nullptr);
	ViewMatrix.resize(viewCount, XMMatrixIdentity());

	for (uint32_t i = 0; i < viewCount; i++)
	{
		SwapchainInfo& colorSwapchainInfo = mColorSwapchainInfos[i];
		SwapchainInfo& depthSwapchainInfo = mDepthSwapchainInfos[i];

		uint32_t colorImageIndex = 0;
		uint32_t depthImageIndex = 0;

		XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		OPENXR_CHECK(xrAcquireSwapchainImage(colorSwapchainInfo.swapchain, &acquireInfo, &colorImageIndex), "Failed to acquire image from the color swapchain");
		OPENXR_CHECK(xrAcquireSwapchainImage(depthSwapchainInfo.swapchain, &acquireInfo, &depthImageIndex), "Failed to acquire image from the depth swapchain");

		XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		waitInfo.timeout = XR_INFINITE_DURATION;
		OPENXR_CHECK(xrWaitSwapchainImage(colorSwapchainInfo.swapchain, &waitInfo), "Failed to wait for color swapchain image");
		OPENXR_CHECK(xrWaitSwapchainImage(depthSwapchainInfo.swapchain, &waitInfo), "Failed to wait for depth swapchain image");


		const int width = D3DApp::GetApp()->GetWidth();
		const int height = D3DApp::GetApp()->GetHeight();

		renderLayerInfo.layerProjectionViews[i] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
		renderLayerInfo.layerProjectionViews[i].pose = views[i].pose;
		renderLayerInfo.layerProjectionViews[i].fov = views[i].fov;
		renderLayerInfo.layerProjectionViews[i].subImage.swapchain = colorSwapchainInfo.swapchain;
		renderLayerInfo.layerProjectionViews[i].subImage.imageRect.offset.x = 0;
		renderLayerInfo.layerProjectionViews[i].subImage.imageRect.offset.y = 0;
		renderLayerInfo.layerProjectionViews[i].subImage.imageRect.extent.width = width;
		renderLayerInfo.layerProjectionViews[i].subImage.imageRect.extent.height = height;
		renderLayerInfo.layerProjectionViews[i].subImage.imageArrayIndex = 0;
	
		void* colorImage = D3DApp::GetApp()->GetXrSwapchainImage(colorSwapchainInfo.swapchain, colorImageIndex);
		void* depthImage = D3DApp::GetApp()->GetXrSwapchainImage(depthSwapchainInfo.swapchain, depthImageIndex);

		mCurrentAcquiredSwapchainColorImages[i] = (reinterpret_cast<ID3D12Resource*>(colorImage));
		mCurrentAcquiredSwapchainDepthImages[i] = (reinterpret_cast<ID3D12Resource*>(depthImage));


		//views[i].pose.position.z -= 2;
		XMVECTOR scale = XMVectorSet(1.0f, 1.0f, 1.0f, 1.0f);
		XMVECTOR zero = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
		
		XrVector3f offsetPos = views[i].pose.position;
		offsetPos.z += 0;
		XMVECTOR T = XMLoadFloat3(&MathLib::XrVectorToXm(offsetPos));
		XMVECTOR Q = XMLoadFloat4(&MathLib::XrVectorToXm(views[i].pose.orientation));

		ViewMatrix[i] = XMMatrixAffineTransformation(scale, zero, Q, T);
	}

	// Fill out the XrCompositionLayerProjection structure for usage with xrEndFrame().
	renderLayerInfo.layerProjection.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT;
	renderLayerInfo.layerProjection.space = mLocalSpace;
	renderLayerInfo.layerProjection.viewCount = static_cast<uint32_t>(renderLayerInfo.layerProjectionViews.size());
	renderLayerInfo.layerProjection.views = renderLayerInfo.layerProjectionViews.data();
}
