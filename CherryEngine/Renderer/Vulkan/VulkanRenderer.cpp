// The mobile renderer: the Renderer API on Vulkan 1.1 (Android). It implements what the games
// on Android use (Dice/Docs/Android.md), not the D3D12 renderer's whole feature set:
//   A2: device, swapchain (surface loss and recreation), ImGui.
//   A3: meshes and materials, instanced mesh pass, sun shadows, particles, tonemap, FXAA.
// Resources the passes don't draw yet (voxel volumes, light volumes, surface shaders) are kept
// as bookkeeping, so games that create them still run.
//
// Shaders are HLSL in Shaders/Mobile, compiled to SPIR-V at build time (CMakeLists.txt,
// cherry_mobile_shaders) into headers included here.
#include "Renderer/Renderer.h"
#include "Renderer/ResourcePool.h"
#include "Core/Log.h"
#include "imgui.h"

#if defined(__ANDROID__)
#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/native_window.h>
#endif
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

#include "ImGuiVS.spv.h"
#include "ImGuiPS.spv.h"

using namespace DirectX;

namespace
{
	constexpr uint32_t kFramesInFlight = 2;

	const char* VkResultName(VkResult result)
	{
		switch (result)
		{
		case VK_SUCCESS: return "VK_SUCCESS";
		case VK_NOT_READY: return "VK_NOT_READY";
		case VK_TIMEOUT: return "VK_TIMEOUT";
		case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
		case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
		case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
		case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
		case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
		case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
		case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
		case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
		case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
		case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
		case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
		default: return "VkResult";
		}
	}

	bool Check(VkResult result, const char* what)
	{
		if (result >= 0) return true;
		CHERRY_ERROR("Vulkan: %s failed: %s (%d)", what, VkResultName(result), static_cast<int>(result));
		return false;
	}
#define VK_TRY(call) Check((call), #call)

	// SPIR-V from DXC's -Fh header (bytes) into words.
	std::vector<uint32_t> Words(const unsigned char* bytes, size_t size)
	{
		std::vector<uint32_t> words(size / 4);
		std::memcpy(words.data(), bytes, words.size() * 4);
		return words;
	}

	struct Buffer
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkDeviceSize size = 0;
		void* mapped = nullptr;
	};

	struct Image
	{
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		VkFormat format = VK_FORMAT_UNDEFINED;
		uint32_t width = 0, height = 0;
	};

	// An ImGui texture (the font atlas, images games draw).
	struct UiTexture
	{
		Image image;
		VkDescriptorSet set = VK_NULL_HANDLE;
	};

	struct GpuMesh
	{
		BoundingBox bounds;
		size_t bytes = 0;
	};

	struct GpuTexture
	{
		int placeholder = 0;
	};

	struct GpuShader
	{
		std::string error;
	};

	struct FrameData
	{
		VkCommandPool pool = VK_NULL_HANDLE;
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		VkSemaphore acquired = VK_NULL_HANDLE;
		Buffer uiVertices;
		Buffer uiIndices;
		std::vector<std::function<void()>> deletions;   // run once the GPU is done with this frame
	};
}

struct Renderer::Impl
{
	// ---- Context -------------------------------------------------------------------------------
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	VkPhysicalDeviceProperties properties{};
	VkPhysicalDeviceMemoryProperties memoryProperties{};
	VkDevice device = VK_NULL_HANDLE;
	uint32_t queueFamily = 0;
	VkQueue queue = VK_NULL_HANDLE;
	VkCommandPool uploadPool = VK_NULL_HANDLE;
	std::string adapterName = "Vulkan";

	// ---- Surface and swapchain -----------------------------------------------------------------
	void* nativeWindow = nullptr;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkFormat swapFormat = VK_FORMAT_UNDEFINED;
	VkExtent2D swapExtent{};
	std::vector<VkImage> swapImages;
	std::vector<VkImageView> swapViews;
	std::vector<VkFramebuffer> swapFramebuffers;
	std::vector<VkSemaphore> swapRendered;   // per image: rendering done, present may go
	VkRenderPass presentPass = VK_NULL_HANDLE;   // swapchain colour, cleared
	bool swapchainDirty = false;

	// ---- Frames ----------------------------------------------------------------------------
	std::array<FrameData, kFramesInFlight> frames{};
	uint32_t frameIndex = 0;
	uint32_t imageIndex = 0;
	bool frameAcquired = false;
	bool imguiFrameOpen = false;

	// ---- ImGui ------------------------------------------------------------------------------
	VkDescriptorSetLayout uiSetLayout = VK_NULL_HANDLE;
	VkPipelineLayout uiLayout = VK_NULL_HANDLE;
	VkPipeline uiPipeline = VK_NULL_HANDLE;
	VkDescriptorPool uiDescriptorPool = VK_NULL_HANDLE;
	VkSampler linearClamp = VK_NULL_HANDLE;
	bool imguiOwned = false;

	// ---- Resources (bookkeeping until A3 draws them) ------------------------------------------
	ResourcePool<GpuMesh, MeshHandle> meshes;
	ResourcePool<GpuTexture, TextureHandle> textures;
	ResourcePool<MaterialDesc, MaterialHandle> materials;
	ResourcePool<XMUINT3, VoxelVolumeHandle> volumes;
	ResourcePool<GpuShader, SurfaceShaderHandle> shaders;
	ResourcePool<int, LightVolumeHandle> lightVolumes;

	RenderSettings settings;
	std::vector<ParticleInstance> particles;
	std::vector<MeshInstance> meshInstances;
	DebugDraw debug;
	RenderStats stats;
	std::string shaderError;
	uint32_t width = 1, height = 1, viewWidth = 1, viewHeight = 1;

	// ---- Helpers -------------------------------------------------------------------------------
	uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const
	{
		for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
		{
			if ((bits & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & flags) == flags) return i;
		}
		return UINT32_MAX;
	}

	bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, Buffer& out)
	{
		VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		info.size = std::max<VkDeviceSize>(size, 16);
		info.usage = usage;
		info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (!VK_TRY(vkCreateBuffer(device, &info, nullptr, &out.buffer))) return false;
		VkMemoryRequirements requirements;
		vkGetBufferMemoryRequirements(device, out.buffer, &requirements);
		const VkMemoryPropertyFlags flags = hostVisible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
		VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		allocate.allocationSize = requirements.size;
		allocate.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, flags);
		if (allocate.memoryTypeIndex == UINT32_MAX || !VK_TRY(vkAllocateMemory(device, &allocate, nullptr, &out.memory)))
		{
			vkDestroyBuffer(device, out.buffer, nullptr);
			out = {};
			return false;
		}
		vkBindBufferMemory(device, out.buffer, out.memory, 0);
		out.size = info.size;
		if (hostVisible) vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped);
		return true;
	}

	void DestroyBuffer(Buffer& buffer)
	{
		if (buffer.buffer) vkDestroyBuffer(device, buffer.buffer, nullptr);
		if (buffer.memory) vkFreeMemory(device, buffer.memory, nullptr);
		buffer = {};
	}

	bool CreateImage(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, Image& out)
	{
		VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		info.imageType = VK_IMAGE_TYPE_2D;
		info.format = format;
		info.extent = { w, h, 1 };
		info.mipLevels = 1;
		info.arrayLayers = 1;
		info.samples = VK_SAMPLE_COUNT_1_BIT;
		info.tiling = VK_IMAGE_TILING_OPTIMAL;
		info.usage = usage;
		info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (!VK_TRY(vkCreateImage(device, &info, nullptr, &out.image))) return false;
		VkMemoryRequirements requirements;
		vkGetImageMemoryRequirements(device, out.image, &requirements);
		VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		allocate.allocationSize = requirements.size;
		allocate.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (allocate.memoryTypeIndex == UINT32_MAX || !VK_TRY(vkAllocateMemory(device, &allocate, nullptr, &out.memory))) return false;
		vkBindImageMemory(device, out.image, out.memory, 0);
		VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		view.image = out.image;
		view.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view.format = format;
		view.subresourceRange = { aspect, 0, 1, 0, 1 };
		if (!VK_TRY(vkCreateImageView(device, &view, nullptr, &out.view))) return false;
		out.format = format;
		out.width = w;
		out.height = h;
		return true;
	}

	void DestroyImage(Image& image)
	{
		if (image.view) vkDestroyImageView(device, image.view, nullptr);
		if (image.image) vkDestroyImage(device, image.image, nullptr);
		if (image.memory) vkFreeMemory(device, image.memory, nullptr);
		image = {};
	}

	// Records `record` into a one-off command buffer and waits for it (uploads).
	void Submit(const std::function<void(VkCommandBuffer)>& record)
	{
		VkCommandBufferAllocateInfo allocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		allocate.commandPool = uploadPool;
		allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocate.commandBufferCount = 1;
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		if (!VK_TRY(vkAllocateCommandBuffers(device, &allocate, &cmd))) return;
		VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(cmd, &begin);
		record(cmd);
		vkEndCommandBuffer(cmd);
		VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &cmd;
		VK_TRY(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
		vkQueueWaitIdle(queue);
		vkFreeCommandBuffers(device, uploadPool, 1, &cmd);
	}

	static void Barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
		VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
	{
		VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		barrier.oldLayout = from;
		barrier.newLayout = to;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;
		barrier.subresourceRange = { aspect, 0, 1, 0, 1 };
		barrier.srcAccessMask = srcAccess;
		barrier.dstAccessMask = dstAccess;
		vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	}

	// Copies RGBA8 rows into a region of a sampled image (left in SHADER_READ_ONLY_OPTIMAL).
	// `fresh`: the image has no contents yet (UNDEFINED layout).
	void UploadRegion(Image& image, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t* pixels, size_t pitch, bool fresh)
	{
		if (w == 0 || h == 0) return;
		Buffer staging;
		if (!CreateBuffer(static_cast<VkDeviceSize>(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, staging)) return;
		for (uint32_t row = 0; row < h; ++row)
		{
			std::memcpy(static_cast<uint8_t*>(staging.mapped) + static_cast<size_t>(row) * w * 4, pixels + row * pitch, static_cast<size_t>(w) * 4);
		}
		Submit([&](VkCommandBuffer cmd) {
			Barrier(cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, fresh ? 0 : VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
				fresh ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			VkBufferImageCopy copy{};
			copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			copy.imageOffset = { static_cast<int32_t>(x), static_cast<int32_t>(y), 0 };
			copy.imageExtent = { w, h, 1 };
			vkCmdCopyBufferToImage(cmd, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
			Barrier(cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		});
		DestroyBuffer(staging);
	}

	VkShaderModule Module(const std::vector<uint32_t>& words)
	{
		VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		info.codeSize = words.size() * 4;
		info.pCode = words.data();
		VkShaderModule module = VK_NULL_HANDLE;
		VK_TRY(vkCreateShaderModule(device, &info, nullptr, &module));
		return module;
	}

	void Defer(std::function<void()> deletion) { frames[frameIndex].deletions.push_back(std::move(deletion)); }

	// ---- Setup -------------------------------------------------------------------------------
	bool CreateDevice(std::string& error);
	bool CreateSurface(void* window);
	bool CreateSwapchain();
	void DestroySwapchain();
	void DestroySurface();
	bool CreateFrames();
	bool CreateUi();
	void DestroyUi();

	// ---- Frame ---------------------------------------------------------------------------------
	void UpdateUiTextures(ImDrawData* data);
	void DrawUi(VkCommandBuffer cmd, ImDrawData* data);
};

// ================================================================================================
// Setup
// ================================================================================================

bool Renderer::Impl::CreateDevice(std::string& error)
{
	VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app.pApplicationName = "CherryEngine";
	app.pEngineName = "CherryEngine";
	app.apiVersion = VK_API_VERSION_1_1;
	const char* extensions[] = {
		VK_KHR_SURFACE_EXTENSION_NAME,
#if defined(__ANDROID__)
		VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
#endif
	};
	VkInstanceCreateInfo instanceInfo{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	instanceInfo.pApplicationInfo = &app;
	instanceInfo.enabledExtensionCount = static_cast<uint32_t>(std::size(extensions));
	instanceInfo.ppEnabledExtensionNames = extensions;
	if (!VK_TRY(vkCreateInstance(&instanceInfo, nullptr, &instance)))
	{
		error = "Vulkan 1.1 isn't available";
		return false;
	}

	uint32_t count = 0;
	vkEnumeratePhysicalDevices(instance, &count, nullptr);
	std::vector<VkPhysicalDevice> devices(count);
	vkEnumeratePhysicalDevices(instance, &count, devices.data());
	for (VkPhysicalDevice candidate : devices)
	{
		uint32_t families = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
		std::vector<VkQueueFamilyProperties> props(families);
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, props.data());
		for (uint32_t i = 0; i < families; ++i)
		{
			if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
			{
				physical = candidate;
				queueFamily = i;
				break;
			}
		}
		if (physical) break;
	}
	if (!physical)
	{
		error = "no Vulkan device with a graphics queue";
		return false;
	}
	vkGetPhysicalDeviceProperties(physical, &properties);
	vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
	adapterName = properties.deviceName;

	const float priority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	queueInfo.queueFamilyIndex = queueFamily;
	queueInfo.queueCount = 1;
	queueInfo.pQueuePriorities = &priority;
	const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkDeviceCreateInfo deviceInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	deviceInfo.queueCreateInfoCount = 1;
	deviceInfo.pQueueCreateInfos = &queueInfo;
	deviceInfo.enabledExtensionCount = 1;
	deviceInfo.ppEnabledExtensionNames = deviceExtensions;
	if (!VK_TRY(vkCreateDevice(physical, &deviceInfo, nullptr, &device)))
	{
		error = "creating the Vulkan device failed";
		return false;
	}
	vkGetDeviceQueue(device, queueFamily, 0, &queue);

	VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = queueFamily;
	VK_TRY(vkCreateCommandPool(device, &poolInfo, nullptr, &uploadPool));

	CHERRY_LOG("Vulkan %u.%u.%u on %s (driver %u)", VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion),
		VK_VERSION_PATCH(properties.apiVersion), properties.deviceName, properties.driverVersion);
	return true;
}

bool Renderer::Impl::CreateSurface(void* window)
{
	nativeWindow = window;
	if (!window) return false;
#if defined(__ANDROID__)
	VkAndroidSurfaceCreateInfoKHR info{ VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR };
	info.window = static_cast<ANativeWindow*>(window);
	if (!VK_TRY(vkCreateAndroidSurfaceKHR(instance, &info, nullptr, &surface))) return false;
#else
	return false;
#endif
	VkBool32 supported = VK_FALSE;
	vkGetPhysicalDeviceSurfaceSupportKHR(physical, queueFamily, surface, &supported);
	if (!supported)
	{
		CHERRY_ERROR("Vulkan: the graphics queue can't present to this surface");
		return false;
	}
	return CreateSwapchain();
}

bool Renderer::Impl::CreateSwapchain()
{
	swapchainDirty = false;
	if (!surface) return false;
	VkSurfaceCapabilitiesKHR caps{};
	if (!VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps))) return false;

	uint32_t count = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr);
	std::vector<VkSurfaceFormatKHR> formats(count);
	vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data());
	if (formats.empty()) return false;
	// UNORM, not sRGB: the tonemap writes gamma-encoded colour and ImGui's colours are too
	// (as on the D3D12 renderer).
	VkSurfaceFormatKHR chosen = formats[0];
	for (VkFormat wanted : { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM })
	{
		auto it = std::find_if(formats.begin(), formats.end(), [&](const VkSurfaceFormatKHR& f) { return f.format == wanted; });
		if (it != formats.end()) { chosen = *it; break; }
	}

	VkExtent2D extent = caps.currentExtent;
	if (extent.width == UINT32_MAX) extent = { width, height };
	extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
	extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
	if (extent.width == 0 || extent.height == 0) return false;

	uint32_t imageCount = std::max(caps.minImageCount, 3u);
	if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

	VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	for (VkCompositeAlphaFlagBitsKHR candidate : { VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR })
	{
		if (caps.supportedCompositeAlpha & candidate) { alpha = candidate; break; }
	}

	VkSwapchainCreateInfoKHR info{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	info.surface = surface;
	info.minImageCount = imageCount;
	info.imageFormat = chosen.format;
	info.imageColorSpace = chosen.colorSpace;
	info.imageExtent = extent;
	info.imageArrayLayers = 1;
	info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	// The compositor applies the display's rotation (pre-rotating ourselves would save it
	// that work; later).
	info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
	info.compositeAlpha = alpha;
	info.presentMode = VK_PRESENT_MODE_FIFO_KHR;   // always available; vsync
	info.clipped = VK_TRUE;
	info.oldSwapchain = swapchain;
	VkSwapchainKHR created = VK_NULL_HANDLE;
	if (!VK_TRY(vkCreateSwapchainKHR(device, &info, nullptr, &created))) return false;
	DestroySwapchain();   // the old one (retired by oldSwapchain)
	swapchain = created;
	swapExtent = extent;

	const bool formatChanged = chosen.format != swapFormat;
	swapFormat = chosen.format;
	if (formatChanged && presentPass)
	{
		DestroyUi();
		vkDestroyRenderPass(device, presentPass, nullptr);
		presentPass = VK_NULL_HANDLE;
	}
	if (!presentPass)
	{
		VkAttachmentDescription color{};
		color.format = swapFormat;
		color.samples = VK_SAMPLE_COUNT_1_BIT;
		color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &ref;
		VkSubpassDependency dependency{};
		dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
		dependency.dstSubpass = 0;
		dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		VkRenderPassCreateInfo passInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
		passInfo.attachmentCount = 1;
		passInfo.pAttachments = &color;
		passInfo.subpassCount = 1;
		passInfo.pSubpasses = &subpass;
		passInfo.dependencyCount = 1;
		passInfo.pDependencies = &dependency;
		if (!VK_TRY(vkCreateRenderPass(device, &passInfo, nullptr, &presentPass))) return false;
	}
	if (!uiPipeline && !CreateUi()) return false;

	vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
	swapImages.resize(count);
	vkGetSwapchainImagesKHR(device, swapchain, &count, swapImages.data());
	for (VkImage image : swapImages)
	{
		VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		view.image = image;
		view.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view.format = swapFormat;
		view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VkImageView imageView = VK_NULL_HANDLE;
		VK_TRY(vkCreateImageView(device, &view, nullptr, &imageView));
		swapViews.push_back(imageView);
		VkFramebufferCreateInfo fb{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		fb.renderPass = presentPass;
		fb.attachmentCount = 1;
		fb.pAttachments = &imageView;
		fb.width = extent.width;
		fb.height = extent.height;
		fb.layers = 1;
		VkFramebuffer framebuffer = VK_NULL_HANDLE;
		VK_TRY(vkCreateFramebuffer(device, &fb, nullptr, &framebuffer));
		swapFramebuffers.push_back(framebuffer);
		VkSemaphoreCreateInfo semaphore{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		VkSemaphore rendered = VK_NULL_HANDLE;
		VK_TRY(vkCreateSemaphore(device, &semaphore, nullptr, &rendered));
		swapRendered.push_back(rendered);
	}
	CHERRY_LOG("Vulkan swapchain %ux%u, %zu images (transform %u)", extent.width, extent.height, swapImages.size(),
		static_cast<unsigned>(caps.currentTransform));
	return true;
}

void Renderer::Impl::DestroySwapchain()
{
	if (!device) return;
	if (!swapViews.empty() || swapchain) vkDeviceWaitIdle(device);
	for (VkFramebuffer framebuffer : swapFramebuffers) vkDestroyFramebuffer(device, framebuffer, nullptr);
	for (VkImageView view : swapViews) vkDestroyImageView(device, view, nullptr);
	for (VkSemaphore semaphore : swapRendered) vkDestroySemaphore(device, semaphore, nullptr);
	swapFramebuffers.clear();
	swapViews.clear();
	swapRendered.clear();
	swapImages.clear();
	if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
	swapchain = VK_NULL_HANDLE;
}

void Renderer::Impl::DestroySurface()
{
	DestroySwapchain();
	if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
	surface = VK_NULL_HANDLE;
	nativeWindow = nullptr;
}

bool Renderer::Impl::CreateFrames()
{
	for (FrameData& frame : frames)
	{
		VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
		poolInfo.queueFamilyIndex = queueFamily;
		if (!VK_TRY(vkCreateCommandPool(device, &poolInfo, nullptr, &frame.pool))) return false;
		VkCommandBufferAllocateInfo allocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		allocate.commandPool = frame.pool;
		allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocate.commandBufferCount = 1;
		if (!VK_TRY(vkAllocateCommandBuffers(device, &allocate, &frame.cmd))) return false;
		VkFenceCreateInfo fence{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		if (!VK_TRY(vkCreateFence(device, &fence, nullptr, &frame.fence))) return false;
		VkSemaphoreCreateInfo semaphore{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		if (!VK_TRY(vkCreateSemaphore(device, &semaphore, nullptr, &frame.acquired))) return false;
	}
	return true;
}

bool Renderer::Impl::CreateUi()
{
	if (!linearClamp)
	{
		VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
		sampler.magFilter = VK_FILTER_LINEAR;
		sampler.minFilter = VK_FILTER_LINEAR;
		sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler.maxLod = VK_LOD_CLAMP_NONE;
		if (!VK_TRY(vkCreateSampler(device, &sampler, nullptr, &linearClamp))) return false;
	}
	if (!uiSetLayout)
	{
		VkDescriptorSetLayoutBinding binding{};
		binding.binding = 0;
		binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		binding.descriptorCount = 1;
		binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		VkDescriptorSetLayoutCreateInfo layout{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
		layout.bindingCount = 1;
		layout.pBindings = &binding;
		if (!VK_TRY(vkCreateDescriptorSetLayout(device, &layout, nullptr, &uiSetLayout))) return false;

		VkPushConstantRange push{ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 4 };
		VkPipelineLayoutCreateInfo pipelineLayout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
		pipelineLayout.setLayoutCount = 1;
		pipelineLayout.pSetLayouts = &uiSetLayout;
		pipelineLayout.pushConstantRangeCount = 1;
		pipelineLayout.pPushConstantRanges = &push;
		if (!VK_TRY(vkCreatePipelineLayout(device, &pipelineLayout, nullptr, &uiLayout))) return false;

		VkDescriptorPoolSize size{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256 };
		VkDescriptorPoolCreateInfo pool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
		pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		pool.maxSets = 256;
		pool.poolSizeCount = 1;
		pool.pPoolSizes = &size;
		if (!VK_TRY(vkCreateDescriptorPool(device, &pool, nullptr, &uiDescriptorPool))) return false;
	}

	const std::vector<uint32_t> vsWords = Words(kSpirv_ImGuiVS, sizeof(kSpirv_ImGuiVS));
	const std::vector<uint32_t> psWords = Words(kSpirv_ImGuiPS, sizeof(kSpirv_ImGuiPS));
	VkShaderModule vs = Module(vsWords), ps = Module(psWords);
	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "VSMain", nullptr };
	stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps, "PSMain", nullptr };

	VkVertexInputBindingDescription binding{ 0, sizeof(ImDrawVert), VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attributes[3] = {
		{ 0, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(ImDrawVert, pos)) },
		{ 1, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(ImDrawVert, uv)) },
		{ 2, 0, VK_FORMAT_R8G8B8A8_UNORM, static_cast<uint32_t>(offsetof(ImDrawVert, col)) },
	};
	VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vertexInput.vertexBindingDescriptionCount = 1;
	vertexInput.pVertexBindingDescriptions = &binding;
	vertexInput.vertexAttributeDescriptionCount = 3;
	vertexInput.pVertexAttributeDescriptions = attributes;
	VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState blendAttachment{};
	blendAttachment.blendEnable = VK_TRUE;
	blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
	blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
	blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	blend.attachmentCount = 1;
	blend.pAttachments = &blendAttachment;
	VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dynamic.dynamicStateCount = 2;
	dynamic.pDynamicStates = dynamicStates;

	VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	info.stageCount = 2;
	info.pStages = stages;
	info.pVertexInputState = &vertexInput;
	info.pInputAssemblyState = &assembly;
	info.pViewportState = &viewport;
	info.pRasterizationState = &raster;
	info.pMultisampleState = &multisample;
	info.pDepthStencilState = &depth;
	info.pColorBlendState = &blend;
	info.pDynamicState = &dynamic;
	info.layout = uiLayout;
	info.renderPass = presentPass;
	const bool ok = VK_TRY(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &uiPipeline));
	vkDestroyShaderModule(device, vs, nullptr);
	vkDestroyShaderModule(device, ps, nullptr);
	return ok;
}

void Renderer::Impl::DestroyUi()
{
	if (uiPipeline) vkDestroyPipeline(device, uiPipeline, nullptr);
	uiPipeline = VK_NULL_HANDLE;
}

// ================================================================================================
// ImGui
// ================================================================================================

void Renderer::Impl::UpdateUiTextures(ImDrawData* data)
{
	if (!data || !data->Textures) return;
	for (ImTextureData* texture : *data->Textures)
	{
		if (texture->Status == ImTextureStatus_OK) continue;
		if (texture->Status == ImTextureStatus_WantCreate)
		{
			UiTexture* ui = new UiTexture();
			if (!CreateImage(static_cast<uint32_t>(texture->Width), static_cast<uint32_t>(texture->Height), VK_FORMAT_R8G8B8A8_UNORM,
				VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, ui->image))
			{
				delete ui;
				continue;
			}
			UploadRegion(ui->image, 0, 0, ui->image.width, ui->image.height, static_cast<const uint8_t*>(texture->GetPixels()),
				static_cast<size_t>(texture->GetPitch()), true);
			VkDescriptorSetAllocateInfo allocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
			allocate.descriptorPool = uiDescriptorPool;
			allocate.descriptorSetCount = 1;
			allocate.pSetLayouts = &uiSetLayout;
			VK_TRY(vkAllocateDescriptorSets(device, &allocate, &ui->set));
			VkDescriptorImageInfo image{ linearClamp, ui->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
			VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
			write.dstSet = ui->set;
			write.descriptorCount = 1;
			write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			write.pImageInfo = &image;
			vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
			texture->SetTexID(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(ui->set)));
			texture->BackendUserData = ui;
			texture->SetStatus(ImTextureStatus_OK);
		}
		else if (texture->Status == ImTextureStatus_WantUpdates)
		{
			UiTexture* ui = static_cast<UiTexture*>(texture->BackendUserData);
			if (ui)
			{
				const ImTextureRect& r = texture->UpdateRect;
				UploadRegion(ui->image, r.x, r.y, r.w, r.h, static_cast<const uint8_t*>(texture->GetPixelsAt(r.x, r.y)),
					static_cast<size_t>(texture->GetPitch()), false);
			}
			texture->SetStatus(ImTextureStatus_OK);
		}
		else if (texture->Status == ImTextureStatus_WantDestroy && texture->UnusedFrames >= static_cast<int>(kFramesInFlight))
		{
			if (UiTexture* ui = static_cast<UiTexture*>(texture->BackendUserData))
			{
				Defer([this, ui]() {
					vkFreeDescriptorSets(device, uiDescriptorPool, 1, &ui->set);
					DestroyImage(ui->image);
					delete ui;
				});
			}
			texture->SetTexID(ImTextureID_Invalid);
			texture->BackendUserData = nullptr;
			texture->SetStatus(ImTextureStatus_Destroyed);
		}
	}
}

void Renderer::Impl::DrawUi(VkCommandBuffer cmd, ImDrawData* data)
{
	if (!data || data->TotalVtxCount == 0 || data->DisplaySize.x <= 0.0f || data->DisplaySize.y <= 0.0f) return;
	FrameData& frame = frames[frameIndex];
	const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(data->TotalVtxCount) * sizeof(ImDrawVert);
	const VkDeviceSize indexBytes = static_cast<VkDeviceSize>(data->TotalIdxCount) * sizeof(ImDrawIdx);
	if (frame.uiVertices.size < vertexBytes)
	{
		Buffer old = frame.uiVertices;
		Defer([this, old]() mutable { DestroyBuffer(old); });
		CreateBuffer(vertexBytes * 2, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true, frame.uiVertices);
	}
	if (frame.uiIndices.size < indexBytes)
	{
		Buffer old = frame.uiIndices;
		Defer([this, old]() mutable { DestroyBuffer(old); });
		CreateBuffer(indexBytes * 2, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true, frame.uiIndices);
	}
	if (!frame.uiVertices.mapped || !frame.uiIndices.mapped) return;
	ImDrawVert* vertices = static_cast<ImDrawVert*>(frame.uiVertices.mapped);
	ImDrawIdx* indices = static_cast<ImDrawIdx*>(frame.uiIndices.mapped);
	for (const ImDrawList* list : data->CmdLists)
	{
		std::memcpy(vertices, list->VtxBuffer.Data, list->VtxBuffer.Size * sizeof(ImDrawVert));
		std::memcpy(indices, list->IdxBuffer.Data, list->IdxBuffer.Size * sizeof(ImDrawIdx));
		vertices += list->VtxBuffer.Size;
		indices += list->IdxBuffer.Size;
	}

	auto setup = [&]() {
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, uiPipeline);
		const VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &frame.uiVertices.buffer, &offset);
		vkCmdBindIndexBuffer(cmd, frame.uiIndices.buffer, 0, sizeof(ImDrawIdx) == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
		const VkViewport vp{ 0.0f, 0.0f, static_cast<float>(swapExtent.width), static_cast<float>(swapExtent.height), 0.0f, 1.0f };
		vkCmdSetViewport(cmd, 0, 1, &vp);
		// ImGui's coordinates are window pixels; the swapchain may differ in size (scale to it).
		const float sx = 2.0f / data->DisplaySize.x, sy = 2.0f / data->DisplaySize.y;
		const float constants[4] = { sx, sy, -1.0f - data->DisplayPos.x * sx, -1.0f - data->DisplayPos.y * sy };
		vkCmdPushConstants(cmd, uiLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(constants), constants);
	};
	setup();

	const float scaleX = swapExtent.width / data->DisplaySize.x, scaleY = swapExtent.height / data->DisplaySize.y;
	int vertexOffset = 0;
	uint32_t indexOffset = 0;
	VkDescriptorSet bound = VK_NULL_HANDLE;
	for (const ImDrawList* list : data->CmdLists)
	{
		for (const ImDrawCmd& draw : list->CmdBuffer)
		{
			if (draw.UserCallback)
			{
				if (draw.UserCallback == ImDrawCallback_ResetRenderState) { setup(); bound = VK_NULL_HANDLE; }
				else draw.UserCallback(list, &draw);
				continue;
			}
			const float x0 = (draw.ClipRect.x - data->DisplayPos.x) * scaleX, y0 = (draw.ClipRect.y - data->DisplayPos.y) * scaleY;
			const float x1 = (draw.ClipRect.z - data->DisplayPos.x) * scaleX, y1 = (draw.ClipRect.w - data->DisplayPos.y) * scaleY;
			const int32_t cx0 = std::max(0, static_cast<int32_t>(x0)), cy0 = std::max(0, static_cast<int32_t>(y0));
			const int32_t cx1 = std::min(static_cast<int32_t>(swapExtent.width), static_cast<int32_t>(x1));
			const int32_t cy1 = std::min(static_cast<int32_t>(swapExtent.height), static_cast<int32_t>(y1));
			if (cx1 <= cx0 || cy1 <= cy0) continue;
			const VkRect2D scissor{ { cx0, cy0 }, { static_cast<uint32_t>(cx1 - cx0), static_cast<uint32_t>(cy1 - cy0) } };
			vkCmdSetScissor(cmd, 0, 1, &scissor);
			const VkDescriptorSet set = reinterpret_cast<VkDescriptorSet>(static_cast<uintptr_t>(draw.GetTexID()));
			if (!set) continue;
			if (set != bound)
			{
				vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, uiLayout, 0, 1, &set, 0, nullptr);
				bound = set;
			}
			vkCmdDrawIndexed(cmd, draw.ElemCount, 1, indexOffset + draw.IdxOffset, vertexOffset + static_cast<int>(draw.VtxOffset), 0);
		}
		vertexOffset += list->VtxBuffer.Size;
		indexOffset += static_cast<uint32_t>(list->IdxBuffer.Size);
	}
}

