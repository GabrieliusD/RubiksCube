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
#include "Scene/Scene.h"
#include "Scene/Components.h"
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
#include "MeshVS.spv.h"
#include "MeshPS.spv.h"
#include "ShadowVS.spv.h"
#include "ParticlesVS.spv.h"
#include "ParticlesPS.spv.h"
#include "PostVS.spv.h"
#include "TonemapPS.spv.h"
#include "CopyPS.spv.h"
#include "FxaaPS.spv.h"

using namespace DirectX;

namespace
{
	constexpr uint32_t kFramesInFlight = 2;
	constexpr uint32_t kShadowMapSize = 2048;
	constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
	constexpr VkFormat kLdrFormat = VK_FORMAT_R8G8B8A8_UNORM;

	// The C++ side of Shaders/Mobile/Common.hlsli.
	struct FrameConstants
	{
		XMFLOAT4X4 viewProj;
		XMFLOAT4X4 shadowViewProj;
		XMFLOAT4X4 shadowUv;
		XMFLOAT4 eye;             // w: shadows on
		XMFLOAT4 sunDirection;    // w: shadow normal bias (texels)
		XMFLOAT4 sunStrength;     // w: one shadow texel in metres
		XMFLOAT4 ambientSky;
		XMFLOAT4 ambientGround;
		XMFLOAT4 cameraRight;
		XMFLOAT4 cameraUp;        // w: 1 / shadow map size
	};

	struct InstanceGpu
	{
		XMFLOAT4X4 world;
		XMFLOAT4X4 normalMatrix;
		XMFLOAT4 tint;
	};

	struct ParticleGpu
	{
		XMFLOAT4 positionSize;
		XMFLOAT4 color;
		XMFLOAT4 params;          // emissive, rotation, square
	};

	struct DrawConstants
	{
		XMFLOAT4 baseColor;
		float roughness;
		float metallic;
		float f0;
		uint32_t firstInstance;
	};

	struct PostConstants
	{
		float texelX, texelY;
		float exposure;
		uint32_t toneCurve;
	};

	// One run of instances with the same mesh and material.
	struct DrawRun
	{
		MeshHandle mesh;
		MaterialHandle material;
		uint32_t first = 0;
		uint32_t count = 0;
	};

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
		Buffer vertices;          // Vertex (48 bytes), whatever MeshFormat asked for
		Buffer indices;           // uint32
		uint32_t indexCount = 0;
	};

	// A material texture: the image with its mips and the descriptor set materials bind it with.
	struct GpuTexture
	{
		Image image;
		VkDescriptorSet set = VK_NULL_HANDLE;
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
		Buffer constants;         // FrameConstants
		Buffer instances;         // InstanceGpu[]
		Buffer particles;         // ParticleGpu[]
		VkDescriptorSet set = VK_NULL_HANDLE;   // set 0 of the scene passes
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

	// ---- Scene passes -------------------------------------------------------------------------
	bool anisotropy = false;
	bool depthBiasClamp = false;
	VkFormat depthFormat = VK_FORMAT_UNDEFINED;
	VkFormat shadowFormat = VK_FORMAT_UNDEFINED;
	VkSampler materialSampler = VK_NULL_HANDLE;   // linear, mips, repeat, anisotropic
	VkSampler shadowSampler = VK_NULL_HANDLE;     // comparison
	VkDescriptorSetLayout frameSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool framePool = VK_NULL_HANDLE;
	VkPipelineLayout sceneLayout = VK_NULL_HANDLE;   // frame set + texture set, DrawConstants
	VkPipelineLayout postLayout = VK_NULL_HANDLE;    // texture set, PostConstants
	VkRenderPass shadowPass = VK_NULL_HANDLE;
	VkRenderPass scenePass = VK_NULL_HANDLE;
	VkRenderPass ldrPass = VK_NULL_HANDLE;
	VkPipeline shadowPipeline = VK_NULL_HANDLE;
	VkPipeline meshPipeline = VK_NULL_HANDLE;
	VkPipeline particleBlendPipeline = VK_NULL_HANDLE;
	VkPipeline particleAddPipeline = VK_NULL_HANDLE;
	VkPipeline tonemapPipeline = VK_NULL_HANDLE;
	VkPipeline fxaaPipeline = VK_NULL_HANDLE;      // present pass
	VkPipeline copyPipeline = VK_NULL_HANDLE;      // present pass
	Image shadowMap;
	VkFramebuffer shadowFramebuffer = VK_NULL_HANDLE;
	Image hdr, depth, ldr;                         // the view's targets
	VkFramebuffer sceneFramebuffer = VK_NULL_HANDLE;
	VkFramebuffer ldrFramebuffer = VK_NULL_HANDLE;
	VkDescriptorSet hdrSet = VK_NULL_HANDLE;       // the HDR target, sampled by the tonemap
	VkDescriptorSet ldrSet = VK_NULL_HANDLE;       // the LDR target, sampled by FXAA / copy
	TextureHandle whiteTexture;

	// What RenderScene prepared for EndFrame to record.
	bool sceneReady = false;
	std::vector<DrawRun> runs;
	std::vector<DrawRun> shadowRuns;
	std::vector<InstanceGpu> instanceData;
	std::vector<ParticleGpu> particleData;
	uint32_t coveringParticles = 0;               // the first ones; the rest are additive
	bool warnedDds = false;

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

	bool CreateImage(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, Image& out, uint32_t mips = 1)
	{
		VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		info.imageType = VK_IMAGE_TYPE_2D;
		info.format = format;
		info.extent = { w, h, 1 };
		info.mipLevels = mips;
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
		view.subresourceRange = { aspect, 0, mips, 0, 1 };
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

	bool CreateScene();
	void DestroyScene();
	bool EnsureTargets(uint32_t w, uint32_t h);
	void DestroyTargets();
	VkRenderPass MakePass(VkFormat color, VkFormat depthFmt, VkImageLayout colorFinal, bool clearColor);
	struct PipelineDesc
	{
		const unsigned char* vs = nullptr; size_t vsSize = 0; const char* vsEntry = "VSMain";
		const unsigned char* ps = nullptr; size_t psSize = 0; const char* psEntry = "PSMain";
		VkPipelineLayout layout = VK_NULL_HANDLE;
		VkRenderPass pass = VK_NULL_HANDLE;
		enum class Vertices { None, Mesh, Positions } vertices = Vertices::None;
		enum class Blend { None, Alpha, Additive } blend = Blend::None;
		bool depthTest = false, depthWrite = false, depthBias = false, color = true;
	};
	VkPipeline MakePipeline(const PipelineDesc& desc);
	VkDescriptorSet MakeTextureSet(VkImageView view, VkSampler sampler);
	void SetTextureSet(VkDescriptorSet set, VkImageView view, VkSampler sampler, VkImageLayout layout);
	bool EnsureBuffer(Buffer& buffer, VkDeviceSize bytes, VkBufferUsageFlags usage);
	void SetupShadows(const RenderView& view, FrameConstants& constants);
	GpuTexture* TextureOf(MaterialHandle material);

	// ---- Frame ---------------------------------------------------------------------------------
	void UpdateUiTextures(ImDrawData* data);
	void DrawUi(VkCommandBuffer cmd, ImDrawData* data);
	void RecordScene(VkCommandBuffer cmd);
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
	VkPhysicalDeviceFeatures supported{};
	vkGetPhysicalDeviceFeatures(physical, &supported);
	VkPhysicalDeviceFeatures enabled{};
	enabled.samplerAnisotropy = supported.samplerAnisotropy;
	enabled.depthBiasClamp = supported.depthBiasClamp;
	anisotropy = supported.samplerAnisotropy == VK_TRUE;
	depthBiasClamp = supported.depthBiasClamp == VK_TRUE;
	VkDeviceCreateInfo deviceInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	deviceInfo.queueCreateInfoCount = 1;
	deviceInfo.pQueueCreateInfos = &queueInfo;
	deviceInfo.enabledExtensionCount = 1;
	deviceInfo.ppEnabledExtensionNames = deviceExtensions;
	deviceInfo.pEnabledFeatures = &enabled;
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

	// Depth formats: the first one the device can render to (and, for shadows, sample).
	auto pick = [&](std::initializer_list<VkFormat> candidates, VkFormatFeatureFlags needed) {
		for (VkFormat format : candidates)
		{
			VkFormatProperties props;
			vkGetPhysicalDeviceFormatProperties(physical, format, &props);
			if ((props.optimalTilingFeatures & needed) == needed) return format;
		}
		return VK_FORMAT_UNDEFINED;
	};
	depthFormat = pick({ VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM },
		VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
	shadowFormat = pick({ VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM },
		VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
	if (!shadowFormat)
		shadowFormat = pick({ VK_FORMAT_D16_UNORM, VK_FORMAT_D32_SFLOAT }, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
	if (!depthFormat || !shadowFormat)
	{
		error = "no usable depth format";
		return false;
	}

	CHERRY_LOG("Vulkan %u.%u.%u on %s (driver %u; anisotropy %s)", VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion),
		VK_VERSION_PATCH(properties.apiVersion), properties.deviceName, properties.driverVersion, anisotropy ? "yes" : "no");
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
	bool ok = VK_TRY(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &uiPipeline));
	vkDestroyShaderModule(device, vs, nullptr);
	vkDestroyShaderModule(device, ps, nullptr);

	// The view into the swapchain: FXAA, or a plain copy.
	PipelineDesc post;
	post.vs = kSpirv_PostVS; post.vsSize = sizeof(kSpirv_PostVS);
	post.layout = postLayout;
	post.pass = presentPass;
	post.ps = kSpirv_FxaaPS; post.psSize = sizeof(kSpirv_FxaaPS); post.psEntry = "FxaaPS";
	fxaaPipeline = MakePipeline(post);
	post.ps = kSpirv_CopyPS; post.psSize = sizeof(kSpirv_CopyPS); post.psEntry = "CopyPS";
	copyPipeline = MakePipeline(post);
	return ok && fxaaPipeline && copyPipeline;
}

void Renderer::Impl::DestroyUi()
{
	for (VkPipeline* pipeline : { &uiPipeline, &fxaaPipeline, &copyPipeline })
	{
		if (*pipeline) vkDestroyPipeline(device, *pipeline, nullptr);
		*pipeline = VK_NULL_HANDLE;
	}
}

// ================================================================================================
// Scene passes: setup
// ================================================================================================

VkRenderPass Renderer::Impl::MakePass(VkFormat color, VkFormat depthFmt, VkImageLayout colorFinal, bool clearColor)
{
	std::vector<VkAttachmentDescription> attachments;
	VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference depthRef{ 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	if (color != VK_FORMAT_UNDEFINED)
	{
		VkAttachmentDescription a{};
		a.format = color;
		a.samples = VK_SAMPLE_COUNT_1_BIT;
		a.loadOp = clearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		a.finalLayout = colorFinal;
		colorRef.attachment = static_cast<uint32_t>(attachments.size());
		attachments.push_back(a);
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &colorRef;
	}
	if (depthFmt != VK_FORMAT_UNDEFINED)
	{
		// The shadow map is sampled afterwards; the view's depth is thrown away (tile memory only).
		const bool sampled = color == VK_FORMAT_UNDEFINED;
		VkAttachmentDescription a{};
		a.format = depthFmt;
		a.samples = VK_SAMPLE_COUNT_1_BIT;
		a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		a.storeOp = sampled ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
		a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		a.finalLayout = sampled ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depthRef.attachment = static_cast<uint32_t>(attachments.size());
		attachments.push_back(a);
		subpass.pDepthStencilAttachment = &depthRef;
	}
	// Earlier passes' writes are done before this one reads (fragment shaders) or overwrites them;
	// this one's writes are done before later passes sample them.
	VkSubpassDependency dependencies[2]{};
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
		| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
	dependencies[1].srcSubpass = 0;
	dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	VkRenderPassCreateInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	info.attachmentCount = static_cast<uint32_t>(attachments.size());
	info.pAttachments = attachments.data();
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	info.dependencyCount = 2;
	info.pDependencies = dependencies;
	VkRenderPass pass = VK_NULL_HANDLE;
	VK_TRY(vkCreateRenderPass(device, &info, nullptr, &pass));
	return pass;
}

VkPipeline Renderer::Impl::MakePipeline(const PipelineDesc& desc)
{
	const std::vector<uint32_t> vsWords = Words(desc.vs, desc.vsSize);
	VkShaderModule vs = Module(vsWords), ps = VK_NULL_HANDLE;
	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, desc.vsEntry, nullptr };
	uint32_t stageCount = 1;
	if (desc.ps)
	{
		const std::vector<uint32_t> psWords = Words(desc.ps, desc.psSize);
		ps = Module(psWords);
		stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps, desc.psEntry, nullptr };
		stageCount = 2;
	}

	VkVertexInputBindingDescription binding{ 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
	const VkVertexInputAttributeDescription attributes[4] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(Vertex, position)) },
		{ 1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(Vertex, normal)) },
		{ 2, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(Vertex, uv)) },
		{ 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, static_cast<uint32_t>(offsetof(Vertex, color)) },
	};
	VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	if (desc.vertices != PipelineDesc::Vertices::None)
	{
		vertexInput.vertexBindingDescriptionCount = 1;
		vertexInput.pVertexBindingDescriptions = &binding;
		vertexInput.vertexAttributeDescriptionCount = desc.vertices == PipelineDesc::Vertices::Mesh ? 4 : 1;
		vertexInput.pVertexAttributeDescriptions = attributes;
	}
	VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;   // game meshes' winding isn't guaranteed (as the D3D12 shadow pass says)
	raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
	raster.lineWidth = 1.0f;
	if (desc.depthBias)
	{
		raster.depthBiasEnable = VK_TRUE;
		raster.depthBiasConstantFactor = 1.0f;
		raster.depthBiasSlopeFactor = 2.5f;
		raster.depthBiasClamp = depthBiasClamp ? 0.01f : 0.0f;
	}
	VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo depthState{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	depthState.depthTestEnable = desc.depthTest;
	depthState.depthWriteEnable = desc.depthWrite;
	depthState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	VkPipelineColorBlendAttachmentState blendAttachment{};
	blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	if (desc.blend != PipelineDesc::Blend::None)
	{
		blendAttachment.blendEnable = VK_TRUE;
		blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		blendAttachment.dstColorBlendFactor = desc.blend == PipelineDesc::Blend::Additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
		blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
	}
	VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	blend.attachmentCount = desc.color ? 1 : 0;
	blend.pAttachments = &blendAttachment;
	const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dynamic.dynamicStateCount = 2;
	dynamic.pDynamicStates = dynamicStates;

	VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	info.stageCount = stageCount;
	info.pStages = stages;
	info.pVertexInputState = &vertexInput;
	info.pInputAssemblyState = &assembly;
	info.pViewportState = &viewport;
	info.pRasterizationState = &raster;
	info.pMultisampleState = &multisample;
	info.pDepthStencilState = &depthState;
	info.pColorBlendState = &blend;
	info.pDynamicState = &dynamic;
	info.layout = desc.layout;
	info.renderPass = desc.pass;
	VkPipeline pipeline = VK_NULL_HANDLE;
	VK_TRY(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline));
	vkDestroyShaderModule(device, vs, nullptr);
	if (ps) vkDestroyShaderModule(device, ps, nullptr);
	return pipeline;
}