// ================================================================================================
// Renderer API
// ================================================================================================

Renderer::Renderer() : mImpl(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;

MeshHandle Renderer::CreateMesh(const MeshData& data, MeshFormat format)
{
	GpuMesh mesh;
	XMFLOAT3 lo{ FLT_MAX, FLT_MAX, FLT_MAX }, hi{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
	for (const Vertex& v : data.vertices)
	{
		lo = { std::min(lo.x, v.position.x), std::min(lo.y, v.position.y), std::min(lo.z, v.position.z) };
		hi = { std::max(hi.x, v.position.x), std::max(hi.y, v.position.y), std::max(hi.z, v.position.z) };
	}
	if (!data.vertices.empty()) BoundingBox::CreateFromPoints(mesh.bounds, XMLoadFloat3(&lo), XMLoadFloat3(&hi));
	mesh.bytes = data.vertices.size() * (format == MeshFormat::Compact ? 28 : sizeof(Vertex)) + data.indices.size() * sizeof(uint32_t);
	return mImpl->meshes.Add(std::move(mesh));
}
size_t Renderer::GetMeshBytes(MeshHandle mesh) const { const GpuMesh* m = mImpl->meshes.Get(mesh); return m ? m->bytes : 0; }
void Renderer::DestroyMesh(MeshHandle mesh) { mImpl->meshes.Remove(mesh); }
const BoundingBox* Renderer::GetMeshBounds(MeshHandle mesh) const { const GpuMesh* m = mImpl->meshes.Get(mesh); return m ? &m->bounds : nullptr; }

TextureHandle Renderer::LoadTexture(const std::wstring&, bool) { return mImpl->textures.Add(GpuTexture{}); }
TextureHandle Renderer::CreateTexture(uint32_t, uint32_t, const uint8_t*, bool) { return mImpl->textures.Add(GpuTexture{}); }
void Renderer::DestroyTexture(TextureHandle texture) { mImpl->textures.Remove(texture); }

MaterialHandle Renderer::CreateMaterial(const MaterialDesc& desc) { return mImpl->materials.Add(MaterialDesc(desc)); }
void Renderer::SetMaterial(MaterialHandle material, const MaterialDesc& desc) { if (MaterialDesc* m = mImpl->materials.Get(material)) *m = desc; }
const MaterialDesc* Renderer::GetMaterial(MaterialHandle material) const { return mImpl->materials.Get(material); }
void Renderer::DestroyMaterial(MaterialHandle material) { mImpl->materials.Remove(material); }

SurfaceShaderHandle Renderer::CreateSurfaceShader(const std::string&, const std::string&, const SurfaceShaderOptions&, std::function<void(bool)> done)
{
	const SurfaceShaderHandle handle = mImpl->shaders.Add(GpuShader{});
	if (done) done(true);
	return handle;
}
void Renderer::SetSurfaceShaderSource(SurfaceShaderHandle, const std::string&, const SurfaceShaderOptions&, std::function<void(bool)> done) { if (done) done(true); }
bool Renderer::IsSurfaceShaderCompiling(SurfaceShaderHandle) const { return false; }
void Renderer::DestroySurfaceShader(SurfaceShaderHandle shader) { mImpl->shaders.Remove(shader); }
const std::string& Renderer::SurfaceShaderError(SurfaceShaderHandle shader) const
{
	static const std::string none;
	const GpuShader* s = mImpl->shaders.Get(shader);
	return s ? s->error : none;
}

VoxelVolumeHandle Renderer::CreateVoxelVolume(const VoxelVolumeDesc& desc) { return mImpl->volumes.Add(XMUINT3(desc.size)); }
bool Renderer::UpdateVoxelVolume(VoxelVolumeHandle volume, const XMUINT3&, const XMUINT3&, const uint8_t*) { return mImpl->volumes.IsValid(volume); }
void Renderer::DestroyVoxelVolume(VoxelVolumeHandle volume) { mImpl->volumes.Remove(volume); }
XMUINT3 Renderer::GetVoxelVolumeSize(VoxelVolumeHandle volume) const { const XMUINT3* s = mImpl->volumes.Get(volume); return s ? *s : XMUINT3{ 0, 0, 0 }; }

LightVolumeHandle Renderer::CreateLightVolume(const XMUINT3&, const uint8_t*) { return mImpl->lightVolumes.Add(0); }
bool Renderer::UpdateLightVolume(LightVolumeHandle volume, const XMUINT3&, const XMUINT3&, const uint8_t*) { return mImpl->lightVolumes.IsValid(volume); }
void Renderer::DestroyLightVolume(LightVolumeHandle volume) { mImpl->lightVolumes.Remove(volume); }

RenderSettings& Renderer::Settings() { return mImpl->settings; }
std::vector<ParticleInstance>& Renderer::Particles() { return mImpl->particles; }
std::vector<MeshInstance>& Renderer::MeshInstances() { return mImpl->meshInstances; }
DebugDraw& Renderer::Debug() { return mImpl->debug; }
const RenderStats& Renderer::Stats() const { return mImpl->stats; }
const std::string& Renderer::AdapterName() const { return mImpl->adapterName; }
void Renderer::ReloadShaders() {}
const std::string& Renderer::ShaderError() const { return mImpl->shaderError; }

bool Renderer::Initialize(void* nativeWindow, uint32_t width, uint32_t height, std::string& error)
{
	Impl& r = *mImpl;
	r.width = r.viewWidth = std::max(width, 1u);
	r.height = r.viewHeight = std::max(height, 1u);
	if (!r.CreateDevice(error) || !r.CreateFrames()) return false;

	IMGUI_CHECKVERSION();
	if (!ImGui::GetCurrentContext())
	{
		ImGui::CreateContext();
		r.imguiOwned = true;
	}
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
	io.BackendRendererName = "cherry_vulkan";
	io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
	ImGui::StyleColorsDark();

	if (nativeWindow && !r.CreateSurface(nativeWindow)) CHERRY_WARN("Vulkan: no swapchain yet");
	return true;
}

void Renderer::Shutdown()
{
	Impl& r = *mImpl;
	if (!r.device) return;
	vkDeviceWaitIdle(r.device);
	// ImGui's textures (the font atlas) go with the context's last frame data.
	if (ImGui::GetCurrentContext())
	{
		for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
		{
			if (UiTexture* ui = static_cast<UiTexture*>(texture->BackendUserData))
			{
				r.DestroyImage(ui->image);
				delete ui;
				texture->BackendUserData = nullptr;
				texture->SetTexID(ImTextureID_Invalid);
				texture->SetStatus(ImTextureStatus_Destroyed);
			}
		}
	}
	for (FrameData& frame : r.frames)
	{
		for (auto& deletion : frame.deletions) deletion();
		frame.deletions.clear();
		r.DestroyBuffer(frame.uiVertices);
		r.DestroyBuffer(frame.uiIndices);
		if (frame.acquired) vkDestroySemaphore(r.device, frame.acquired, nullptr);
		if (frame.fence) vkDestroyFence(r.device, frame.fence, nullptr);
		if (frame.pool) vkDestroyCommandPool(r.device, frame.pool, nullptr);
		frame = {};
	}
	r.DestroySurface();
	r.DestroyUi();
	if (r.presentPass) vkDestroyRenderPass(r.device, r.presentPass, nullptr);
	if (r.uiDescriptorPool) vkDestroyDescriptorPool(r.device, r.uiDescriptorPool, nullptr);
	if (r.uiLayout) vkDestroyPipelineLayout(r.device, r.uiLayout, nullptr);
	if (r.uiSetLayout) vkDestroyDescriptorSetLayout(r.device, r.uiSetLayout, nullptr);
	if (r.linearClamp) vkDestroySampler(r.device, r.linearClamp, nullptr);
	if (r.uploadPool) vkDestroyCommandPool(r.device, r.uploadPool, nullptr);
	vkDestroyDevice(r.device, nullptr);
	vkDestroyInstance(r.instance, nullptr);
	r.device = VK_NULL_HANDLE;
	r.instance = VK_NULL_HANDLE;
	if (r.imguiOwned && ImGui::GetCurrentContext()) ImGui::DestroyContext();
	r.imguiOwned = false;
}

void Renderer::OnWindowResized(uint32_t width, uint32_t height)
{
	mImpl->width = std::max(width, 1u);
	mImpl->height = std::max(height, 1u);
	mImpl->swapchainDirty = true;
}

void Renderer::SetNativeWindow(void* nativeWindow, uint32_t width, uint32_t height)
{
	Impl& r = *mImpl;
	r.width = std::max(width, 1u);
	r.height = std::max(height, 1u);
	if (nativeWindow == r.nativeWindow && r.surface) { r.swapchainDirty = true; return; }
	r.DestroySurface();
	if (nativeWindow) r.CreateSurface(nativeWindow);
	CHERRY_LOG("Vulkan: surface %s", nativeWindow ? "recreated" : "released");
}

void Renderer::SetViewSize(uint32_t width, uint32_t height) { mImpl->viewWidth = std::max(width, 1u); mImpl->viewHeight = std::max(height, 1u); }
uint32_t Renderer::ViewWidth() const { return mImpl->viewWidth; }
uint32_t Renderer::ViewHeight() const { return mImpl->viewHeight; }
unsigned long long Renderer::ViewTextureId() const { return 0; }
unsigned long long Renderer::RenderPreview(MeshHandle, MaterialHandle, uint32_t, float, float) { return 0; }

void Renderer::BeginFrame(float, float deltaTime)
{
	Impl& r = *mImpl;
	r.frameAcquired = false;

	if (r.surface && (r.swapchainDirty || !r.swapchain)) r.CreateSwapchain();
	FrameData& frame = r.frames[r.frameIndex];
	vkWaitForFences(r.device, 1, &frame.fence, VK_TRUE, UINT64_MAX);
	for (auto& deletion : frame.deletions) deletion();
	frame.deletions.clear();

	if (r.swapchain)
	{
		VkResult result = vkAcquireNextImageKHR(r.device, r.swapchain, UINT64_MAX, frame.acquired, VK_NULL_HANDLE, &r.imageIndex);
		if (result == VK_ERROR_OUT_OF_DATE_KHR)
		{
			r.CreateSwapchain();
			if (r.swapchain) result = vkAcquireNextImageKHR(r.device, r.swapchain, UINT64_MAX, frame.acquired, VK_NULL_HANDLE, &r.imageIndex);
		}
		// SUBOPTIMAL here means "not pre-rotated" on Android (the compositor rotates for us);
		// only OUT_OF_DATE and resizes rebuild the swapchain.
		r.frameAcquired = result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
		if (!r.frameAcquired) Check(result, "vkAcquireNextImageKHR");
	}

	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2(static_cast<float>(r.width), static_cast<float>(r.height));
	io.DeltaTime = deltaTime > 0.0f ? deltaTime : 1.0f / 60.0f;
	ImGui::NewFrame();
	r.imguiFrameOpen = true;
}

void Renderer::RenderScene(Scene&, const RenderView&)
{
	Impl& r = *mImpl;
	r.stats = {};
	r.stats.instancesSubmitted = static_cast<uint32_t>(r.meshInstances.size());
	r.meshInstances.clear();
	r.particles.clear();
}

void Renderer::EndFrame(bool)
{
	Impl& r = *mImpl;
	if (!r.imguiFrameOpen) return;
	ImGui::Render();
	r.imguiFrameOpen = false;
	ImDrawData* data = ImGui::GetDrawData();
	r.UpdateUiTextures(data);
	r.debug.Clear();
	if (!r.frameAcquired) return;

	FrameData& frame = r.frames[r.frameIndex];
	vkResetFences(r.device, 1, &frame.fence);
	vkResetCommandPool(r.device, frame.pool, 0);
	VkCommandBuffer cmd = frame.cmd;
	VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &begin);

	// The sky colour is linear; the swapchain holds gamma-encoded colour.
	auto encode = [](float linear) { return std::pow(std::clamp(linear, 0.0f, 1.0f), 1.0f / 2.2f); };
	VkClearValue clear{};
	clear.color = { { encode(r.settings.clearColor.x), encode(r.settings.clearColor.y), encode(r.settings.clearColor.z), 1.0f } };
	VkRenderPassBeginInfo pass{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	pass.renderPass = r.presentPass;
	pass.framebuffer = r.swapFramebuffers[r.imageIndex];
	pass.renderArea = { { 0, 0 }, r.swapExtent };
	pass.clearValueCount = 1;
	pass.pClearValues = &clear;
	vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
	r.DrawUi(cmd, data);
	vkCmdEndRenderPass(cmd);
	vkEndCommandBuffer(cmd);

	const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.waitSemaphoreCount = 1;
	submit.pWaitSemaphores = &frame.acquired;
	submit.pWaitDstStageMask = &waitStage;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores = &r.swapRendered[r.imageIndex];
	VK_TRY(vkQueueSubmit(r.queue, 1, &submit, frame.fence));

	VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores = &r.swapRendered[r.imageIndex];
	present.swapchainCount = 1;
	present.pSwapchains = &r.swapchain;
	present.pImageIndices = &r.imageIndex;
	const VkResult result = vkQueuePresentKHR(r.queue, &present);
	if (result == VK_ERROR_OUT_OF_DATE_KHR) r.swapchainDirty = true;
	else if (result != VK_SUBOPTIMAL_KHR) Check(result, "vkQueuePresentKHR");

	r.frameIndex = (r.frameIndex + 1) % kFramesInFlight;
	r.stats.viewWidth = r.viewWidth;
	r.stats.viewHeight = r.viewHeight;
}