VkDescriptorSet Renderer::Impl::MakeTextureSet(VkImageView view, VkSampler sampler)
{
	VkDescriptorSetAllocateInfo allocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	allocate.descriptorPool = uiDescriptorPool;
	allocate.descriptorSetCount = 1;
	allocate.pSetLayouts = &uiSetLayout;
	VkDescriptorSet set = VK_NULL_HANDLE;
	if (!VK_TRY(vkAllocateDescriptorSets(device, &allocate, &set))) return VK_NULL_HANDLE;
	SetTextureSet(set, view, sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	return set;
}

void Renderer::Impl::SetTextureSet(VkDescriptorSet set, VkImageView view, VkSampler sampler, VkImageLayout layout)
{
	VkDescriptorImageInfo image{ sampler, view, layout };
	VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	write.dstSet = set;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &image;
	vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

bool Renderer::Impl::EnsureBuffer(Buffer& buffer, VkDeviceSize bytes, VkBufferUsageFlags usage)
{
	if (buffer.size >= bytes && buffer.buffer) return true;
	if (buffer.buffer)
	{
		Buffer old = buffer;
		Defer([this, old]() mutable { DestroyBuffer(old); });
		buffer = {};
	}
	return CreateBuffer(std::max<VkDeviceSize>(bytes + bytes / 2, 4096), usage, true, buffer);
}

bool Renderer::Impl::CreateScene()
{
	// Samplers.
	VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.maxLod = VK_LOD_CLAMP_NONE;
	if (!VK_TRY(vkCreateSampler(device, &sampler, nullptr, &linearClamp))) return false;
	sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sampler.anisotropyEnable = anisotropy ? VK_TRUE : VK_FALSE;
	sampler.maxAnisotropy = anisotropy ? std::min(8.0f, properties.limits.maxSamplerAnisotropy) : 1.0f;
	if (!VK_TRY(vkCreateSampler(device, &sampler, nullptr, &materialSampler))) return false;
	VkSamplerCreateInfo shadow{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	shadow.magFilter = shadow.minFilter = VK_FILTER_LINEAR;
	shadow.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	shadow.addressModeU = shadow.addressModeV = shadow.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	shadow.compareEnable = VK_TRUE;
	shadow.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	if (!VK_TRY(vkCreateSampler(device, &shadow, nullptr, &shadowSampler))) return false;

	// One texture: ImGui's textures, materials' albedo and the full-screen passes' sources.
	VkDescriptorSetLayoutBinding textureBinding{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
	VkDescriptorSetLayoutCreateInfo textureLayout{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	textureLayout.bindingCount = 1;
	textureLayout.pBindings = &textureBinding;
	if (!VK_TRY(vkCreateDescriptorSetLayout(device, &textureLayout, nullptr, &uiSetLayout))) return false;
	VkDescriptorPoolSize textureSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024 };
	VkDescriptorPoolCreateInfo texturePool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	texturePool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	texturePool.maxSets = 1024;
	texturePool.poolSizeCount = 1;
	texturePool.pPoolSizes = &textureSize;
	if (!VK_TRY(vkCreateDescriptorPool(device, &texturePool, nullptr, &uiDescriptorPool))) return false;
	VkPushConstantRange uiPush{ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 4 };
	VkPipelineLayoutCreateInfo uiPipelineLayout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	uiPipelineLayout.setLayoutCount = 1;
	uiPipelineLayout.pSetLayouts = &uiSetLayout;
	uiPipelineLayout.pushConstantRangeCount = 1;
	uiPipelineLayout.pPushConstantRanges = &uiPush;
	if (!VK_TRY(vkCreatePipelineLayout(device, &uiPipelineLayout, nullptr, &uiLayout))) return false;

	// The frame set: constants, instances, particles, the shadow map.
	const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	const VkDescriptorSetLayoutBinding frameBindings[4] = {
		{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, nullptr },
		{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr },
		{ 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr },
		{ 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
	};
	VkDescriptorSetLayoutCreateInfo frameLayout{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	frameLayout.bindingCount = 4;
	frameLayout.pBindings = frameBindings;
	if (!VK_TRY(vkCreateDescriptorSetLayout(device, &frameLayout, nullptr, &frameSetLayout))) return false;
	const VkDescriptorPoolSize frameSizes[3] = {
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * kFramesInFlight },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight },
	};
	VkDescriptorPoolCreateInfo framePoolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	framePoolInfo.maxSets = kFramesInFlight;
	framePoolInfo.poolSizeCount = 3;
	framePoolInfo.pPoolSizes = frameSizes;
	if (!VK_TRY(vkCreateDescriptorPool(device, &framePoolInfo, nullptr, &framePool))) return false;

	const VkDescriptorSetLayout sceneSets[2] = { frameSetLayout, uiSetLayout };
	VkPushConstantRange scenePush{ both, 0, sizeof(DrawConstants) };
	VkPipelineLayoutCreateInfo sceneLayoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	sceneLayoutInfo.setLayoutCount = 2;
	sceneLayoutInfo.pSetLayouts = sceneSets;
	sceneLayoutInfo.pushConstantRangeCount = 1;
	sceneLayoutInfo.pPushConstantRanges = &scenePush;
	if (!VK_TRY(vkCreatePipelineLayout(device, &sceneLayoutInfo, nullptr, &sceneLayout))) return false;
	VkPushConstantRange postPush{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PostConstants) };
	VkPipelineLayoutCreateInfo postLayoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	postLayoutInfo.setLayoutCount = 1;
	postLayoutInfo.pSetLayouts = &uiSetLayout;
	postLayoutInfo.pushConstantRangeCount = 1;
	postLayoutInfo.pPushConstantRanges = &postPush;
	if (!VK_TRY(vkCreatePipelineLayout(device, &postLayoutInfo, nullptr, &postLayout))) return false;

	// Passes and pipelines.
	shadowPass = MakePass(VK_FORMAT_UNDEFINED, shadowFormat, VK_IMAGE_LAYOUT_UNDEFINED, false);
	scenePass = MakePass(kHdrFormat, depthFormat, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true);
	ldrPass = MakePass(kLdrFormat, VK_FORMAT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
	if (!shadowPass || !scenePass || !ldrPass) return false;

	PipelineDesc desc;
	desc.layout = sceneLayout;
	desc.vs = kSpirv_ShadowVS; desc.vsSize = sizeof(kSpirv_ShadowVS);
	desc.pass = shadowPass;
	desc.vertices = PipelineDesc::Vertices::Positions;
	desc.depthTest = desc.depthWrite = desc.depthBias = true;
	desc.color = false;
	shadowPipeline = MakePipeline(desc);

	desc = {};
	desc.layout = sceneLayout;
	desc.pass = scenePass;
	desc.vs = kSpirv_MeshVS; desc.vsSize = sizeof(kSpirv_MeshVS);
	desc.ps = kSpirv_MeshPS; desc.psSize = sizeof(kSpirv_MeshPS);
	desc.vertices = PipelineDesc::Vertices::Mesh;
	desc.depthTest = desc.depthWrite = true;
	meshPipeline = MakePipeline(desc);

	desc.vs = kSpirv_ParticlesVS; desc.vsSize = sizeof(kSpirv_ParticlesVS);
	desc.ps = kSpirv_ParticlesPS; desc.psSize = sizeof(kSpirv_ParticlesPS);
	desc.vertices = PipelineDesc::Vertices::None;
	desc.depthWrite = false;
	desc.blend = PipelineDesc::Blend::Alpha;
	particleBlendPipeline = MakePipeline(desc);
	desc.blend = PipelineDesc::Blend::Additive;
	particleAddPipeline = MakePipeline(desc);

	desc = {};
	desc.layout = postLayout;
	desc.pass = ldrPass;
	desc.vs = kSpirv_PostVS; desc.vsSize = sizeof(kSpirv_PostVS);
	desc.ps = kSpirv_TonemapPS; desc.psSize = sizeof(kSpirv_TonemapPS); desc.psEntry = "TonemapPS";
	tonemapPipeline = MakePipeline(desc);
	if (!shadowPipeline || !meshPipeline || !particleBlendPipeline || !particleAddPipeline || !tonemapPipeline) return false;

	// The shadow map.
	if (!CreateImage(kShadowMapSize, kShadowMapSize, shadowFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		VK_IMAGE_ASPECT_DEPTH_BIT, shadowMap)) return false;
	VkFramebufferCreateInfo fb{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
	fb.renderPass = shadowPass;
	fb.attachmentCount = 1;
	fb.pAttachments = &shadowMap.view;
	fb.width = fb.height = kShadowMapSize;
	fb.layers = 1;
	if (!VK_TRY(vkCreateFramebuffer(device, &fb, nullptr, &shadowFramebuffer))) return false;
	// Never drawn yet: make it readable (all lit) until the first shadow pass.
	Submit([&](VkCommandBuffer cmd) {
		Barrier(cmd, shadowMap.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkClearDepthStencilValue one{ 1.0f, 0 };
		VkImageSubresourceRange range{ VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
		vkCmdClearDepthStencilImage(cmd, shadowMap.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &range);
		Barrier(cmd, shadowMap.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	});

	// Per frame: constants, instance and particle buffers, and their set.
	for (FrameData& frame : frames)
	{
		if (!CreateBuffer(sizeof(FrameConstants), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, frame.constants)) return false;
		if (!EnsureBuffer(frame.instances, 1024 * sizeof(InstanceGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return false;
		if (!EnsureBuffer(frame.particles, 256 * sizeof(ParticleGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return false;
		VkDescriptorSetAllocateInfo allocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		allocate.descriptorPool = framePool;
		allocate.descriptorSetCount = 1;
		allocate.pSetLayouts = &frameSetLayout;
		if (!VK_TRY(vkAllocateDescriptorSets(device, &allocate, &frame.set))) return false;
	}
	return true;
}

void Renderer::Impl::DestroyTargets()
{
	if (sceneFramebuffer) vkDestroyFramebuffer(device, sceneFramebuffer, nullptr);
	if (ldrFramebuffer) vkDestroyFramebuffer(device, ldrFramebuffer, nullptr);
	sceneFramebuffer = ldrFramebuffer = VK_NULL_HANDLE;
	DestroyImage(hdr);
	DestroyImage(depth);
	DestroyImage(ldr);
}

bool Renderer::Impl::EnsureTargets(uint32_t w, uint32_t h)
{
	if (hdr.image && hdr.width == w && hdr.height == h) return true;
	vkDeviceWaitIdle(device);
	DestroyTargets();
	if (!CreateImage(w, h, kHdrFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, hdr)) return false;
	if (!CreateImage(w, h, depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, depth)) return false;
	if (!CreateImage(w, h, kLdrFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, ldr)) return false;
	const VkImageView sceneViews[2] = { hdr.view, depth.view };
	VkFramebufferCreateInfo fb{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
	fb.renderPass = scenePass;
	fb.attachmentCount = 2;
	fb.pAttachments = sceneViews;
	fb.width = w;
	fb.height = h;
	fb.layers = 1;
	if (!VK_TRY(vkCreateFramebuffer(device, &fb, nullptr, &sceneFramebuffer))) return false;
	fb.renderPass = ldrPass;
	fb.attachmentCount = 1;
	fb.pAttachments = &ldr.view;
	if (!VK_TRY(vkCreateFramebuffer(device, &fb, nullptr, &ldrFramebuffer))) return false;
	if (!hdrSet) hdrSet = MakeTextureSet(hdr.view, linearClamp);
	else SetTextureSet(hdrSet, hdr.view, linearClamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	if (!ldrSet) ldrSet = MakeTextureSet(ldr.view, linearClamp);
	else SetTextureSet(ldrSet, ldr.view, linearClamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	CHERRY_LOG("Vulkan view targets %ux%u", w, h);
	return true;
}

void Renderer::Impl::DestroyScene()
{
	DestroyTargets();
	if (shadowFramebuffer) vkDestroyFramebuffer(device, shadowFramebuffer, nullptr);
	DestroyImage(shadowMap);
	for (VkPipeline pipeline : { shadowPipeline, meshPipeline, particleBlendPipeline, particleAddPipeline, tonemapPipeline })
		if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
	for (VkRenderPass pass : { shadowPass, scenePass, ldrPass })
		if (pass) vkDestroyRenderPass(device, pass, nullptr);
	if (sceneLayout) vkDestroyPipelineLayout(device, sceneLayout, nullptr);
	if (postLayout) vkDestroyPipelineLayout(device, postLayout, nullptr);
	if (framePool) vkDestroyDescriptorPool(device, framePool, nullptr);
	if (frameSetLayout) vkDestroyDescriptorSetLayout(device, frameSetLayout, nullptr);
	for (VkSampler sampler : { materialSampler, shadowSampler })
		if (sampler) vkDestroySampler(device, sampler, nullptr);
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
	// Every mesh is stored in the full vertex format here (MeshFormat::Compact only saves memory).
	(void)format;
	Impl& r = *mImpl;
	const VkDeviceSize vertexBytes = data.vertices.size() * sizeof(Vertex), indexBytes = data.indices.size() * sizeof(uint32_t);
	mesh.bytes = static_cast<size_t>(vertexBytes + indexBytes);
	mesh.indexCount = static_cast<uint32_t>(data.indices.size());
	if (r.device && vertexBytes && indexBytes
		&& r.CreateBuffer(vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, mesh.vertices)
		&& r.CreateBuffer(indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, mesh.indices))
	{
		Buffer staging;
		if (r.CreateBuffer(vertexBytes + indexBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, staging))
		{
			std::memcpy(staging.mapped, data.vertices.data(), vertexBytes);
			std::memcpy(static_cast<uint8_t*>(staging.mapped) + vertexBytes, data.indices.data(), indexBytes);
			r.Submit([&](VkCommandBuffer cmd) {
				const VkBufferCopy v{ 0, 0, vertexBytes }, i{ vertexBytes, 0, indexBytes };
				vkCmdCopyBuffer(cmd, staging.buffer, mesh.vertices.buffer, 1, &v);
				vkCmdCopyBuffer(cmd, staging.buffer, mesh.indices.buffer, 1, &i);
			});
			r.DestroyBuffer(staging);
		}
	}
	return r.meshes.Add(std::move(mesh));
}
size_t Renderer::GetMeshBytes(MeshHandle mesh) const { const GpuMesh* m = mImpl->meshes.Get(mesh); return m ? m->bytes : 0; }
void Renderer::DestroyMesh(MeshHandle mesh)
{
	Impl& r = *mImpl;
	if (GpuMesh* m = r.meshes.Get(mesh))
	{
		Buffer vertices = m->vertices, indices = m->indices;
		r.Defer([&r, vertices, indices]() mutable { r.DestroyBuffer(vertices); r.DestroyBuffer(indices); });
	}
	r.meshes.Remove(mesh);
}
const BoundingBox* Renderer::GetMeshBounds(MeshHandle mesh) const { const GpuMesh* m = mImpl->meshes.Get(mesh); return m ? &m->bounds : nullptr; }

TextureHandle Renderer::LoadTexture(const std::wstring& path, bool srgb)
{
	// .dds files need the D3D12 loader; on this renderer they show as white.
	if (!mImpl->warnedDds) CHERRY_WARN("Vulkan renderer: .dds textures aren't supported yet (%s)", std::filesystem::path(path).string().c_str());
	mImpl->warnedDds = true;
	const uint8_t white[4] = { 255, 255, 255, 255 };
	return CreateTexture(1, 1, white, srgb);
}

TextureHandle Renderer::CreateTexture(uint32_t width, uint32_t height, const uint8_t* rgba, bool srgb)
{
	Impl& r = *mImpl;
	GpuTexture texture;
	if (width == 0 || height == 0 || !rgba || !r.device) return r.textures.Add(std::move(texture));

	// The mip chain, built on the CPU (box filter; in linear light for sRGB colour).
	std::vector<std::vector<uint8_t>> levels{ std::vector<uint8_t>(rgba, rgba + static_cast<size_t>(width) * height * 4) };
	std::vector<std::pair<uint32_t, uint32_t>> sizes{ { width, height } };
	auto toLinear = [srgb](uint8_t v) { const float c = v / 255.0f; return srgb ? (c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f)) : c; };
	auto toByte = [srgb](float c) {
		c = std::clamp(c, 0.0f, 1.0f);
		if (srgb) c = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
		return static_cast<uint8_t>(std::lround(c * 255.0f));
	};
	while (sizes.back().first > 1 || sizes.back().second > 1)
	{
		const auto [pw, ph] = sizes.back();
		const uint32_t w = std::max(pw / 2, 1u), h = std::max(ph / 2, 1u);
		const std::vector<uint8_t>& src = levels.back();
		std::vector<uint8_t> dst(static_cast<size_t>(w) * h * 4);
		for (uint32_t y = 0; y < h; ++y)
			for (uint32_t x = 0; x < w; ++x)
				for (int c = 0; c < 4; ++c)
				{
					float sum = 0.0f;
					for (uint32_t dy = 0; dy < 2; ++dy)
						for (uint32_t dx = 0; dx < 2; ++dx)
						{
							const uint32_t sx = std::min(x * 2 + dx, pw - 1), sy = std::min(y * 2 + dy, ph - 1);
							const uint8_t v = src[(static_cast<size_t>(sy) * pw + sx) * 4 + c];
							sum += c == 3 ? v / 255.0f : toLinear(v);
						}
					dst[(static_cast<size_t>(y) * w + x) * 4 + c] = c == 3 ? static_cast<uint8_t>(std::lround(sum / 4.0f * 255.0f)) : toByte(sum / 4.0f);
				}
		levels.push_back(std::move(dst));
		sizes.push_back({ w, h });
	}

	const uint32_t mips = static_cast<uint32_t>(levels.size());
	const VkFormat format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
	if (r.CreateImage(width, height, format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, texture.image, mips))
	{
		VkDeviceSize total = 0;
		for (const auto& level : levels) total += level.size();
		Buffer staging;
		if (r.CreateBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, staging))
		{
			std::vector<VkBufferImageCopy> copies;
			VkDeviceSize offset = 0;
			for (uint32_t m = 0; m < mips; ++m)
			{
				std::memcpy(static_cast<uint8_t*>(staging.mapped) + offset, levels[m].data(), levels[m].size());
				VkBufferImageCopy copy{};
				copy.bufferOffset = offset;
				copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1 };
				copy.imageExtent = { sizes[m].first, sizes[m].second, 1 };
				copies.push_back(copy);
				offset += levels[m].size();
			}
			r.Submit([&](VkCommandBuffer cmd) {
				VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
				barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.image = texture.image.image;
				barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1 };
				barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
				vkCmdCopyBufferToImage(cmd, staging.buffer, texture.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(copies.size()), copies.data());
				barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
				vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
			});
			r.DestroyBuffer(staging);
		}
		texture.set = r.MakeTextureSet(texture.image.view, r.materialSampler);
	}
	return r.textures.Add(std::move(texture));
}

void Renderer::DestroyTexture(TextureHandle texture)
{
	Impl& r = *mImpl;
	if (GpuTexture* t = r.textures.Get(texture))
	{
		Image image = t->image;
		VkDescriptorSet set = t->set;
		r.Defer([&r, image, set]() mutable {
			if (set) vkFreeDescriptorSets(r.device, r.uiDescriptorPool, 1, &set);
			r.DestroyImage(image);
		});
	}
	r.textures.Remove(texture);
}

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
	if (!r.CreateScene())
	{
		error = "setting up the Vulkan passes failed";
		return false;
	}
	const uint8_t white[4] = { 255, 255, 255, 255 };
	r.whiteTexture = CreateTexture(1, 1, white, false);

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
		r.DestroyBuffer(frame.constants);
		r.DestroyBuffer(frame.instances);
		r.DestroyBuffer(frame.particles);
		if (frame.acquired) vkDestroySemaphore(r.device, frame.acquired, nullptr);
		if (frame.fence) vkDestroyFence(r.device, frame.fence, nullptr);
		if (frame.pool) vkDestroyCommandPool(r.device, frame.pool, nullptr);
		frame = {};
	}
	// Whatever the game didn't destroy.
	for (uint32_t slot = 0; slot < r.meshes.SlotCount(); ++slot)
		if (GpuMesh* m = r.meshes.AtSlot(slot)) { r.DestroyBuffer(m->vertices); r.DestroyBuffer(m->indices); }
	for (uint32_t slot = 0; slot < r.textures.SlotCount(); ++slot)
		if (GpuTexture* t = r.textures.AtSlot(slot)) r.DestroyImage(t->image);
	r.meshes.Clear();
	r.textures.Clear();
	r.DestroyScene();
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

// ================================================================================================
// Scene: preparation (CPU) and recording
// ================================================================================================

GpuTexture* Renderer::Impl::TextureOf(MaterialHandle material)
{
	const MaterialDesc* desc = materials.Get(material);
	GpuTexture* texture = desc ? textures.Get(desc->albedoTexture) : nullptr;
	if (!texture || !texture->set) texture = textures.Get(whiteTexture);
	return texture;
}

// The sun's shadow map: fitted to ShadowSettings' world box when it has one (as the D3D12
// renderer does), else to the view's first `farDistance` metres.
void Renderer::Impl::SetupShadows(const RenderView& view, FrameConstants& constants)
{
	const ShadowSettings& shadows = settings.shadows;
	const XMFLOAT3& own = shadows.direction;
	const bool ownDirection = own.x != 0.0f || own.y != 0.0f || own.z != 0.0f;
	const XMVECTOR sun = XMVector3Normalize(XMLoadFloat3(ownDirection ? &own : &settings.sunDirection));
	const XMVECTOR up = std::fabs(XMVectorGetY(sun)) > 0.99f ? XMVectorSet(0, 0, 1, 0) : XMVectorSet(0, 1, 0, 0);
	const XMMATRIX lightView = XMMatrixLookToLH(XMVectorZero(), sun, up);
	const XMMATRIX toUv = XMMatrixScaling(0.5f, -0.5f, 1.0f) * XMMatrixTranslation(0.5f, 0.5f, 0.0f);

	XMFLOAT3 corners[8];
	const bool fitted = shadows.boundsExtents.x > 0.0f || shadows.boundsExtents.y > 0.0f || shadows.boundsExtents.z > 0.0f;
	float reach = 1.0f;
	if (fitted)
		BoundingBox(shadows.boundsCenter, shadows.boundsExtents).GetCorners(corners);
	else
	{
		// The view's frustum out to farDistance.
		const XMMATRIX viewProj = XMLoadFloat4x4(&view.view) * XMLoadFloat4x4(&view.proj);
		const XMMATRIX inverse = XMMatrixInverse(nullptr, viewProj);
		const float farZ = std::min(view.farZ, std::max(shadows.farDistance, view.nearZ + 1.0f));
		const XMVECTOR farClip = XMVector3TransformCoord(XMVectorSet(0, 0, farZ, 1), XMLoadFloat4x4(&view.proj));
		const float farDepth = std::clamp(XMVectorGetZ(farClip), 0.0f, 1.0f);
		int i = 0;
		for (float z : { 0.0f, farDepth })
			for (float y : { -1.0f, 1.0f })
				for (float x : { -1.0f, 1.0f })
					XMStoreFloat3(&corners[i++], XMVector3TransformCoord(XMVectorSet(x, y, z, 1), inverse));
		reach = 400.0f;   // towards the sun, for casters outside the view
	}
	XMVECTOR lo = XMVectorReplicate(FLT_MAX), hi = XMVectorReplicate(-FLT_MAX);
	for (const XMFLOAT3& corner : corners)
	{
		const XMVECTOR lp = XMVector3TransformCoord(XMLoadFloat3(&corner), lightView);
		lo = XMVectorMin(lo, lp);
		hi = XMVectorMax(hi, lp);
	}
	XMFLOAT3 l, h;
	XMStoreFloat3(&l, lo);
	XMStoreFloat3(&h, hi);
	const float radius = std::max(0.5f * std::max(h.x - l.x, h.y - l.y), 0.01f);
	const float texel = 2.0f * radius / kShadowMapSize;
	const float cx = std::floor(0.5f * (l.x + h.x) / texel) * texel, cy = std::floor(0.5f * (l.y + h.y) / texel) * texel;
	const XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(cx - radius, cx + radius, cy - radius, cy + radius, l.z - reach, h.z + 1.0f);
	XMStoreFloat4x4(&constants.shadowViewProj, lightView * lightProj);
	XMStoreFloat4x4(&constants.shadowUv, lightView * lightProj * toUv);
	constants.sunStrength.w = texel;
	constants.sunDirection.w = shadows.normalBias;
}

void Renderer::RenderScene(Scene& scene, const RenderView& view)
{
	Impl& r = *mImpl;
	r.stats = {};
	r.sceneReady = false;
	r.runs.clear();
	r.shadowRuns.clear();
	r.instanceData.clear();
	r.particleData.clear();

	// Everything to draw: the scene's mesh entities and the game's entity-free instances.
	struct Item { MeshHandle mesh; MaterialHandle material; XMFLOAT4X4 world; XMFLOAT4 tint; bool shadows; };
	std::vector<Item> items;
	items.reserve(r.meshInstances.size() + scene.MeshEntities().size());
	for (Entity entity : scene.MeshEntities())
	{
		const MeshRenderer& mesh = scene.Get<MeshRenderer>(entity);
		if (!mesh.visible || mesh.layer != RenderLayer::World || !r.meshes.IsValid(mesh.mesh) || mesh.dither <= 0.0f) continue;
		Item item{ mesh.mesh, mesh.material, {}, { 1, 1, 1, 1 }, mesh.castShadows };
		XMStoreFloat4x4(&item.world, scene.WorldMatrix(entity));
		items.push_back(item);
	}
	for (const MeshInstance& instance : r.meshInstances)
	{
		if (!r.meshes.IsValid(instance.mesh)) continue;
		items.push_back({ instance.mesh, instance.material, instance.world, instance.tint, instance.castShadows });
	}
	r.meshInstances.clear();

	// Runs of the same mesh and material, one instanced draw each; shadow casters in a second list.
	std::vector<uint32_t> order(items.size());
	for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
	std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
		const Item& x = items[a];
		const Item& y = items[b];
		if (x.material.index != y.material.index) return x.material.index < y.material.index;
		return x.mesh.index < y.mesh.index;
	});
	r.instanceData.reserve(items.size() * 2);
	auto addInstance = [&](const Item& item) {
		InstanceGpu gpu;
		gpu.world = item.world;
		const XMMATRIX world = XMLoadFloat4x4(&item.world);
		XMStoreFloat4x4(&gpu.normalMatrix, XMMatrixTranspose(XMMatrixInverse(nullptr, world)));
		gpu.tint = item.tint;
		r.instanceData.push_back(gpu);
	};
	for (uint32_t index : order)
	{
		const Item& item = items[index];
		if (r.runs.empty() || r.runs.back().mesh != item.mesh || r.runs.back().material != item.material)
			r.runs.push_back({ item.mesh, item.material, static_cast<uint32_t>(r.instanceData.size()), 0 });
		addInstance(item);
		++r.runs.back().count;
	}
	if (r.settings.shadows.enabled)
	{
		for (uint32_t index : order)
		{
			const Item& item = items[index];
			if (!item.shadows) continue;
			if (r.shadowRuns.empty() || r.shadowRuns.back().mesh != item.mesh)
				r.shadowRuns.push_back({ item.mesh, {}, static_cast<uint32_t>(r.instanceData.size()), 0 });
			addInstance(item);
			++r.shadowRuns.back().count;
		}
	}

	// Particles: covering ones back to front, then the additive ones.
	const XMMATRIX viewMatrix = XMLoadFloat4x4(&view.view);
	const XMMATRIX invView = XMMatrixInverse(nullptr, viewMatrix);
	std::vector<std::pair<float, const ParticleInstance*>> covering;
	std::vector<const ParticleInstance*> additive;
	for (const ParticleInstance& p : r.particles)
	{
		if (p.additive) { additive.push_back(&p); continue; }
		const float depth = XMVectorGetZ(XMVector3TransformCoord(XMLoadFloat3(&p.position), viewMatrix));
		covering.push_back({ depth, &p });
	}
	std::sort(covering.begin(), covering.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
	auto addParticle = [&](const ParticleInstance& p) {
		r.particleData.push_back({ XMFLOAT4(p.position.x, p.position.y, p.position.z, p.size), p.color,
			XMFLOAT4(p.emissive, p.rotation, p.square ? 1.0f : 0.0f, 0.0f) });
	};
	for (const auto& entry : covering) addParticle(*entry.second);
	for (const ParticleInstance* p : additive) addParticle(*p);
	r.coveringParticles = static_cast<uint32_t>(covering.size());
	r.particles.clear();

	if (!r.frameAcquired) return;

	// The frame's constants and buffers (this frame's previous use is done: BeginFrame waited).
	FrameData& frame = r.frames[r.frameIndex];
	FrameConstants constants{};
	XMStoreFloat4x4(&constants.viewProj, viewMatrix * XMLoadFloat4x4(&view.proj));
	constants.eye = { view.position.x, view.position.y, view.position.z, r.settings.shadows.enabled ? 1.0f : 0.0f };
	XMFLOAT3 sun;
	XMStoreFloat3(&sun, XMVector3Normalize(XMLoadFloat3(&r.settings.sunDirection)));
	constants.sunDirection = { sun.x, sun.y, sun.z, 0.0f };
	constants.sunStrength = { r.settings.sunColor.x, r.settings.sunColor.y, r.settings.sunColor.z, 0.0f };
	const float contrast = std::clamp(r.settings.ambientContrast, 0.0f, 1.0f);
	const XMFLOAT3& ambient = r.settings.ambient;
	constants.ambientSky = { ambient.x * (1 + contrast), ambient.y * (1 + contrast), ambient.z * (1 + contrast), 0.0f };
	constants.ambientGround = { ambient.x * (1 - contrast), ambient.y * (1 - contrast), ambient.z * (1 - contrast), 0.0f };
	XMStoreFloat4(&constants.cameraRight, XMVector3Normalize(invView.r[0]));
	XMStoreFloat4(&constants.cameraUp, XMVector3Normalize(invView.r[1]));
	constants.cameraUp.w = 1.0f / kShadowMapSize;
	if (r.settings.shadows.enabled) r.SetupShadows(view, constants);
	std::memcpy(frame.constants.mapped, &constants, sizeof(constants));

	const VkDeviceSize instanceBytes = std::max<size_t>(r.instanceData.size(), 1) * sizeof(InstanceGpu);
	const VkDeviceSize particleBytes = std::max<size_t>(r.particleData.size(), 1) * sizeof(ParticleGpu);
	if (!r.EnsureBuffer(frame.instances, instanceBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
		|| !r.EnsureBuffer(frame.particles, particleBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return;
	if (!r.instanceData.empty()) std::memcpy(frame.instances.mapped, r.instanceData.data(), r.instanceData.size() * sizeof(InstanceGpu));
	if (!r.particleData.empty()) std::memcpy(frame.particles.mapped, r.particleData.data(), r.particleData.size() * sizeof(ParticleGpu));

	const VkDescriptorBufferInfo constantsInfo{ frame.constants.buffer, 0, sizeof(FrameConstants) };
	const VkDescriptorBufferInfo instancesInfo{ frame.instances.buffer, 0, VK_WHOLE_SIZE };
	const VkDescriptorBufferInfo particlesInfo{ frame.particles.buffer, 0, VK_WHOLE_SIZE };
	const VkDescriptorImageInfo shadowInfo{ r.shadowSampler, r.shadowMap.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkWriteDescriptorSet writes[4]{};
	for (uint32_t i = 0; i < 4; ++i)
	{
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = frame.set;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
	}
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	writes[0].pBufferInfo = &constantsInfo;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[1].pBufferInfo = &instancesInfo;
	writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[2].pBufferInfo = &particlesInfo;
	writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[3].pImageInfo = &shadowInfo;
	vkUpdateDescriptorSets(r.device, 4, writes, 0, nullptr);

	r.sceneReady = r.EnsureTargets(r.viewWidth, r.viewHeight);
	r.stats.instancesSubmitted = static_cast<uint32_t>(items.size());
}

void Renderer::Impl::RecordScene(VkCommandBuffer cmd)
{
	FrameData& frame = frames[frameIndex];
	auto viewportTo = [&](uint32_t w, uint32_t h) {
		const VkViewport vp{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
		const VkRect2D scissor{ { 0, 0 }, { w, h } };
		vkCmdSetViewport(cmd, 0, 1, &vp);
		vkCmdSetScissor(cmd, 0, 1, &scissor);
	};
	auto drawRuns = [&](const std::vector<DrawRun>& list, bool materialsToo) {
		for (const DrawRun& run : list)
		{
			const GpuMesh* mesh = meshes.Get(run.mesh);
			if (!mesh || !mesh->vertices.buffer || mesh->indexCount == 0) continue;
			DrawConstants draw{ { 1, 1, 1, 1 }, 0.5f, 0.0f, 0.04f, run.first };
			if (materialsToo)
			{
				if (const MaterialDesc* material = materials.Get(run.material))
				{
					draw.baseColor = material->baseColor;
					draw.roughness = material->roughness;
					draw.metallic = material->metallic;
					draw.f0 = material->fresnelR0.x;
				}
				const VkDescriptorSet set = TextureOf(run.material)->set;
				vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sceneLayout, 1, 1, &set, 0, nullptr);
			}
			vkCmdPushConstants(cmd, sceneLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(draw), &draw);
			const VkDeviceSize offset = 0;
			vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vertices.buffer, &offset);
			vkCmdBindIndexBuffer(cmd, mesh->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
			vkCmdDrawIndexed(cmd, mesh->indexCount, run.count, 0, 0, 0);
			++stats.drawCalls;
		}
	};

	// 1. Shadow map.
	if (settings.shadows.enabled)
	{
		VkClearValue clear{};
		clear.depthStencil = { 1.0f, 0 };
		VkRenderPassBeginInfo pass{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
		pass.renderPass = shadowPass;
		pass.framebuffer = shadowFramebuffer;
		pass.renderArea = { { 0, 0 }, { kShadowMapSize, kShadowMapSize } };
		pass.clearValueCount = 1;
		pass.pClearValues = &clear;
		vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
		viewportTo(kShadowMapSize, kShadowMapSize);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sceneLayout, 0, 1, &frame.set, 0, nullptr);
		drawRuns(shadowRuns, false);
		vkCmdEndRenderPass(cmd);
	}

	// 2. The scene into HDR: meshes, then particles.
	VkClearValue clears[2]{};
	clears[0].color = { { settings.clearColor.x, settings.clearColor.y, settings.clearColor.z, 1.0f } };
	clears[1].depthStencil = { 1.0f, 0 };
	VkRenderPassBeginInfo scene{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	scene.renderPass = scenePass;
	scene.framebuffer = sceneFramebuffer;
	scene.renderArea = { { 0, 0 }, { hdr.width, hdr.height } };
	scene.clearValueCount = 2;
	scene.pClearValues = clears;
	vkCmdBeginRenderPass(cmd, &scene, VK_SUBPASS_CONTENTS_INLINE);
	viewportTo(hdr.width, hdr.height);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sceneLayout, 0, 1, &frame.set, 0, nullptr);
	drawRuns(runs, true);
	const uint32_t particleCount = static_cast<uint32_t>(particleData.size());
	if (particleCount > 0)
	{
		const VkDescriptorSet white = textures.Get(whiteTexture)->set;
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sceneLayout, 1, 1, &white, 0, nullptr);
		auto drawParticles = [&](VkPipeline pipeline, uint32_t first, uint32_t count) {
			if (count == 0) return;
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
			const DrawConstants draw{ { 1, 1, 1, 1 }, 0.0f, 0.0f, 0.0f, first };
			vkCmdPushConstants(cmd, sceneLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(draw), &draw);
			vkCmdDraw(cmd, 6, count, 0, 0);
			++stats.drawCalls;
		};
		drawParticles(particleBlendPipeline, 0, coveringParticles);
		drawParticles(particleAddPipeline, coveringParticles, particleCount - coveringParticles);
	}
	vkCmdEndRenderPass(cmd);

	// 3. Tonemap into LDR.
	VkRenderPassBeginInfo tonemap{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	tonemap.renderPass = ldrPass;
	tonemap.framebuffer = ldrFramebuffer;
	tonemap.renderArea = { { 0, 0 }, { ldr.width, ldr.height } };
	vkCmdBeginRenderPass(cmd, &tonemap, VK_SUBPASS_CONTENTS_INLINE);
	viewportTo(ldr.width, ldr.height);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemapPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postLayout, 0, 1, &hdrSet, 0, nullptr);
	const PostConstants post{ 1.0f / hdr.width, 1.0f / hdr.height, settings.exposure, settings.toneCurve ? 1u : 0u };
	vkCmdPushConstants(cmd, postLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(post), &post);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRenderPass(cmd);
}

void Renderer::EndFrame(bool viewFullscreen)
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

	const bool drawView = r.sceneReady && viewFullscreen;
	if (drawView) r.RecordScene(cmd);
	r.sceneReady = false;

	// The swapchain: the view (FXAA or a copy) over the clear colour, then ImGui.
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
	if (drawView)
	{
		const VkViewport vp{ 0.0f, 0.0f, static_cast<float>(r.swapExtent.width), static_cast<float>(r.swapExtent.height), 0.0f, 1.0f };
		const VkRect2D scissor{ { 0, 0 }, r.swapExtent };
		vkCmdSetViewport(cmd, 0, 1, &vp);
		vkCmdSetScissor(cmd, 0, 1, &scissor);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.settings.antiAliasing ? r.fxaaPipeline : r.copyPipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.postLayout, 0, 1, &r.ldrSet, 0, nullptr);
		const PostConstants post{ 1.0f / r.ldr.width, 1.0f / r.ldr.height, 1.0f, 0u };
		vkCmdPushConstants(cmd, r.postLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(post), &post);
		vkCmdDraw(cmd, 3, 1, 0, 0);
	}
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
