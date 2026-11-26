// The Vulkan backend of the RHI (Windows; Vulkan 1.3).
//
// Binding model (matches the SPIR-V ShaderCompiler emits, see RHI/RHI.h):
//   set 0: binding 1, 2  view and frame constants (dynamic uniform buffers in the constant ring)
//          bindings 16..22 the fixed samplers (immutable)
//   set 1: binding 0     the bindless heap, an array of VK_EXT_mutable_descriptor_type
//                        descriptors (sampled images, storage images, storage buffers) indexed
//                        like D3D12's ResourceDescriptorHeap
//   push constants       b0, 64 bytes
// Clip space matches D3D through a negative viewport height. Rendering is dynamic rendering;
// barriers are synchronization2. Frames and uploads each have a timeline semaphore.
#include "RHI/RHI.h"
#include "Core/Log.h"

#if defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#else
#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/native_window.h>
#endif
#include <volk.h>
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace rhi
{
	namespace
	{
		constexpr uint64_t kConstantBytesPerFrame = 4 * 1024 * 1024;
#if defined(_WIN32)
		constexpr const char* kSurfaceExtension = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#else
		constexpr const char* kSurfaceExtension = VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
#endif

		bool EnvFlag(const char* name)
		{
#if defined(_MSC_VER)
			char* value = nullptr;
			size_t length = 0;
			const bool set = _dupenv_s(&value, &length, name) == 0 && value && value[0] == '1';
			free(value);
			return set;
#else
			const char* value = std::getenv(name);
			return value && value[0] == '1';
#endif
		}

		// Without mutable descriptors the heap is three typed arrays (bindings 0, 1, 2 of set 1).
		// DXC puts every ResourceDescriptorHeap variable at set 1 binding 0; this moves each to the
		// binding of its type: sampled images 0, storage images 1, storage buffers 2.
		std::vector<uint32_t> SplitHeapBindings(const std::vector<uint32_t>& code)
		{
			std::vector<uint32_t> words = code;
			if (words.size() < 5) return words;
			constexpr uint32_t OpTypeImage = 25, OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32, OpVariable = 59, OpDecorate = 71;
			constexpr uint32_t DecorationBinding = 33, DecorationDescriptorSet = 34;
			std::unordered_map<uint32_t, uint32_t> imageSampled, arrayElement, pointee, variableType;
			std::unordered_map<uint32_t, bool> isStruct, inSet1;
			std::vector<size_t> bindingWords;   // the operand of each Binding decoration
			std::vector<uint32_t> bindingTargets;
			for (size_t i = 5; i < words.size(); )
			{
				const uint32_t op = words[i] & 0xffff, count = words[i] >> 16;
				if (count == 0 || i + count > words.size()) break;
				if (op == OpTypeImage && count >= 9) imageSampled[words[i + 1]] = words[i + 7];
				else if (op == OpTypeRuntimeArray && count >= 3) arrayElement[words[i + 1]] = words[i + 2];
				else if (op == OpTypeStruct) isStruct[words[i + 1]] = true;
				else if (op == OpTypePointer && count >= 4) pointee[words[i + 1]] = words[i + 3];
				else if (op == OpVariable && count >= 4) variableType[words[i + 2]] = words[i + 1];
				else if (op == OpDecorate && count >= 4)
				{
					if (words[i + 2] == DecorationDescriptorSet && words[i + 3] == 1) inSet1[words[i + 1]] = true;
					if (words[i + 2] == DecorationBinding) { bindingWords.push_back(i + 3); bindingTargets.push_back(words[i + 1]); }
				}
				i += count;
			}
			for (size_t b = 0; b < bindingWords.size(); ++b)
			{
				const uint32_t variable = bindingTargets[b];
				if (!inSet1[variable]) continue;
				const uint32_t array = pointee[variableType[variable]];
				const auto element = arrayElement.find(array);
				const uint32_t type = element != arrayElement.end() ? element->second : array;
				uint32_t binding = 2;   // storage buffers (structs)
				if (const auto image = imageSampled.find(type); image != imageSampled.end()) binding = image->second == 2 ? 1 : 0;
				else if (!isStruct[type]) continue;
				words[bindingWords[b]] = binding;
			}
			return words;
		}
		constexpr uint32_t kTimestampCount = kFramesInFlight * kTimestampsPerFrame;
		constexpr uint32_t kSamplerBinding = 16;

		std::string VkResultName(VkResult result)
		{
			switch (result)
			{
			case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
			case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
			case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
			case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
			case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
			case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
			case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
			case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
			case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
			case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
			default: return "VkResult " + std::to_string(static_cast<int>(result));
			}
		}

		void Check(VkResult result, const char* what, const char* file, int line)
		{
			if (result >= 0) return;
			throw std::runtime_error(std::string(what) + " failed: " + VkResultName(result) + " (" + file + ":" + std::to_string(line) + ")");
		}
#define VK_CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)

		VkFormat ToVk(Format format)
		{
			switch (format)
			{
			case Format::RGBA8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
			case Format::RGBA8UnormSrgb: return VK_FORMAT_R8G8B8A8_SRGB;
			case Format::BGRA8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
			case Format::BGRA8UnormSrgb: return VK_FORMAT_B8G8R8A8_SRGB;
			case Format::BGRX8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
			case Format::BGRX8UnormSrgb: return VK_FORMAT_B8G8R8A8_SRGB;
			case Format::RGBA16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
			case Format::RGBA32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
			case Format::RG32Float: return VK_FORMAT_R32G32_SFLOAT;
			case Format::RG16Float: return VK_FORMAT_R16G16_SFLOAT;
			case Format::R32Float: return VK_FORMAT_R32_SFLOAT;
			case Format::R16Float: return VK_FORMAT_R16_SFLOAT;
			case Format::R8Unorm: return VK_FORMAT_R8_UNORM;
			case Format::R8Uint: return VK_FORMAT_R8_UINT;
			case Format::RG8Unorm: return VK_FORMAT_R8G8_UNORM;
			case Format::R16Unorm: return VK_FORMAT_R16_UNORM;
			case Format::RGB10A2Unorm: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
			case Format::RG11B10Float: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
			case Format::D32Float: return VK_FORMAT_D32_SFLOAT;
			case Format::BC1Unorm: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
			case Format::BC1UnormSrgb: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
			case Format::BC2Unorm: return VK_FORMAT_BC2_UNORM_BLOCK;
			case Format::BC2UnormSrgb: return VK_FORMAT_BC2_SRGB_BLOCK;
			case Format::BC3Unorm: return VK_FORMAT_BC3_UNORM_BLOCK;
			case Format::BC3UnormSrgb: return VK_FORMAT_BC3_SRGB_BLOCK;
			case Format::BC4Unorm: return VK_FORMAT_BC4_UNORM_BLOCK;
			case Format::BC5Unorm: return VK_FORMAT_BC5_UNORM_BLOCK;
			case Format::BC6HUfloat: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
			case Format::BC7Unorm: return VK_FORMAT_BC7_UNORM_BLOCK;
			case Format::BC7UnormSrgb: return VK_FORMAT_BC7_SRGB_BLOCK;
			default: return VK_FORMAT_UNDEFINED;
			}
		}

		// What a texture state means to Vulkan: its layout, and the stages and accesses that use it.
		struct StateInfo
		{
			VkImageLayout layout;
			VkPipelineStageFlags2 stages;
			VkAccessFlags2 access;
		};

		StateInfo Info(ResourceState state)
		{
			constexpr VkPipelineStageFlags2 kShaders = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			switch (state)
			{
			case ResourceState::RenderTarget:
				return { VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
					VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT };
			case ResourceState::DepthWrite:
				return { VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
					VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT };
			case ResourceState::ShaderResource:
				return { VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kShaders, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT };
			case ResourceState::UnorderedAccess:
				return { VK_IMAGE_LAYOUT_GENERAL, kShaders, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT };
			case ResourceState::CopyDest:
				return { VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT };
			case ResourceState::CopySource:
				return { VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT };
			case ResourceState::Present:
				return { VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE };
			default:
				return { VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT };
			}
		}

		VkCompareOp ToVk(CompareOp op)
		{
			switch (op)
			{
			case CompareOp::Never: return VK_COMPARE_OP_NEVER;
			case CompareOp::Less: return VK_COMPARE_OP_LESS;
			case CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
			case CompareOp::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
			case CompareOp::Greater: return VK_COMPARE_OP_GREATER;
			case CompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
			case CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
			default: return VK_COMPARE_OP_ALWAYS;
			}
		}

		class VulkanDevice;

		void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, const StateInfo& from, const StateInfo& to, bool discard = false)
		{
			VkImageMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
			barrier.srcStageMask = from.stages == VK_PIPELINE_STAGE_2_NONE ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : from.stages;
			barrier.srcAccessMask = from.access;
			barrier.dstStageMask = to.stages == VK_PIPELINE_STAGE_2_NONE ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : to.stages;
			barrier.dstAccessMask = to.access;
			barrier.oldLayout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : from.layout;
			barrier.newLayout = to.layout;
			barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = image;
			barrier.subresourceRange = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
			VkDependencyInfo dependency{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
			dependency.imageMemoryBarrierCount = 1;
			dependency.pImageMemoryBarriers = &barrier;
			vkCmdPipelineBarrier2(cmd, &dependency);
		}

		class VulkanPipeline final : public Pipeline
		{
		public:
			VulkanPipeline(VulkanDevice& device, VkPipeline pipeline, bool compute, Topology topology);
			~VulkanPipeline() override;
			VkPipeline Handle() const { return mPipeline; }
		private:
			VulkanDevice& mDevice;
			VkPipeline mPipeline;
		};

		class VulkanBuffer final : public Buffer
		{
		public:
			VulkanBuffer(VulkanDevice& device, const BufferDesc& desc);
			~VulkanBuffer() override;
			void Upload(const void* data, uint64_t size, uint64_t dstOffset) override;
			VkBuffer Handle() const { return mBuffer; }
		private:
			VulkanDevice& mDevice;
			VkBuffer mBuffer = VK_NULL_HANDLE;
			VmaAllocation mAllocation = nullptr;
		};

		class VulkanTexture final : public Texture
		{
		public:
			VulkanTexture(VulkanDevice& device, const TextureDesc& desc);
			~VulkanTexture() override;
			void Upload(const SubresourceData* data, uint32_t count, ResourceState finalState) override;
			void UploadRegion(uint32_t x, uint32_t y, uint32_t z, uint32_t width, uint32_t height, uint32_t depth,
				const void* data, uint32_t bytesPerTexel) override;
			VkImage Image() const { return mImage; }
			VkImageView TargetView() const { return mTargetView; }
			VkImageAspectFlags Aspect() const { return mDesc.format == Format::D32Float ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT; }
			void Barrier(VkCommandBuffer cmd, ResourceState state)
			{
				if (mState == state) return;
				ImageBarrier(cmd, mImage, Aspect(), Info(mState), Info(state));
				mState = state;
			}
		private:
			VulkanDevice& mDevice;
			VkImage mImage = VK_NULL_HANDLE;
			VmaAllocation mAllocation = nullptr;
			VkImageView mSampledView = VK_NULL_HANDLE;
			VkImageView mTargetView = VK_NULL_HANDLE;   // storage, colour or depth attachment
		};

		class VulkanCommandList final : public CommandList
		{
		public:
			explicit VulkanCommandList(VulkanDevice& device) : mDevice(device) {}
			VkCommandBuffer cmd = VK_NULL_HANDLE;

			void BeginRendering(const RenderPassDesc& pass) override;
			void EndRendering() override { vkCmdEndRendering(cmd); }
			void SetViewport(float x, float y, float width, float height) override
			{
				// Negative height: D3D's clip space (y up) maps onto the image as on D3D.
				VkViewport viewport{ x, y + height, width, -height, 0.0f, 1.0f };
				vkCmdSetViewport(cmd, 0, 1, &viewport);
			}
			void SetScissor(int32_t left, int32_t top, int32_t right, int32_t bottom) override
			{
				VkRect2D rect{ { std::max(left, 0), std::max(top, 0) }, { static_cast<uint32_t>(std::max(right - left, 0)), static_cast<uint32_t>(std::max(bottom - top, 0)) } };
				vkCmdSetScissor(cmd, 0, 1, &rect);
			}
			void SetPipeline(Pipeline* pipeline) override
			{
				auto* p = static_cast<VulkanPipeline*>(pipeline);
				vkCmdBindPipeline(cmd, p->IsCompute() ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS, p->Handle());
			}
			void SetConstants(const void* data, uint32_t bytes) override;
			void SetViewConstants(const ConstantAllocation& constants) override { mOffsets[0] = static_cast<uint32_t>(constants.offset); BindSet0(); }
			void SetFrameConstants(const ConstantAllocation& constants) override { mOffsets[1] = static_cast<uint32_t>(constants.offset); BindSet0(); }
			void SetVertexBuffer(Buffer& buffer, uint32_t, uint64_t offset) override
			{
				VkBuffer handle = static_cast<VulkanBuffer&>(buffer).Handle();
				vkCmdBindVertexBuffers(cmd, 0, 1, &handle, &offset);
			}
			void SetIndexBuffer(Buffer& buffer, bool sixteenBit, uint64_t offset) override
			{
				vkCmdBindIndexBuffer(cmd, static_cast<VulkanBuffer&>(buffer).Handle(), offset, sixteenBit ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
			}
			void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override
			{
				vkCmdDraw(cmd, vertexCount, instanceCount, firstVertex, firstInstance);
			}
			void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t baseVertex, uint32_t firstInstance) override
			{
				vkCmdDrawIndexed(cmd, indexCount, instanceCount, firstIndex, baseVertex, firstInstance);
			}
			void Dispatch(uint32_t x, uint32_t y, uint32_t z) override { vkCmdDispatch(cmd, x, y, z); }
			void Barrier(Texture& texture, ResourceState before, ResourceState after) override
			{
				auto& t = static_cast<VulkanTexture&>(texture);
				ImageBarrier(cmd, t.Image(), t.Aspect(), Info(before), Info(after));
			}
			void UavBarrier(Texture& texture) override
			{
				auto& t = static_cast<VulkanTexture&>(texture);
				const StateInfo uav = Info(ResourceState::UnorderedAccess);
				ImageBarrier(cmd, t.Image(), t.Aspect(), uav, uav);
			}
			void Timestamp(uint32_t index) override;
			void ResolveTimestamps(uint32_t first, uint32_t count) override;
			void BeginEvent(const char* name) override
			{
				if (!vkCmdBeginDebugUtilsLabelEXT) return;
				VkDebugUtilsLabelEXT label{ VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
				label.pLabelName = name;
				vkCmdBeginDebugUtilsLabelEXT(cmd, &label);
			}
			void EndEvent() override
			{
				if (vkCmdEndDebugUtilsLabelEXT) vkCmdEndDebugUtilsLabelEXT(cmd);
			}

			void StartFrame();

		private:
			void BindSet0();
			VulkanDevice& mDevice;
			uint32_t mOffsets[2] = { 0, 0 };
		};

		class VulkanDevice final : public Device
		{
		public:
			~VulkanDevice() override { Shutdown(); }
			bool Initialize(const DeviceOptions& options, void* window, uint32_t width, uint32_t height, std::string& error);
			void SetWindow(void* window, uint32_t width, uint32_t height) override;
			void Shutdown();

			Backend GetBackend() const override { return Backend::Vulkan; }
			ShaderTarget GetShaderTarget() const override { return ShaderTarget::Spirv; }
			const DeviceCaps& Caps() const override { return mCaps; }

			void BeginFrame() override;
			CommandList& Cmd() override { return mCmd; }
			void EndFrame(bool vsync) override;
			uint32_t FrameIndex() const override { return mFrameIndex; }
			uint64_t FrameNumber() const override { return mFrameNumber; }
			void WaitIdle() override;

			void ResizeSwapChain(uint32_t width, uint32_t height) override
			{
				if (width == 0 || height == 0 || (width == mSwapWidth && height == mSwapHeight)) return;
				mSwapWidth = width;
				mSwapHeight = height;
				mSwapChainDirty = true;
			}
			uint32_t SwapChainWidth() const override { return mSwapWidth; }
			uint32_t SwapChainHeight() const override { return mSwapHeight; }
			Format SwapChainFormat() const override { return mSwapFormat; }

			std::unique_ptr<Buffer> CreateBuffer(const BufferDesc& desc) override { return std::make_unique<VulkanBuffer>(*this, desc); }
			std::unique_ptr<Texture> CreateTexture(const TextureDesc& desc) override { return std::make_unique<VulkanTexture>(*this, desc); }
			std::shared_ptr<Pipeline> CreateGraphicsPipeline(const GraphicsPipelineDesc& desc, const std::vector<uint8_t>& vs,
				const std::vector<uint8_t>& ps, const std::string& name, std::string& error) override;
			std::shared_ptr<Pipeline> CreateComputePipeline(const std::vector<uint8_t>& cs, const std::string& name, std::string& error) override;

			void DeferRelease(std::shared_ptr<void> object) override
			{
				if (!object) return;
				std::lock_guard lock(mReleaseMutex);
				mReleases.push_back({ std::move(object), {}, mFrameNumber + 1 });
			}
			// Runs `destroy` once the GPU has finished every frame that may use what it destroys.
			void Defer(std::function<void()> destroy)
			{
				std::lock_guard lock(mReleaseMutex);
				mReleases.push_back({ {}, std::move(destroy), mFrameNumber + 1 });
			}

			ConstantAllocation AllocateConstants(uint32_t size) override;
			const uint64_t* Timestamps() const override { return mTimestampsMapped; }
			double TimestampTicksToMs() const override { return mTicksToMs; }
			uint32_t DescriptorsInUse() const override { return mHeapAllocated; }
			uint32_t DebugErrorCount() const override { return mDebugErrors; }
			uint32_t DebugWarningCount() const override { return mDebugWarnings; }

			VkDevice Handle() const { return mDevice; }
			VmaAllocator Allocator() const { return mAllocator; }
			VkPipelineLayout Layout() const { return mPipelineLayout; }
			VkDescriptorSet Set0() const { return mSet0; }
			VkDescriptorSet Set1() const { return mSet1; }
			VkQueryPool QueryPool() const { return mQueryPool; }
			VkBuffer TimestampBuffer() const { return mTimestampBuffer; }
			bool HasTimestamps() const { return mTimestamps; }
			uint64_t CurrentFrameValue() const { return mFrameNumber + 1; }

			// Bindless heap slots: a slot freed now is reused once the frames that may use it are done.
			uint32_t AllocateIndex();
			void FreeIndex(uint32_t index);
			void WriteDescriptor(uint32_t index, VkDescriptorType type, const VkDescriptorImageInfo* image, const VkDescriptorBufferInfo* buffer);

			// Records `record` into a one-off command buffer and submits it now without waiting: the
			// queue runs it before any frame submitted later. `staging` is destroyed once it has run.
			void ExecuteAsync(const std::function<void(VkCommandBuffer)>& record, VkBuffer staging, VmaAllocation stagingMemory);
			// A host-visible staging buffer holding `size` bytes (mapped).
			void CreateStaging(uint64_t size, VkBuffer& buffer, VmaAllocation& allocation, void*& mapped);
			void SetName(VkObjectType type, uint64_t handle, const char* name);

			// The back buffer as a colour attachment (BeginRendering with backBuffer): acquires the
			// next swap chain image the first time in a frame. Falls back to a 1x1 sink image while
			// the window can't present (minimized), so the frame's commands stay valid.
			VkImageView AcquireBackBuffer(uint32_t& width, uint32_t& height);

		private:
			struct Release
			{
				std::shared_ptr<void> object;
				std::function<void()> destroy;
				uint64_t frameValue;
			};
			struct AsyncContext
			{
				VkCommandBuffer cmd = VK_NULL_HANDLE;
				uint64_t value = 0;
				VkBuffer staging = VK_NULL_HANDLE;
				VmaAllocation stagingMemory = nullptr;
			};

			bool CreateInstance(const DeviceOptions& options, std::string& error);
			bool CreateSurface(void* window, std::string& error);
			bool PickPhysicalDevice(std::string& error);
			void CreateLogicalDevice();
			void CreateDescriptors();
			void CreateSwapChain(bool vsync);
			void DestroySwapChain();
			void ProcessReleases(uint64_t completedFrameValue, bool all = false);
			void ProcessAsync(bool all = false);
			static VKAPI_ATTR VkBool32 VKAPI_CALL OnDebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
				const VkDebugUtilsMessengerCallbackDataEXT* data, void* user);

			VkInstance mInstance = VK_NULL_HANDLE;
			VkDebugUtilsMessengerEXT mMessenger = VK_NULL_HANDLE;
			VkSurfaceKHR mSurface = VK_NULL_HANDLE;
			VkPhysicalDevice mPhysical = VK_NULL_HANDLE;
			VkDevice mDevice = VK_NULL_HANDLE;
			uint32_t mQueueFamily = 0;
			VkQueue mQueue = VK_NULL_HANDLE;
			VmaAllocator mAllocator = nullptr;
			void* mWindow = nullptr;
			uint32_t mInstanceVersion = 0;
			uint32_t mApi = 0;                     // the device's Vulkan version (up to 1.3)
			bool mMutable = true;                  // mutable descriptors; else three typed arrays
			bool mTimestamps = true;
			bool mAnisotropy = true;
			uint32_t mHeapCapacity = 0;
			uint64_t mConstantRange = 65536;       // what one dynamic uniform buffer descriptor covers
			VkPhysicalDeviceLimits mLimits{};
			VkPhysicalDeviceDescriptorIndexingProperties mDescriptorIndexingProps{};
			bool mDebugUtils = false;
			bool mOldValidationLayer = false;

			VulkanCommandList mCmd{ *this };
			std::array<VkCommandPool, kFramesInFlight> mPools{};
			std::array<VkCommandBuffer, kFramesInFlight> mCmdBuffers{};
			std::array<uint64_t, kFramesInFlight> mFrameValues{};
			VkSemaphore mFrameTimeline = VK_NULL_HANDLE;    // value n: frame n - 1 is done
			uint32_t mFrameIndex = 0;
			uint64_t mFrameNumber = 0;

			// Uploads.
			std::mutex mAsyncMutex;
			VkCommandPool mAsyncPool = VK_NULL_HANDLE;
			VkSemaphore mAsyncTimeline = VK_NULL_HANDLE;
			uint64_t mAsyncValue = 0;
			std::vector<AsyncContext> mAsync;

			// Descriptors.
			std::array<VkSampler, 7> mSamplers{};
			VkDescriptorSetLayout mSet0Layout = VK_NULL_HANDLE, mSet1Layout = VK_NULL_HANDLE;
			VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;
			VkDescriptorPool mDescriptorPool = VK_NULL_HANDLE, mHeapPool = VK_NULL_HANDLE;
			VkDescriptorSet mSet0 = VK_NULL_HANDLE, mSet1 = VK_NULL_HANDLE;
			std::mutex mHeapMutex;
			std::vector<uint32_t> mFreeIndices;
			std::vector<std::pair<uint32_t, uint64_t>> mPendingIndices;   // index, frame value
			uint32_t mHeapAllocated = 0;

			// Constant ring.
			VkBuffer mConstants = VK_NULL_HANDLE;
			VmaAllocation mConstantsMemory = nullptr;
			uint8_t* mConstantsMapped = nullptr;
			uint64_t mConstantOffset = 0;

			// Timestamps.
			VkQueryPool mQueryPool = VK_NULL_HANDLE;
			VkBuffer mTimestampBuffer = VK_NULL_HANDLE;
			VmaAllocation mTimestampMemory = nullptr;
			uint64_t* mTimestampsMapped = nullptr;
			double mTicksToMs = 0.0;

			// Swap chain.
			VkSwapchainKHR mSwapChain = VK_NULL_HANDLE;
			std::vector<VkImage> mSwapImages;
			std::vector<VkImageView> mSwapViews;
			std::vector<VkSemaphore> mRenderDone;       // per swap chain image
			std::array<VkSemaphore, kFramesInFlight> mImageReady{};
			Format mSwapFormat = Format::BGRA8Unorm;
			uint32_t mSwapWidth = 0, mSwapHeight = 0;
			bool mSwapChainDirty = false;
			bool mSwapChainMade = false;
			bool mVsync = true;
			uint32_t mImageIndex = UINT32_MAX;          // acquired this frame
			bool mUsingSink = false;
			std::unique_ptr<VulkanTexture> mSink;

			std::mutex mReleaseMutex;
			std::vector<Release> mReleases;

			DeviceCaps mCaps;
			uint32_t mDebugErrors = 0, mDebugWarnings = 0;
			bool mInitialized = false;
		};

		// =========================================================================================
		// Device setup
		// =========================================================================================

		VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDevice::OnDebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
			const VkDebugUtilsMessengerCallbackDataEXT* data, void* user)
		{
			auto* self = static_cast<VulkanDevice*>(user);
			// Layers before 1.3.250 compare a pipeline's attachment formats with the pipeline bound
			// before it even across render pass instances; the spec only asks for that within one.
			if (self->mOldValidationLayer && data->pMessageIdName && std::strstr(data->pMessageIdName, "VUID-vkCmdBindPipeline-pipeline-0619"))
				return VK_FALSE;
			if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
			{
				++self->mDebugErrors;
				CHERRY_ERROR("Vulkan: %s", data->pMessage);
			}
			else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
			{
				++self->mDebugWarnings;
				CHERRY_WARN("Vulkan: %s", data->pMessage);
			}
			return VK_FALSE;
		}

		bool VulkanDevice::CreateInstance(const DeviceOptions& options, std::string& error)
		{
			if (volkInitialize() != VK_SUCCESS)
			{
				error = "No Vulkan driver was found.";
				return false;
			}
			const uint32_t instanceVersion = volkGetInstanceVersion();
			if (instanceVersion < VK_API_VERSION_1_1)
			{
				error = "The Vulkan driver is older than Vulkan 1.1.";
				return false;
			}
			mInstanceVersion = std::min(instanceVersion, static_cast<uint32_t>(VK_API_VERSION_1_3));

			uint32_t layerCount = 0;
			vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
			std::vector<VkLayerProperties> layers(layerCount);
			vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
			uint32_t extensionCount = 0;
			vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
			std::vector<VkExtensionProperties> extensions(extensionCount);
			vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions.data());
			auto hasLayer = [&](const char* name) { return std::any_of(layers.begin(), layers.end(), [&](const VkLayerProperties& l) { return std::strcmp(l.layerName, name) == 0; }); };
			auto hasExtension = [&](const char* name) { return std::any_of(extensions.begin(), extensions.end(), [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; }); };

			std::vector<const char*> enabledLayers;
			std::vector<const char*> enabledExtensions = { VK_KHR_SURFACE_EXTENSION_NAME, kSurfaceExtension };
			mDebugUtils = hasExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
			if (mDebugUtils) enabledExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
			if (options.debugLayer)
			{
				if (hasLayer("VK_LAYER_KHRONOS_validation"))
				{
					enabledLayers.push_back("VK_LAYER_KHRONOS_validation");
					for (const VkLayerProperties& layer : layers)
						if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0 && layer.specVersion < VK_MAKE_API_VERSION(0, 1, 3, 250))
						{
							mOldValidationLayer = true;
							CHERRY_WARN("Vulkan: the validation layers are from SDK %u.%u.%u; install a newer Vulkan SDK to have the bindless heap checked too",
								VK_API_VERSION_MAJOR(layer.specVersion), VK_API_VERSION_MINOR(layer.specVersion), VK_API_VERSION_PATCH(layer.specVersion));
						}
				}
				else CHERRY_WARN("Vulkan validation layers requested but not installed (the Vulkan SDK has them)");
			}

			VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
			app.pApplicationName = "CherryEngine";
			app.pEngineName = "CherryEngine";
			app.apiVersion = mInstanceVersion;
			VkInstanceCreateInfo info{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
			info.pApplicationInfo = &app;
			info.enabledLayerCount = static_cast<uint32_t>(enabledLayers.size());
			info.ppEnabledLayerNames = enabledLayers.data();
			info.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
			info.ppEnabledExtensionNames = enabledExtensions.data();
			const VkResult result = vkCreateInstance(&info, nullptr, &mInstance);
			if (result != VK_SUCCESS)
			{
				error = "vkCreateInstance failed: " + VkResultName(result);
				return false;
			}
			volkLoadInstance(mInstance);

			if (mDebugUtils && !enabledLayers.empty())
			{
				VkDebugUtilsMessengerCreateInfoEXT messenger{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
				messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
				messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
				messenger.pfnUserCallback = &VulkanDevice::OnDebugMessage;
				messenger.pUserData = this;
				vkCreateDebugUtilsMessengerEXT(mInstance, &messenger, nullptr, &mMessenger);
				CHERRY_LOG("Vulkan validation layers enabled");
			}
			return mWindow ? CreateSurface(mWindow, error) : true;
		}

		bool VulkanDevice::CreateSurface(void* window, std::string& error)
		{
#if defined(_WIN32)
			// Looked up here: vcpkg's volk is built without the platform functions.
			auto create = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(vkGetInstanceProcAddr(mInstance, "vkCreateWin32SurfaceKHR"));
			VkWin32SurfaceCreateInfoKHR info{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
			info.hinstance = GetModuleHandleW(nullptr);
			info.hwnd = static_cast<HWND>(window);
#else
			auto create = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(vkGetInstanceProcAddr(mInstance, "vkCreateAndroidSurfaceKHR"));
			VkAndroidSurfaceCreateInfoKHR info{ VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR };
			info.window = static_cast<ANativeWindow*>(window);
#endif
			if (!create)
			{
				error = "The Vulkan driver can't draw into a window (no surface extension).";
				return false;
			}
			const VkResult result = create(mInstance, &info, nullptr, &mSurface);
			if (result != VK_SUCCESS)
			{
				error = "Creating the window surface failed: " + VkResultName(result);
				return false;
			}
			return true;
		}

		namespace
		{
			// What a device offers of what the backend uses, whether in the core version it
			// reports or as extensions (phones are often Vulkan 1.1 with extensions).
			struct DeviceFeatures
			{
				uint32_t api = 0;
				std::vector<VkExtensionProperties> extensions;
				bool Has(const char* name) const
				{
					return std::any_of(extensions.begin(), extensions.end(), [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
				}

				// Filled by Query.
				VkPhysicalDeviceFeatures2 features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
				VkPhysicalDeviceVulkan12Features v12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
				VkPhysicalDeviceVulkan13Features v13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
				VkPhysicalDeviceDescriptorIndexingFeatures indexing{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES };
				VkPhysicalDeviceTimelineSemaphoreFeatures timeline{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
				VkPhysicalDeviceScalarBlockLayoutFeatures scalar{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES };
				VkPhysicalDeviceDynamicRenderingFeatures dynamicRendering{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES };
				VkPhysicalDeviceSynchronization2Features sync2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES };
				VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutableType{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT };
				bool mutableExtension = false;
				std::vector<const char*> enable;   // the device extensions to enable

				// Chains the structs this device can answer for into `features`.
				void Chain()
				{
					void** next = &features.pNext;
					auto link = [&](auto& s) { *next = &s; next = &s.pNext; s.pNext = nullptr; };
					enable = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
					if (api >= VK_API_VERSION_1_2) link(v12);
					else
					{
						if (Has(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME)) { link(indexing); enable.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME); enable.push_back(VK_KHR_MAINTENANCE_3_EXTENSION_NAME); }
						if (Has(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME)) { link(timeline); enable.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME); }
						if (Has(VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME)) { link(scalar); enable.push_back(VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME); }
					}
					if (api >= VK_API_VERSION_1_3) link(v13);
					else
					{
						if (Has(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME))
						{
							link(dynamicRendering);
							enable.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
							if (api < VK_API_VERSION_1_2) { enable.push_back(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME); enable.push_back(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME); }
						}
						if (Has(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) { link(sync2); enable.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME); }
					}
					mutableExtension = Has(VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME);
					if (mutableExtension) { link(mutableType); enable.push_back(VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME); }
				}

				// The features the backend needs, wherever they live.
				bool DynamicRendering() const { return api >= VK_API_VERSION_1_3 ? v13.dynamicRendering : dynamicRendering.dynamicRendering; }
				bool Sync2() const { return api >= VK_API_VERSION_1_3 ? v13.synchronization2 : sync2.synchronization2; }
				bool Timeline() const { return api >= VK_API_VERSION_1_2 ? v12.timelineSemaphore : timeline.timelineSemaphore; }
				bool Scalar() const { return api >= VK_API_VERSION_1_2 ? v12.scalarBlockLayout : scalar.scalarBlockLayout; }
				bool Bindless() const
				{
					auto ok = [](const auto& f) { return f.runtimeDescriptorArray && f.descriptorBindingPartiallyBound && f.descriptorBindingSampledImageUpdateAfterBind
						&& f.descriptorBindingStorageImageUpdateAfterBind && f.descriptorBindingStorageBufferUpdateAfterBind; };
					return api >= VK_API_VERSION_1_2 ? (v12.descriptorIndexing && ok(v12)) : ok(indexing);
				}
				bool Mutable() const { return mutableExtension && mutableType.mutableDescriptorType; }

				// Keeps only what the backend turns on (everything it reads stays as the device said).
				void Select()
				{
					const VkPhysicalDeviceFeatures supported = features.features;
					features.features = {};
					features.features.samplerAnisotropy = supported.samplerAnisotropy;
					features.features.shaderStorageImageReadWithoutFormat = supported.shaderStorageImageReadWithoutFormat;
					features.features.shaderStorageImageWriteWithoutFormat = supported.shaderStorageImageWriteWithoutFormat;
					features.features.shaderStorageImageExtendedFormats = supported.shaderStorageImageExtendedFormats;
					features.features.fragmentStoresAndAtomics = supported.fragmentStoresAndAtomics;
					features.features.depthBiasClamp = supported.depthBiasClamp;
					features.features.shaderImageGatherExtended = supported.shaderImageGatherExtended;
					auto bindless = [](auto& f)
					{
						auto s = f;
						f = {};
						f.sType = s.sType;
						f.runtimeDescriptorArray = s.runtimeDescriptorArray;
						f.descriptorBindingPartiallyBound = s.descriptorBindingPartiallyBound;
						f.descriptorBindingUpdateUnusedWhilePending = s.descriptorBindingUpdateUnusedWhilePending;
						f.descriptorBindingSampledImageUpdateAfterBind = s.descriptorBindingSampledImageUpdateAfterBind;
						f.descriptorBindingStorageImageUpdateAfterBind = s.descriptorBindingStorageImageUpdateAfterBind;
						f.descriptorBindingStorageBufferUpdateAfterBind = s.descriptorBindingStorageBufferUpdateAfterBind;
						f.shaderSampledImageArrayNonUniformIndexing = s.shaderSampledImageArrayNonUniformIndexing;
						f.shaderStorageImageArrayNonUniformIndexing = s.shaderStorageImageArrayNonUniformIndexing;
						f.shaderStorageBufferArrayNonUniformIndexing = s.shaderStorageBufferArrayNonUniformIndexing;
					};
					if (api >= VK_API_VERSION_1_2)
					{
						const VkPhysicalDeviceVulkan12Features s = v12;
						bindless(v12);
						v12.descriptorIndexing = s.descriptorIndexing;
						v12.timelineSemaphore = s.timelineSemaphore;
						v12.scalarBlockLayout = s.scalarBlockLayout;
						v12.uniformBufferStandardLayout = s.uniformBufferStandardLayout;
					}
					else
					{
						bindless(indexing);
						timeline = { timeline.sType, nullptr, timeline.timelineSemaphore };
						scalar = { scalar.sType, nullptr, scalar.scalarBlockLayout };
					}
					if (api >= VK_API_VERSION_1_3)
					{
						const VkPhysicalDeviceVulkan13Features s = v13;
						v13 = { v13.sType };
						v13.dynamicRendering = s.dynamicRendering;
						v13.synchronization2 = s.synchronization2;
						v13.shaderDemoteToHelperInvocation = s.shaderDemoteToHelperInvocation;
					}
					else
					{
						dynamicRendering = { dynamicRendering.sType, nullptr, dynamicRendering.dynamicRendering };
						sync2 = { sync2.sType, nullptr, sync2.synchronization2 };
					}
					mutableType = { mutableType.sType, nullptr, mutableType.mutableDescriptorType };
					Chain();   // relink the cleared structs
				}
			};

			void QueryFeatures(VkPhysicalDevice device, DeviceFeatures& f)
			{
				VkPhysicalDeviceProperties props;
				vkGetPhysicalDeviceProperties(device, &props);
				f.api = std::min(props.apiVersion, static_cast<uint32_t>(VK_API_VERSION_1_3));
				uint32_t count = 0;
				vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
				f.extensions.resize(count);
				vkEnumerateDeviceExtensionProperties(device, nullptr, &count, f.extensions.data());
				f.Chain();
				vkGetPhysicalDeviceFeatures2(device, &f.features);
			}
		}

		bool VulkanDevice::PickPhysicalDevice(std::string& error)
		{
			uint32_t count = 0;
			vkEnumeratePhysicalDevices(mInstance, &count, nullptr);
			std::vector<VkPhysicalDevice> devices(count);
			vkEnumeratePhysicalDevices(mInstance, &count, devices.data());
			std::string reasons;
			int bestScore = -1;
			for (VkPhysicalDevice device : devices)
			{
				VkPhysicalDeviceProperties props;
				vkGetPhysicalDeviceProperties(device, &props);
				auto reject = [&](const std::string& why) { reasons += std::string("\n  ") + props.deviceName + ": " + why; };
				DeviceFeatures f;
				QueryFeatures(device, f);
				if (!f.Has(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) { reject("no swap chain"); continue; }
				std::string missing;
				if (!f.Bindless()) missing += " descriptor-indexing";
				if (!f.Timeline()) missing += " timeline-semaphores";
				if (!f.DynamicRendering()) missing += " dynamic-rendering";
				if (!f.Sync2()) missing += " synchronization2";
				if (!f.Scalar()) missing += " scalar-block-layout";
				if (!f.features.features.shaderStorageImageWriteWithoutFormat) missing += " storage-image-write-without-format";
				if (!missing.empty()) { reject("missing" + missing); continue; }

				uint32_t familyCount = 0;
				vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
				std::vector<VkQueueFamilyProperties> families(familyCount);
				vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
				uint32_t family = UINT32_MAX;
				for (uint32_t i = 0; i < familyCount; ++i)
				{
					VkBool32 present = VK_TRUE;
					if (mSurface) vkGetPhysicalDeviceSurfaceSupportKHR(device, i, mSurface, &present);
					if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present)
					{
						family = i;
						break;
					}
				}
				if (family == UINT32_MAX) { reject("no graphics queue that can present"); continue; }

				const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
				if (score > bestScore)
				{
					bestScore = score;
					mPhysical = device;
					mQueueFamily = family;
					mCaps.adapterName = props.deviceName;
					mTicksToMs = static_cast<double>(props.limits.timestampPeriod) * 1e-6;
					mTimestamps = families[family].timestampValidBits > 0 && props.limits.timestampPeriod > 0.0f;
				}
			}
			if (!mPhysical)
			{
				error = "No GPU can run the Vulkan renderer:" + reasons;
				return false;
			}
			return true;
		}

		void VulkanDevice::CreateLogicalDevice()
		{
			DeviceFeatures f;
			QueryFeatures(mPhysical, f);
			mApi = f.api;
			mMutable = f.Mutable();
			if (EnvFlag("CHERRY_VK_NO_MUTABLE")) mMutable = false;   // to check the typed-array path on a desktop GPU
			f.Select();
			if (!mMutable && f.mutableExtension)
			{
				f.mutableType.mutableDescriptorType = VK_FALSE;
			}

			const float priority = 1.0f;
			VkDeviceQueueCreateInfo queue{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
			queue.queueFamilyIndex = mQueueFamily;
			queue.queueCount = 1;
			queue.pQueuePriorities = &priority;
			VkDeviceCreateInfo info{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f.features };
			info.queueCreateInfoCount = 1;
			info.pQueueCreateInfos = &queue;
			info.enabledExtensionCount = static_cast<uint32_t>(f.enable.size());
			info.ppEnabledExtensionNames = f.enable.data();
			VK_CHECK(vkCreateDevice(mPhysical, &info, nullptr, &mDevice));
			volkLoadDevice(mDevice);
			// Before Vulkan 1.3 / 1.2 these come from extensions under KHR names.
			if (!vkCmdBeginRendering) vkCmdBeginRendering = vkCmdBeginRenderingKHR;
			if (!vkCmdEndRendering) vkCmdEndRendering = vkCmdEndRenderingKHR;
			if (!vkCmdPipelineBarrier2) vkCmdPipelineBarrier2 = vkCmdPipelineBarrier2KHR;
			if (!vkQueueSubmit2) vkQueueSubmit2 = vkQueueSubmit2KHR;
			if (!vkCmdWriteTimestamp2) vkCmdWriteTimestamp2 = vkCmdWriteTimestamp2KHR;
			if (!vkWaitSemaphores) vkWaitSemaphores = vkWaitSemaphoresKHR;
			if (!vkGetSemaphoreCounterValue) vkGetSemaphoreCounterValue = vkGetSemaphoreCounterValueKHR;
			vkGetDeviceQueue(mDevice, mQueueFamily, 0, &mQueue);

			VkPhysicalDeviceProperties props;
			vkGetPhysicalDeviceProperties(mPhysical, &props);
			mLimits = props.limits;
			mDescriptorIndexingProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES };
			VkPhysicalDeviceProperties2 props2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &mDescriptorIndexingProps };
			vkGetPhysicalDeviceProperties2(mPhysical, &props2);

			VmaVulkanFunctions functions{};
			functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
			functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
			VmaAllocatorCreateInfo allocator{};
			allocator.vulkanApiVersion = mApi;
			allocator.physicalDevice = mPhysical;
			allocator.device = mDevice;
			allocator.instance = mInstance;
			allocator.pVulkanFunctions = &functions;
			VK_CHECK(vmaCreateAllocator(&allocator, &mAllocator));
		}

		void VulkanDevice::CreateDescriptors()
		{
			// The bindless heap's size: as large as the device allows, up to 65536.
			const VkPhysicalDeviceDescriptorIndexingProperties& p = mDescriptorIndexingProps;
			mHeapCapacity = std::min<uint32_t>({ 65536u,
				p.maxDescriptorSetUpdateAfterBindSampledImages, p.maxDescriptorSetUpdateAfterBindStorageImages, p.maxDescriptorSetUpdateAfterBindStorageBuffers,
				p.maxPerStageDescriptorUpdateAfterBindSampledImages, p.maxPerStageDescriptorUpdateAfterBindStorageImages, p.maxPerStageDescriptorUpdateAfterBindStorageBuffers });
			mConstantRange = std::min<uint64_t>(65536, mLimits.maxUniformBufferRange);

			// The fixed samplers s0..s6 (as D3D12's static samplers).
			const bool aniso = mLimits.maxSamplerAnisotropy >= 1.0f;
			auto sampler = [&](VkFilter filter, VkSamplerMipmapMode mip, VkSamplerAddressMode mode, bool anisotropic)
			{
				VkSamplerCreateInfo info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
				info.magFilter = info.minFilter = filter;
				info.mipmapMode = mip;
				info.addressModeU = info.addressModeV = info.addressModeW = mode;
				info.anisotropyEnable = anisotropic && aniso && mAnisotropy ? VK_TRUE : VK_FALSE;
				info.maxAnisotropy = info.anisotropyEnable ? std::min(8.0f, mLimits.maxSamplerAnisotropy) : 1.0f;
				info.maxLod = VK_LOD_CLAMP_NONE;
				info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
				VkSampler result;
				VK_CHECK(vkCreateSampler(mDevice, &info, nullptr, &result));
				return result;
			};
			mSamplers[0] = sampler(VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT, false);
			mSamplers[1] = sampler(VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, false);
			mSamplers[2] = sampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, false);
			mSamplers[3] = sampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, false);
			mSamplers[4] = sampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, true);
			mSamplers[5] = sampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, true);
			{
				// s6: shadow-map comparisons, lit where the reference depth <= the stored depth;
				// outside the map counts as lit.
				VkSamplerCreateInfo info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
				info.magFilter = info.minFilter = VK_FILTER_LINEAR;
				info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
				info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
				info.compareEnable = VK_TRUE;
				info.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
				info.maxLod = VK_LOD_CLAMP_NONE;
				info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
				VK_CHECK(vkCreateSampler(mDevice, &info, nullptr, &mSamplers[6]));
			}

			// Set 0: the view and frame constants, and the samplers.
			std::vector<VkDescriptorSetLayoutBinding> bindings;
			for (uint32_t b = 1; b <= 2; ++b)
				bindings.push_back({ b, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_ALL, nullptr });
			for (uint32_t s = 0; s < mSamplers.size(); ++s)
				bindings.push_back({ kSamplerBinding + s, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_ALL, &mSamplers[s] });
			VkDescriptorSetLayoutCreateInfo set0{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
			set0.bindingCount = static_cast<uint32_t>(bindings.size());
			set0.pBindings = bindings.data();
			VK_CHECK(vkCreateDescriptorSetLayout(mDevice, &set0, nullptr, &mSet0Layout));

			// Set 1: the bindless heap. With mutable descriptors, one array of them; without, one
			// array per type (sampled images, storage images, storage buffers: bindings 0, 1, 2),
			// all indexed by the same bindless index (the shaders' bindings are rewritten to match
			// in CreateModule).
			const VkDescriptorType heapTypes[] = { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
			VkMutableDescriptorTypeListEXT typeList{ 3, heapTypes };
			VkMutableDescriptorTypeCreateInfoEXT mutableInfo{ VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT };
			mutableInfo.mutableDescriptorTypeListCount = 1;
			mutableInfo.pMutableDescriptorTypeLists = &typeList;
			const VkDescriptorBindingFlags heapFlag = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT
				| VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
			std::vector<VkDescriptorSetLayoutBinding> heap;
			if (mMutable) heap.push_back({ 0, VK_DESCRIPTOR_TYPE_MUTABLE_EXT, mHeapCapacity, VK_SHADER_STAGE_ALL, nullptr });
			else for (uint32_t t = 0; t < 3; ++t) heap.push_back({ t, heapTypes[t], mHeapCapacity, VK_SHADER_STAGE_ALL, nullptr });
			const std::vector<VkDescriptorBindingFlags> heapFlags(heap.size(), heapFlag);
			VkDescriptorSetLayoutBindingFlagsCreateInfo flags{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, mMutable ? &mutableInfo : nullptr };
			flags.bindingCount = static_cast<uint32_t>(heapFlags.size());
			flags.pBindingFlags = heapFlags.data();
			VkDescriptorSetLayoutCreateInfo set1{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &flags };
			set1.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
			set1.bindingCount = static_cast<uint32_t>(heap.size());
			set1.pBindings = heap.data();
			VK_CHECK(vkCreateDescriptorSetLayout(mDevice, &set1, nullptr, &mSet1Layout));

			const VkDescriptorSetLayout setLayouts[] = { mSet0Layout, mSet1Layout };
			VkPushConstantRange push{ VK_SHADER_STAGE_ALL, 0, kDrawConstantBytes };
			VkPipelineLayoutCreateInfo layout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
			layout.setLayoutCount = 2;
			layout.pSetLayouts = setLayouts;
			layout.pushConstantRangeCount = 1;
			layout.pPushConstantRanges = &push;
			VK_CHECK(vkCreatePipelineLayout(mDevice, &layout, nullptr, &mPipelineLayout));

			const VkDescriptorPoolSize poolSizes[] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 7 } };
			VkDescriptorPoolCreateInfo pool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
			pool.maxSets = 1;
			pool.poolSizeCount = 2;
			pool.pPoolSizes = poolSizes;
			VK_CHECK(vkCreateDescriptorPool(mDevice, &pool, nullptr, &mDescriptorPool));
			std::vector<VkDescriptorPoolSize> heapSizes;
			if (mMutable) heapSizes.push_back({ VK_DESCRIPTOR_TYPE_MUTABLE_EXT, mHeapCapacity });
			else for (VkDescriptorType type : heapTypes) heapSizes.push_back({ type, mHeapCapacity });
			VkDescriptorPoolCreateInfo heapPool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, mMutable ? &mutableInfo : nullptr };
			heapPool.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
			heapPool.maxSets = 1;
			heapPool.poolSizeCount = static_cast<uint32_t>(heapSizes.size());
			heapPool.pPoolSizes = heapSizes.data();
			VK_CHECK(vkCreateDescriptorPool(mDevice, &heapPool, nullptr, &mHeapPool));

			VkDescriptorSetAllocateInfo allocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
			allocate.descriptorPool = mDescriptorPool;
			allocate.descriptorSetCount = 1;
			allocate.pSetLayouts = &mSet0Layout;
			VK_CHECK(vkAllocateDescriptorSets(mDevice, &allocate, &mSet0));
			allocate.descriptorPool = mHeapPool;
			allocate.pSetLayouts = &mSet1Layout;
			VK_CHECK(vkAllocateDescriptorSets(mDevice, &allocate, &mSet1));

			// The constant ring, and set 0's two windows into it.
			VkBufferCreateInfo ring{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
			ring.size = kConstantBytesPerFrame * kFramesInFlight + mConstantRange;
			ring.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
			VmaAllocationCreateInfo ringMemory{};
			ringMemory.usage = VMA_MEMORY_USAGE_AUTO;
			ringMemory.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
			VmaAllocationInfo ringInfo{};
			VK_CHECK(vmaCreateBuffer(mAllocator, &ring, &ringMemory, &mConstants, &mConstantsMemory, &ringInfo));
			mConstantsMapped = static_cast<uint8_t*>(ringInfo.pMappedData);
			SetName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(mConstants), "ConstantRing");
			VkDescriptorBufferInfo window{ mConstants, 0, mConstantRange };
			VkWriteDescriptorSet writes[2]{};
			for (uint32_t i = 0; i < 2; ++i)
			{
				writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
				writes[i].dstSet = mSet0;
				writes[i].dstBinding = i + 1;
				writes[i].descriptorCount = 1;
				writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
				writes[i].pBufferInfo = &window;
			}
			vkUpdateDescriptorSets(mDevice, 2, writes, 0, nullptr);

			mFreeIndices.resize(mHeapCapacity);
			for (uint32_t i = 0; i < mHeapCapacity; ++i) mFreeIndices[i] = mHeapCapacity - 1 - i;
		}

		bool VulkanDevice::Initialize(const DeviceOptions& options, void* window, uint32_t width, uint32_t height, std::string& error)
		{
			mWindow = window;
			mSwapWidth = std::max(width, 1u);
			mSwapHeight = std::max(height, 1u);
			if (!CreateInstance(options, error)) return false;
			if (!PickPhysicalDevice(error)) return false;
			{
				VkPhysicalDeviceFeatures supported{};
				vkGetPhysicalDeviceFeatures(mPhysical, &supported);
				mAnisotropy = supported.samplerAnisotropy == VK_TRUE;
			}
			CreateLogicalDevice();
			CreateDescriptors();

			VkSemaphoreTypeCreateInfo timeline{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
			timeline.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
			VkSemaphoreCreateInfo semaphore{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &timeline };
			VK_CHECK(vkCreateSemaphore(mDevice, &semaphore, nullptr, &mFrameTimeline));
			VK_CHECK(vkCreateSemaphore(mDevice, &semaphore, nullptr, &mAsyncTimeline));
			VkSemaphoreCreateInfo binary{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
			for (auto& ready : mImageReady) VK_CHECK(vkCreateSemaphore(mDevice, &binary, nullptr, &ready));

			VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
			poolInfo.queueFamilyIndex = mQueueFamily;
			for (uint32_t i = 0; i < kFramesInFlight; ++i)
			{
				VK_CHECK(vkCreateCommandPool(mDevice, &poolInfo, nullptr, &mPools[i]));
				VkCommandBufferAllocateInfo allocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
				allocate.commandPool = mPools[i];
				allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
				allocate.commandBufferCount = 1;
				VK_CHECK(vkAllocateCommandBuffers(mDevice, &allocate, &mCmdBuffers[i]));
			}
			poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
			VK_CHECK(vkCreateCommandPool(mDevice, &poolInfo, nullptr, &mAsyncPool));

			// Timestamp queries, copied into a host-visible buffer by ResolveTimestamps (reset by
			// each frame's command buffer before use).
			VkQueryPoolCreateInfo query{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
			query.queryType = VK_QUERY_TYPE_TIMESTAMP;
			query.queryCount = kTimestampCount;
			VK_CHECK(vkCreateQueryPool(mDevice, &query, nullptr, &mQueryPool));
			VkBufferCreateInfo readback{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
			readback.size = sizeof(uint64_t) * kTimestampCount;
			readback.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			VmaAllocationCreateInfo readbackMemory{};
			readbackMemory.usage = VMA_MEMORY_USAGE_AUTO;
			readbackMemory.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
			VmaAllocationInfo readbackInfo{};
			VK_CHECK(vmaCreateBuffer(mAllocator, &readback, &readbackMemory, &mTimestampBuffer, &mTimestampMemory, &readbackInfo));
			mTimestampsMapped = static_cast<uint64_t*>(readbackInfo.pMappedData);
			std::memset(mTimestampsMapped, 0, readback.size);

			// The sink: the back buffer's stand-in while the window can't present.
			TextureDesc sink;
			sink.format = Format::RGBA8Unorm;
			sink.usage = TextureUsage_RenderTarget;
			sink.name = "SwapChainSink";
			mSink = std::make_unique<VulkanTexture>(*this, sink);

			if (mSurface) CreateSwapChain(true);
			mInitialized = true;
			CHERRY_LOG("GPU: %s | Vulkan %u.%u | bindless heap %u (%s) | swap chain %ux%u", mCaps.adapterName.c_str(),
				VK_API_VERSION_MAJOR(mApi), VK_API_VERSION_MINOR(mApi), mHeapCapacity, mMutable ? "mutable descriptors" : "typed arrays", mSwapWidth, mSwapHeight);
			return true;
		}

		void VulkanDevice::SetWindow(void* window, uint32_t width, uint32_t height)
		{
			if (!mDevice) return;
			mSwapWidth = std::max(width, 1u);
			mSwapHeight = std::max(height, 1u);
			if (window == mWindow && mSurface)
			{
				mSwapChainDirty = true;
				return;
			}
			vkDeviceWaitIdle(mDevice);
			DestroySwapChain();
			if (mSurface) vkDestroySurfaceKHR(mInstance, mSurface, nullptr);
			mSurface = VK_NULL_HANDLE;
			mWindow = window;
			if (window)
			{
				std::string error;
				if (!CreateSurface(window, error)) CHERRY_ERROR("Vulkan: %s", error.c_str());
				VkBool32 present = VK_FALSE;
				if (mSurface) vkGetPhysicalDeviceSurfaceSupportKHR(mPhysical, mQueueFamily, mSurface, &present);
				if (mSurface && !present) CHERRY_ERROR("Vulkan: the new window can't be presented to from this queue");
				mSwapChainDirty = true;
			}
			CHERRY_LOG("Vulkan: surface %s", window ? "recreated" : "released");
		}

		void VulkanDevice::CreateSwapChain(bool vsync)
		{
			if (!mSurface) return;
			VkSurfaceCapabilitiesKHR caps;
			VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(mPhysical, mSurface, &caps));
			VkExtent2D extent = caps.currentExtent;
			if (extent.width == UINT32_MAX) extent = { mSwapWidth, mSwapHeight };
			extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
			extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
			if (extent.width == 0 || extent.height == 0) return;   // minimized: try again later
			mSwapWidth = extent.width;
			mSwapHeight = extent.height;

			uint32_t formatCount = 0;
			vkGetPhysicalDeviceSurfaceFormatsKHR(mPhysical, mSurface, &formatCount, nullptr);
			std::vector<VkSurfaceFormatKHR> formats(formatCount);
			vkGetPhysicalDeviceSurfaceFormatsKHR(mPhysical, mSurface, &formatCount, formats.data());
			VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR } : formats[0];
			// UNORM, not sRGB: the tonemap writes gamma-encoded colour, as on D3D12. The format the
			// first swap chain had is kept when it's offered (the UI pipelines are built for it).
			const VkFormat previous = mSwapChainMade ? ToVk(mSwapFormat) : VK_FORMAT_UNDEFINED;
			for (VkFormat wanted : { previous, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM })
			{
				auto it = std::find_if(formats.begin(), formats.end(), [&](const VkSurfaceFormatKHR& f) { return f.format == wanted && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR; });
				if (wanted != VK_FORMAT_UNDEFINED && it != formats.end()) { chosen = *it; break; }
			}
			const Format format = chosen.format == VK_FORMAT_R8G8B8A8_UNORM ? Format::RGBA8Unorm : Format::BGRA8Unorm;
			if (mSwapChainMade && format != mSwapFormat) CHERRY_WARN("Vulkan: the swap chain format changed; pipelines made for the old one stay");
			mSwapFormat = format;
			mSwapChainMade = true;

			uint32_t modeCount = 0;
			vkGetPhysicalDeviceSurfacePresentModesKHR(mPhysical, mSurface, &modeCount, nullptr);
			std::vector<VkPresentModeKHR> modes(modeCount);
			vkGetPhysicalDeviceSurfacePresentModesKHR(mPhysical, mSurface, &modeCount, modes.data());
			auto hasMode = [&](VkPresentModeKHR mode) { return std::find(modes.begin(), modes.end(), mode) != modes.end(); };
			VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
			if (!vsync) mode = hasMode(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR : hasMode(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
			VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
			for (VkCompositeAlphaFlagBitsKHR candidate : { VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR })
				if (caps.supportedCompositeAlpha & candidate) { alpha = candidate; break; }

			VkSwapchainKHR old = mSwapChain;
			VkSwapchainCreateInfoKHR info{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
			info.surface = mSurface;
			info.minImageCount = std::max(caps.minImageCount, 3u);
			if (caps.maxImageCount) info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
			info.imageFormat = chosen.format;
			info.imageColorSpace = chosen.colorSpace;
			info.imageExtent = extent;
			info.imageArrayLayers = 1;
			info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
			// On phones the compositor applies the display's rotation (pre-rotating would save it
			// that work; later).
			info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
			info.compositeAlpha = alpha;
			info.presentMode = mode;
			info.clipped = VK_TRUE;
			info.oldSwapchain = old;
			VK_CHECK(vkCreateSwapchainKHR(mDevice, &info, nullptr, &mSwapChain));
			if (old)
			{
				for (VkImageView view : mSwapViews) vkDestroyImageView(mDevice, view, nullptr);
				for (VkSemaphore done : mRenderDone) vkDestroySemaphore(mDevice, done, nullptr);
				vkDestroySwapchainKHR(mDevice, old, nullptr);
			}
			mSwapViews.clear();
			mRenderDone.clear();

			uint32_t imageCount = 0;
			vkGetSwapchainImagesKHR(mDevice, mSwapChain, &imageCount, nullptr);
			mSwapImages.resize(imageCount);
			vkGetSwapchainImagesKHR(mDevice, mSwapChain, &imageCount, mSwapImages.data());
			for (VkImage image : mSwapImages)
			{
				VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
				view.image = image;
				view.viewType = VK_IMAGE_VIEW_TYPE_2D;
				view.format = chosen.format;
				view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
				VkImageView handle;
				VK_CHECK(vkCreateImageView(mDevice, &view, nullptr, &handle));
				mSwapViews.push_back(handle);
				VkSemaphoreCreateInfo binary{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
				VkSemaphore done;
				VK_CHECK(vkCreateSemaphore(mDevice, &binary, nullptr, &done));
				mRenderDone.push_back(done);
			}
			mVsync = vsync;
			mSwapChainDirty = false;
			CHERRY_LOG("Vulkan swap chain %ux%u, %u images (transform %u)", extent.width, extent.height, imageCount, static_cast<unsigned>(caps.currentTransform));
		}

		void VulkanDevice::DestroySwapChain()
		{
			for (VkImageView view : mSwapViews) vkDestroyImageView(mDevice, view, nullptr);
			for (VkSemaphore done : mRenderDone) vkDestroySemaphore(mDevice, done, nullptr);
			mSwapViews.clear();
			mRenderDone.clear();
			mSwapImages.clear();
			if (mSwapChain) vkDestroySwapchainKHR(mDevice, mSwapChain, nullptr);
			mSwapChain = VK_NULL_HANDLE;
		}

		void VulkanDevice::Shutdown()
		{
			if (!mDevice)
			{
				if (mSurface) vkDestroySurfaceKHR(mInstance, mSurface, nullptr);
				if (mMessenger) vkDestroyDebugUtilsMessengerEXT(mInstance, mMessenger, nullptr);
				if (mInstance) vkDestroyInstance(mInstance, nullptr);
				mSurface = VK_NULL_HANDLE;
				mMessenger = VK_NULL_HANDLE;
				mInstance = VK_NULL_HANDLE;
				return;
			}
			vkDeviceWaitIdle(mDevice);
			mSink.reset();
			ProcessAsync(true);
			ProcessReleases(UINT64_MAX, true);
			DestroySwapChain();
			for (VkSemaphore ready : mImageReady) if (ready) vkDestroySemaphore(mDevice, ready, nullptr);
			if (mTimestampBuffer) vmaDestroyBuffer(mAllocator, mTimestampBuffer, mTimestampMemory);
			if (mQueryPool) vkDestroyQueryPool(mDevice, mQueryPool, nullptr);
			if (mConstants) vmaDestroyBuffer(mAllocator, mConstants, mConstantsMemory);
			if (mAsyncPool) vkDestroyCommandPool(mDevice, mAsyncPool, nullptr);
			for (VkCommandPool pool : mPools) if (pool) vkDestroyCommandPool(mDevice, pool, nullptr);
			if (mFrameTimeline) vkDestroySemaphore(mDevice, mFrameTimeline, nullptr);
			if (mAsyncTimeline) vkDestroySemaphore(mDevice, mAsyncTimeline, nullptr);
			if (mDescriptorPool) vkDestroyDescriptorPool(mDevice, mDescriptorPool, nullptr);
			if (mHeapPool) vkDestroyDescriptorPool(mDevice, mHeapPool, nullptr);
			if (mPipelineLayout) vkDestroyPipelineLayout(mDevice, mPipelineLayout, nullptr);
			if (mSet0Layout) vkDestroyDescriptorSetLayout(mDevice, mSet0Layout, nullptr);
			if (mSet1Layout) vkDestroyDescriptorSetLayout(mDevice, mSet1Layout, nullptr);
			for (VkSampler sampler : mSamplers) if (sampler) vkDestroySampler(mDevice, sampler, nullptr);
			if (mAllocator) vmaDestroyAllocator(mAllocator);
			vkDestroyDevice(mDevice, nullptr);
			mDevice = VK_NULL_HANDLE;
			Shutdown();   // the instance-level objects
			mInitialized = false;
		}

		void VulkanDevice::SetName(VkObjectType type, uint64_t handle, const char* name)
		{
			if (!mDebugUtils || !vkSetDebugUtilsObjectNameEXT || !name) return;
			VkDebugUtilsObjectNameInfoEXT info{ VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
			info.objectType = type;
			info.objectHandle = handle;
			info.pObjectName = name;
			vkSetDebugUtilsObjectNameEXT(mDevice, &info);
		}

		// =========================================================================================
		// Frames
		// =========================================================================================

		void VulkanDevice::BeginFrame()
		{
			// The frame that last used this slot.
			const uint64_t wait = mFrameValues[mFrameIndex];
			if (wait)
			{
				VkSemaphoreWaitInfo info{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
				info.semaphoreCount = 1;
				info.pSemaphores = &mFrameTimeline;
				info.pValues = &wait;
				VK_CHECK(vkWaitSemaphores(mDevice, &info, UINT64_MAX));
			}
			uint64_t completed = 0;
			vkGetSemaphoreCounterValue(mDevice, mFrameTimeline, &completed);
			ProcessReleases(completed);
			ProcessAsync();

			VK_CHECK(vkResetCommandPool(mDevice, mPools[mFrameIndex], 0));
			mCmd.cmd = mCmdBuffers[mFrameIndex];
			VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
			begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			VK_CHECK(vkBeginCommandBuffer(mCmd.cmd, &begin));
			if (mTimestamps) vkCmdResetQueryPool(mCmd.cmd, mQueryPool, mFrameIndex * kTimestampsPerFrame, kTimestampsPerFrame);
			mCmd.StartFrame();
			mConstantOffset = 0;
			mImageIndex = UINT32_MAX;
			mUsingSink = false;
		}

		VkImageView VulkanDevice::AcquireBackBuffer(uint32_t& width, uint32_t& height)
		{
			if (mImageIndex == UINT32_MAX && !mUsingSink)
			{
				if (mSurface && (mSwapChainDirty || !mSwapChain))
				{
					vkDeviceWaitIdle(mDevice);
					CreateSwapChain(mVsync);
				}
				VkResult result = mSwapChain && mSurface ? vkAcquireNextImageKHR(mDevice, mSwapChain, UINT64_MAX, mImageReady[mFrameIndex], VK_NULL_HANDLE, &mImageIndex) : VK_ERROR_OUT_OF_DATE_KHR;
				if (result == VK_ERROR_OUT_OF_DATE_KHR && mSwapChain && mSurface)
				{
					vkDeviceWaitIdle(mDevice);
					CreateSwapChain(mVsync);
					result = mSwapChain ? vkAcquireNextImageKHR(mDevice, mSwapChain, UINT64_MAX, mImageReady[mFrameIndex], VK_NULL_HANDLE, &mImageIndex) : VK_ERROR_OUT_OF_DATE_KHR;
				}
				if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
				{
					mImageIndex = UINT32_MAX;
					mUsingSink = true;
				}
				else
				{
					ImageBarrier(mCmd.cmd, mSwapImages[mImageIndex], VK_IMAGE_ASPECT_COLOR_BIT, Info(ResourceState::Present), Info(ResourceState::RenderTarget), true);
				}
			}
			if (mUsingSink)
			{
				if (!mSink || mSink->Desc().format != mSwapFormat)
				{
					TextureDesc sink;
					sink.format = mSwapFormat;
					sink.usage = TextureUsage_RenderTarget;
					sink.name = "SwapChainSink";
					mSink = std::make_unique<VulkanTexture>(*this, sink);
				}
				mSink->Barrier(mCmd.cmd, ResourceState::RenderTarget);
				width = height = 1;
				return mSink->TargetView();
			}
			width = mSwapWidth;
			height = mSwapHeight;
			return mSwapViews[mImageIndex];
		}

		void VulkanDevice::EndFrame(bool vsync)
		{
			const bool present = mImageIndex != UINT32_MAX;
			if (present)
				ImageBarrier(mCmd.cmd, mSwapImages[mImageIndex], VK_IMAGE_ASPECT_COLOR_BIT, Info(ResourceState::RenderTarget), Info(ResourceState::Present));
			VK_CHECK(vkEndCommandBuffer(mCmd.cmd));

			const uint64_t signalValue = mFrameNumber + 1;
			VkSemaphoreSubmitInfo waits[1]{};
			VkSemaphoreSubmitInfo signals[2]{};
			uint32_t waitCount = 0, signalCount = 0;
			if (present)
			{
				waits[waitCount++] = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, mImageReady[mFrameIndex], 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0 };
				signals[signalCount++] = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, mRenderDone[mImageIndex], 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };
			}
			signals[signalCount++] = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, mFrameTimeline, signalValue, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };
			VkCommandBufferSubmitInfo cmdInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, mCmd.cmd, 0 };
			VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
			submit.waitSemaphoreInfoCount = waitCount;
			submit.pWaitSemaphoreInfos = waits;
			submit.commandBufferInfoCount = 1;
			submit.pCommandBufferInfos = &cmdInfo;
			submit.signalSemaphoreInfoCount = signalCount;
			submit.pSignalSemaphoreInfos = signals;
			{
				std::lock_guard lock(mAsyncMutex);   // the queue is shared with uploads
				VK_CHECK(vkQueueSubmit2(mQueue, 1, &submit, VK_NULL_HANDLE));
				if (present)
				{
					VkPresentInfoKHR info{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
					info.waitSemaphoreCount = 1;
					info.pWaitSemaphores = &mRenderDone[mImageIndex];
					info.swapchainCount = 1;
					info.pSwapchains = &mSwapChain;
					info.pImageIndices = &mImageIndex;
					const VkResult result = vkQueuePresentKHR(mQueue, &info);
#if defined(_WIN32)
					if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) mSwapChainDirty = true;
#else
					// SUBOPTIMAL on Android means "not pre-rotated": the compositor rotates for us.
					if (result == VK_ERROR_OUT_OF_DATE_KHR) mSwapChainDirty = true;
#endif
					else VK_CHECK(result);
				}
			}
			if (vsync != mVsync)
			{
				mVsync = vsync;
				mSwapChainDirty = true;
			}
			mFrameValues[mFrameIndex] = signalValue;
			mFrameIndex = (mFrameIndex + 1) % kFramesInFlight;
			++mFrameNumber;
		}

		void VulkanDevice::WaitIdle()
		{
			if (!mDevice) return;
			vkDeviceWaitIdle(mDevice);
			uint64_t completed = 0;
			vkGetSemaphoreCounterValue(mDevice, mFrameTimeline, &completed);
			ProcessReleases(completed);
			ProcessAsync();
		}

		void VulkanDevice::ProcessReleases(uint64_t completedFrameValue, bool all)
		{
			std::vector<Release> done;
			{
				std::lock_guard lock(mReleaseMutex);
				auto split = std::stable_partition(mReleases.begin(), mReleases.end(),
					[&](const Release& release) { return !all && release.frameValue > completedFrameValue; });
				done.assign(std::make_move_iterator(split), std::make_move_iterator(mReleases.end()));
				mReleases.erase(split, mReleases.end());
			}
			for (Release& release : done)
			{
				release.object.reset();
				if (release.destroy) release.destroy();
			}
			std::lock_guard lock(mHeapMutex);
			for (auto it = mPendingIndices.begin(); it != mPendingIndices.end(); )
			{
				if (all || it->second <= completedFrameValue)
				{
					mFreeIndices.push_back(it->first);
					--mHeapAllocated;
					it = mPendingIndices.erase(it);
				}
				else ++it;
			}
		}

		ConstantAllocation VulkanDevice::AllocateConstants(uint32_t size)
		{
			const uint64_t aligned = (mConstantOffset + 255) & ~uint64_t(255);
			if (aligned + size > kConstantBytesPerFrame) throw std::runtime_error("Constant ring out of memory for this frame");
			mConstantOffset = aligned + size;
			const uint64_t offset = mFrameIndex * kConstantBytesPerFrame + aligned;
			ConstantAllocation allocation;
			allocation.cpu = mConstantsMapped + offset;
			allocation.offset = offset;
			allocation.size = size;
			return allocation;
		}

		uint32_t VulkanDevice::AllocateIndex()
		{
			std::lock_guard lock(mHeapMutex);
			if (mFreeIndices.empty()) throw std::runtime_error("The bindless descriptor heap is full");
			const uint32_t index = mFreeIndices.back();
			mFreeIndices.pop_back();
			++mHeapAllocated;
			return index;
		}

		void VulkanDevice::FreeIndex(uint32_t index)
		{
			if (index == kInvalidIndex) return;
			std::lock_guard lock(mHeapMutex);
			mPendingIndices.push_back({ index, mFrameNumber + 1 });
		}

		void VulkanDevice::WriteDescriptor(uint32_t index, VkDescriptorType type, const VkDescriptorImageInfo* image, const VkDescriptorBufferInfo* buffer)
		{
			VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
			write.dstSet = mSet1;
			write.dstBinding = mMutable ? 0 : type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ? 0 : type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? 1 : 2;
			write.dstArrayElement = index;
			write.descriptorCount = 1;
			write.descriptorType = type;
			write.pImageInfo = image;
			write.pBufferInfo = buffer;
			vkUpdateDescriptorSets(mDevice, 1, &write, 0, nullptr);
		}

		void VulkanDevice::CreateStaging(uint64_t size, VkBuffer& buffer, VmaAllocation& allocation, void*& mapped)
		{
			VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
			info.size = std::max<uint64_t>(size, 4);
			info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
			VmaAllocationCreateInfo memory{};
			memory.usage = VMA_MEMORY_USAGE_AUTO;
			memory.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
			VmaAllocationInfo allocationInfo{};
			VK_CHECK(vmaCreateBuffer(mAllocator, &info, &memory, &buffer, &allocation, &allocationInfo));
			mapped = allocationInfo.pMappedData;
		}

		void VulkanDevice::ExecuteAsync(const std::function<void(VkCommandBuffer)>& record, VkBuffer staging, VmaAllocation stagingMemory)
		{
			std::lock_guard lock(mAsyncMutex);
			// A command buffer the GPU has finished with, or a new one.
			uint64_t completed = 0;
			vkGetSemaphoreCounterValue(mDevice, mAsyncTimeline, &completed);
			AsyncContext* context = nullptr;
			for (AsyncContext& c : mAsync)
			{
				if (c.value <= completed)
				{
					if (c.staging) vmaDestroyBuffer(mAllocator, c.staging, c.stagingMemory);
					c.staging = VK_NULL_HANDLE;
					if (!context) context = &c;
				}
			}
			if (!context && mAsync.size() >= 64)
			{
				AsyncContext* oldest = &mAsync.front();
				for (AsyncContext& c : mAsync) if (c.value < oldest->value) oldest = &c;
				VkSemaphoreWaitInfo wait{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
				wait.semaphoreCount = 1;
				wait.pSemaphores = &mAsyncTimeline;
				wait.pValues = &oldest->value;
				VK_CHECK(vkWaitSemaphores(mDevice, &wait, UINT64_MAX));
				if (oldest->staging) vmaDestroyBuffer(mAllocator, oldest->staging, oldest->stagingMemory);
				oldest->staging = VK_NULL_HANDLE;
				context = oldest;
			}
			if (!context)
			{
				AsyncContext c;
				VkCommandBufferAllocateInfo allocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
				allocate.commandPool = mAsyncPool;
				allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
				allocate.commandBufferCount = 1;
				VK_CHECK(vkAllocateCommandBuffers(mDevice, &allocate, &c.cmd));
				mAsync.push_back(c);
				context = &mAsync.back();
			}
			VK_CHECK(vkResetCommandBuffer(context->cmd, 0));
			VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
			begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			VK_CHECK(vkBeginCommandBuffer(context->cmd, &begin));
			record(context->cmd);
			// Everything copied is visible to whatever the queue runs later (the frames).
			VkMemoryBarrier2 visible{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
			visible.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
			visible.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			visible.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
			visible.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
			VkDependencyInfo dependency{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
			dependency.memoryBarrierCount = 1;
			dependency.pMemoryBarriers = &visible;
			vkCmdPipelineBarrier2(context->cmd, &dependency);
			VK_CHECK(vkEndCommandBuffer(context->cmd));

			context->value = ++mAsyncValue;
			context->staging = staging;
			context->stagingMemory = stagingMemory;
			VkSemaphoreSubmitInfo signal{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, mAsyncTimeline, context->value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };
			VkCommandBufferSubmitInfo cmdInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, context->cmd, 0 };
			VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
			submit.commandBufferInfoCount = 1;
			submit.pCommandBufferInfos = &cmdInfo;
			submit.signalSemaphoreInfoCount = 1;
			submit.pSignalSemaphoreInfos = &signal;
			VK_CHECK(vkQueueSubmit2(mQueue, 1, &submit, VK_NULL_HANDLE));
		}

		void VulkanDevice::ProcessAsync(bool all)
		{
			std::lock_guard lock(mAsyncMutex);
			uint64_t completed = 0;
			vkGetSemaphoreCounterValue(mDevice, mAsyncTimeline, &completed);
			for (AsyncContext& c : mAsync)
			{
				if ((all || c.value <= completed) && c.staging)
				{
					vmaDestroyBuffer(mAllocator, c.staging, c.stagingMemory);
					c.staging = VK_NULL_HANDLE;
				}
			}
			if (all)
			{
				for (AsyncContext& c : mAsync) vkFreeCommandBuffers(mDevice, mAsyncPool, 1, &c.cmd);
				mAsync.clear();
			}
		}

		// =========================================================================================
		// Pipelines
		// =========================================================================================

		VulkanPipeline::VulkanPipeline(VulkanDevice& device, VkPipeline pipeline, bool compute, Topology topology) : mDevice(device), mPipeline(pipeline)
		{
			mCompute = compute;
			mTopology = topology;
		}

		VulkanPipeline::~VulkanPipeline()
		{
			// Deferred by the PipelineLibrary already (it keeps the pipeline alive for the frames in flight).
			vkDestroyPipeline(mDevice.Handle(), mPipeline, nullptr);
		}

		VkShaderModule CreateModule(VkDevice device, const std::vector<uint8_t>& code, bool mutableHeap)
		{
			std::vector<uint32_t> words(code.size() / 4);
			std::memcpy(words.data(), code.data(), words.size() * 4);
			if (!mutableHeap) words = SplitHeapBindings(words);
			VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
			info.codeSize = words.size() * 4;
			info.pCode = words.data();
			VkShaderModule module = VK_NULL_HANDLE;
			vkCreateShaderModule(device, &info, nullptr, &module);
			return module;
		}

		std::shared_ptr<Pipeline> VulkanDevice::CreateComputePipeline(const std::vector<uint8_t>& cs, const std::string& name, std::string& error)
		{
			VkShaderModule module = CreateModule(mDevice, cs, mMutable);
			if (!module)
			{
				error = "vkCreateShaderModule failed";
				return nullptr;
			}
			VkComputePipelineCreateInfo info{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
			info.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr };
			info.layout = mPipelineLayout;
			VkPipeline pipeline = VK_NULL_HANDLE;
			const VkResult result = vkCreateComputePipelines(mDevice, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
			vkDestroyShaderModule(mDevice, module, nullptr);
			if (result != VK_SUCCESS)
			{
				error = "vkCreateComputePipelines failed: " + VkResultName(result);
				return nullptr;
			}
			SetName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(pipeline), name.c_str());
			return std::make_shared<VulkanPipeline>(*this, pipeline, true, Topology::Triangles);
		}

		std::shared_ptr<Pipeline> VulkanDevice::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc, const std::vector<uint8_t>& vs,
			const std::vector<uint8_t>& ps, const std::string& name, std::string& error)
		{
			VkShaderModule vsModule = CreateModule(mDevice, vs, mMutable);
			VkShaderModule psModule = ps.empty() ? VK_NULL_HANDLE : CreateModule(mDevice, ps, mMutable);
			if (!vsModule || (!ps.empty() && !psModule))
			{
				if (vsModule) vkDestroyShaderModule(mDevice, vsModule, nullptr);
				if (psModule) vkDestroyShaderModule(mDevice, psModule, nullptr);
				error = "vkCreateShaderModule failed";
				return nullptr;
			}
			VkPipelineShaderStageCreateInfo stages[2] = {
				{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vsModule, "main", nullptr },
				{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, psModule, "main", nullptr },
			};

			// Vertex layouts (locations from CHERRY_LOCATION in the shaders).
			VkVertexInputBindingDescription binding{ 0, 0, VK_VERTEX_INPUT_RATE_VERTEX };
			std::vector<VkVertexInputAttributeDescription> attributes;
			switch (desc.vertexLayout)
			{
			case VertexLayout::Mesh:
				binding.stride = 48;
				attributes = { { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 },
					{ 2, 0, VK_FORMAT_R32G32_SFLOAT, 24 }, { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32 } };
				break;
			case VertexLayout::MeshCompact:
				binding.stride = 28;
				attributes = { { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_R8G8B8A8_SNORM, 12 },
					{ 2, 0, VK_FORMAT_R16G16_SFLOAT, 16 }, { 3, 0, VK_FORMAT_R16G16B16A16_SFLOAT, 20 } };
				break;
			case VertexLayout::ImGui:
				binding.stride = 20;
				attributes = { { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_R32G32_SFLOAT, 8 }, { 2, 0, VK_FORMAT_R8G8B8A8_UNORM, 16 } };
				break;
			default:
				break;
			}
			VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
			if (!attributes.empty())
			{
				vertexInput.vertexBindingDescriptionCount = 1;
				vertexInput.pVertexBindingDescriptions = &binding;
				vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
				vertexInput.pVertexAttributeDescriptions = attributes.data();
			}
			VkPipelineInputAssemblyStateCreateInfo inputAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
			inputAssembly.topology = desc.topology == Topology::Lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
			viewport.viewportCount = 1;
			viewport.scissorCount = 1;
			VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
			raster.polygonMode = VK_POLYGON_MODE_FILL;
			raster.cullMode = desc.cullMode == CullMode::None ? VK_CULL_MODE_NONE : desc.cullMode == CullMode::Front ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
			// The negative viewport keeps D3D's picture, so D3D's clockwise front faces stay clockwise.
			raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
			raster.depthBiasEnable = desc.depthBias != 0 || desc.slopeScaledDepthBias != 0.0f ? VK_TRUE : VK_FALSE;
			raster.depthBiasConstantFactor = static_cast<float>(desc.depthBias);
			raster.depthBiasSlopeFactor = desc.slopeScaledDepthBias;
			raster.depthBiasClamp = desc.depthBiasClamp;
			raster.lineWidth = 1.0f;
			VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
			multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
			VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
			depth.depthTestEnable = desc.depthTest && desc.depthFormat != Format::Unknown ? VK_TRUE : VK_FALSE;
			depth.depthWriteEnable = desc.depthWrite && desc.depthFormat != Format::Unknown ? VK_TRUE : VK_FALSE;
			depth.depthCompareOp = ToVk(desc.depthFunc);
			VkPipelineColorBlendAttachmentState blend{};
			blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
			if (desc.alphaBlend)
			{
				blend.blendEnable = VK_TRUE;
				blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
				blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
				blend.colorBlendOp = VK_BLEND_OP_ADD;
				blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
				blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
				blend.alphaBlendOp = VK_BLEND_OP_ADD;
			}
			else if (desc.additiveBlend)
			{
				blend.blendEnable = VK_TRUE;
				blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
				blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
				blend.colorBlendOp = VK_BLEND_OP_ADD;
				blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
				blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
				blend.alphaBlendOp = VK_BLEND_OP_ADD;
			}
			std::vector<VkPipelineColorBlendAttachmentState> blends(desc.colorFormats.size(), blend);
			VkPipelineColorBlendStateCreateInfo colorBlend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
			colorBlend.attachmentCount = static_cast<uint32_t>(blends.size());
			colorBlend.pAttachments = blends.data();
			const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
			VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
			dynamic.dynamicStateCount = 2;
			dynamic.pDynamicStates = dynamicStates;

			std::vector<VkFormat> colorFormats;
			for (Format format : desc.colorFormats) colorFormats.push_back(ToVk(format));
			VkPipelineRenderingCreateInfo rendering{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
			rendering.colorAttachmentCount = static_cast<uint32_t>(colorFormats.size());
			rendering.pColorAttachmentFormats = colorFormats.data();
			rendering.depthAttachmentFormat = ToVk(desc.depthFormat);

			VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering };
			info.stageCount = psModule ? 2 : 1;
			info.pStages = stages;
			info.pVertexInputState = &vertexInput;
			info.pInputAssemblyState = &inputAssembly;
			info.pViewportState = &viewport;
			info.pRasterizationState = &raster;
			info.pMultisampleState = &multisample;
			info.pDepthStencilState = &depth;
			info.pColorBlendState = &colorBlend;
			info.pDynamicState = &dynamic;
			info.layout = mPipelineLayout;
			VkPipeline pipeline = VK_NULL_HANDLE;
			const VkResult result = vkCreateGraphicsPipelines(mDevice, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
			vkDestroyShaderModule(mDevice, vsModule, nullptr);
			if (psModule) vkDestroyShaderModule(mDevice, psModule, nullptr);
			if (result != VK_SUCCESS)
			{
				error = "vkCreateGraphicsPipelines failed: " + VkResultName(result);
				return nullptr;
			}
			SetName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(pipeline), name.c_str());
			return std::make_shared<VulkanPipeline>(*this, pipeline, false, desc.topology);
		}

		// =========================================================================================
		// Command list
		// =========================================================================================

		void VulkanCommandList::StartFrame()
		{
			mOffsets[0] = mOffsets[1] = 0;
			const VkDescriptorSet sets[] = { mDevice.Set0(), mDevice.Set1() };
			for (VkPipelineBindPoint point : { VK_PIPELINE_BIND_POINT_GRAPHICS, VK_PIPELINE_BIND_POINT_COMPUTE })
				vkCmdBindDescriptorSets(cmd, point, mDevice.Layout(), 0, 2, sets, 2, mOffsets);
		}

		void VulkanCommandList::BindSet0()
		{
			const VkDescriptorSet set = mDevice.Set0();
			for (VkPipelineBindPoint point : { VK_PIPELINE_BIND_POINT_GRAPHICS, VK_PIPELINE_BIND_POINT_COMPUTE })
				vkCmdBindDescriptorSets(cmd, point, mDevice.Layout(), 0, 1, &set, 2, mOffsets);
		}

		void VulkanCommandList::SetConstants(const void* data, uint32_t bytes)
		{
			vkCmdPushConstants(cmd, mDevice.Layout(), VK_SHADER_STAGE_ALL, 0, bytes, data);
		}

		void VulkanCommandList::BeginRendering(const RenderPassDesc& pass)
		{
			VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
			VkRenderingAttachmentInfo depth{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
			uint32_t width = 1, height = 1;
			const bool hasColor = pass.color || pass.backBuffer;
			if (pass.backBuffer)
			{
				color.imageView = mDevice.AcquireBackBuffer(width, height);
			}
			else if (pass.color)
			{
				color.imageView = static_cast<VulkanTexture*>(pass.color)->TargetView();
				width = pass.color->Width();
				height = pass.color->Height();
			}
			if (hasColor)
			{
				color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				color.loadOp = pass.clearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
				color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
				std::memcpy(color.clearValue.color.float32, pass.clearValue, sizeof(pass.clearValue));
			}
			if (pass.depth)
			{
				depth.imageView = static_cast<VulkanTexture*>(pass.depth)->TargetView();
				depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
				depth.loadOp = pass.clearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
				depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
				depth.clearValue.depthStencil.depth = pass.depthValue;
				if (!hasColor)
				{
					width = pass.depth->Width();
					height = pass.depth->Height();
				}
			}
			VkRenderingInfo info{ VK_STRUCTURE_TYPE_RENDERING_INFO };
			info.renderArea = { { 0, 0 }, { width, height } };
			info.layerCount = 1;
			info.colorAttachmentCount = hasColor ? 1 : 0;
			info.pColorAttachments = hasColor ? &color : nullptr;
			info.pDepthAttachment = pass.depth ? &depth : nullptr;
			vkCmdBeginRendering(cmd, &info);
			SetViewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
			SetScissor(0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height));
		}

		void VulkanCommandList::Timestamp(uint32_t index)
		{
			if (mDevice.HasTimestamps()) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mDevice.QueryPool(), index);
		}

		void VulkanCommandList::ResolveTimestamps(uint32_t first, uint32_t count)
		{
			if (mDevice.HasTimestamps()) vkCmdCopyQueryPoolResults(cmd, mDevice.QueryPool(), first, count, mDevice.TimestampBuffer(), sizeof(uint64_t) * first,
				sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
		}

		// =========================================================================================
		// Buffers
		// =========================================================================================

		VulkanBuffer::VulkanBuffer(VulkanDevice& device, const BufferDesc& desc) : mDevice(device)
		{
			mDesc = desc;
			const bool upload = desc.heap == HeapKind::Upload;
			VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
			info.size = std::max<uint64_t>(desc.size, 4);
			info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
				| VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
			VmaAllocationCreateInfo memory{};
			memory.usage = VMA_MEMORY_USAGE_AUTO;
			if (upload) memory.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
			VmaAllocationInfo allocationInfo{};
			VK_CHECK(vmaCreateBuffer(device.Allocator(), &info, &memory, &mBuffer, &mAllocation, &allocationInfo));
			mMapped = upload ? allocationInfo.pMappedData : nullptr;
			device.SetName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(mBuffer), desc.name);

			// SRV and UAV are both storage buffer descriptors (StructuredBuffer and ByteAddressBuffer
			// compile to storage buffers).
			VkDescriptorBufferInfo whole{ mBuffer, 0, VK_WHOLE_SIZE };
			if (desc.createSrv)
			{
				mSrvIndex = device.AllocateIndex();
				device.WriteDescriptor(mSrvIndex, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &whole);
			}
			if (desc.createUav)
			{
				mUavIndex = device.AllocateIndex();
				device.WriteDescriptor(mUavIndex, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &whole);
			}
		}

		VulkanBuffer::~VulkanBuffer()
		{
			mDevice.FreeIndex(mSrvIndex);
			mDevice.FreeIndex(mUavIndex);
			VulkanDevice& device = mDevice;
			VkBuffer buffer = mBuffer;
			VmaAllocation allocation = mAllocation;
			mDevice.Defer([&device, buffer, allocation]() { vmaDestroyBuffer(device.Allocator(), buffer, allocation); });
		}

		void VulkanBuffer::Upload(const void* data, uint64_t size, uint64_t dstOffset)
		{
			if (mMapped)
			{
				std::memcpy(static_cast<uint8_t*>(mMapped) + dstOffset, data, static_cast<size_t>(size));
				return;
			}
			VkBuffer staging;
			VmaAllocation stagingMemory;
			void* mapped;
			mDevice.CreateStaging(size, staging, stagingMemory, mapped);
			std::memcpy(mapped, data, static_cast<size_t>(size));
			VkBuffer target = mBuffer;
			mDevice.ExecuteAsync([&](VkCommandBuffer cmd)
			{
				VkBufferCopy copy{ 0, dstOffset, size };
				vkCmdCopyBuffer(cmd, staging, target, 1, &copy);
			}, staging, stagingMemory);
		}

		// =========================================================================================
		// Textures
		// =========================================================================================

		VulkanTexture::VulkanTexture(VulkanDevice& device, const TextureDesc& desc) : mDevice(device)
		{
			mDesc = desc;
			const bool is3D = desc.dimension == TextureDimension::Texture3D;
			const VkFormat format = ToVk(desc.format);
			const VkFormat sampledFormat = ToVk(desc.srvFormat != Format::Unknown ? desc.srvFormat : desc.format);
			const bool depth = desc.format == Format::D32Float;

			VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
			info.imageType = is3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
			info.format = format;
			info.extent = { desc.width, desc.height, is3D ? desc.depth : 1u };
			info.mipLevels = desc.mipLevels;
			info.arrayLayers = 1;
			info.samples = VK_SAMPLE_COUNT_1_BIT;
			info.tiling = VK_IMAGE_TILING_OPTIMAL;
			info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
			if (desc.usage & TextureUsage_ShaderResource) info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
			if (desc.usage & TextureUsage_UnorderedAccess) info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
			if (desc.usage & TextureUsage_RenderTarget) info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			if (desc.usage & TextureUsage_DepthStencil) info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
			if (sampledFormat != format) info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
			info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			VmaAllocationCreateInfo memory{};
			memory.usage = VMA_MEMORY_USAGE_AUTO;
			if (desc.usage & (TextureUsage_RenderTarget | TextureUsage_DepthStencil)) memory.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
			VK_CHECK(vmaCreateImage(device.Allocator(), &info, &memory, &mImage, &mAllocation, nullptr));
			device.SetName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(mImage), desc.name);

			const VkImageAspectFlags aspect = Aspect();
			auto makeView = [&](VkFormat viewFormat)
			{
				VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
				view.image = mImage;
				view.viewType = is3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
				view.format = viewFormat;
				view.subresourceRange = { aspect, 0, desc.mipLevels, 0, 1 };
				VkImageView handle;
				VK_CHECK(vkCreateImageView(device.Handle(), &view, nullptr, &handle));
				return handle;
			};
			if (desc.usage & TextureUsage_ShaderResource)
			{
				mSampledView = makeView(sampledFormat);
				mSrvIndex = device.AllocateIndex();
				VkDescriptorImageInfo image{ VK_NULL_HANDLE, mSampledView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
				device.WriteDescriptor(mSrvIndex, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &image, nullptr);
			}
			if (desc.usage & (TextureUsage_UnorderedAccess | TextureUsage_RenderTarget | TextureUsage_DepthStencil))
			{
				// Storage and attachments use mip 0 in the resource format.
				VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
				view.image = mImage;
				view.viewType = is3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
				view.format = format;
				view.subresourceRange = { aspect, 0, 1, 0, 1 };
				VK_CHECK(vkCreateImageView(device.Handle(), &view, nullptr, &mTargetView));
			}
			if (desc.usage & TextureUsage_UnorderedAccess)
			{
				mUavIndex = device.AllocateIndex();
				VkDescriptorImageInfo image{ VK_NULL_HANDLE, mTargetView, VK_IMAGE_LAYOUT_GENERAL };
				device.WriteDescriptor(mUavIndex, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &image, nullptr);
			}
			(void)depth;

			// From UNDEFINED into the state it starts in.
			mState = desc.initialState;
			const VkImage image = mImage;
			const StateInfo target = Info(desc.initialState);
			device.ExecuteAsync([&](VkCommandBuffer cmd)
			{
				ImageBarrier(cmd, image, aspect, Info(ResourceState::Common), target, true);
			}, VK_NULL_HANDLE, nullptr);
		}

		VulkanTexture::~VulkanTexture()
		{
			mDevice.FreeIndex(mSrvIndex);
			mDevice.FreeIndex(mUavIndex);
			VulkanDevice& device = mDevice;
			VkImage image = mImage;
			VmaAllocation allocation = mAllocation;
			VkImageView sampled = mSampledView, target = mTargetView;
			mDevice.Defer([&device, image, allocation, sampled, target]()
			{
				if (sampled) vkDestroyImageView(device.Handle(), sampled, nullptr);
				if (target) vkDestroyImageView(device.Handle(), target, nullptr);
				vmaDestroyImage(device.Allocator(), image, allocation);
			});
		}

		void VulkanTexture::Upload(const SubresourceData* data, uint32_t count, ResourceState finalState)
		{
			const bool blocks = IsBlockCompressed(mDesc.format);
			const uint32_t unit = FormatBytes(mDesc.format);
			// Mip levels one after another in the staging buffer, each tightly packed.
			struct Level { uint64_t offset; uint32_t width, height, depth, rowBytes, rows; };
			std::vector<Level> levels;
			uint64_t total = 0;
			uint32_t w = mDesc.width, h = mDesc.height, d = mDesc.dimension == TextureDimension::Texture3D ? mDesc.depth : 1;
			for (uint32_t i = 0; i < count; ++i)
			{
				Level level{ total, w, h, d, blocks ? std::max(1u, (w + 3) / 4) * unit : w * unit, blocks ? std::max(1u, (h + 3) / 4) : h };
				levels.push_back(level);
				total += (static_cast<uint64_t>(level.rowBytes) * level.rows * d + 15) & ~uint64_t(15);
				w = std::max(w / 2, 1u);
				h = std::max(h / 2, 1u);
				d = std::max(d / 2, 1u);
			}
			VkBuffer staging;
			VmaAllocation stagingMemory;
			void* mappedVoid;
			mDevice.CreateStaging(total, staging, stagingMemory, mappedVoid);
			uint8_t* mapped = static_cast<uint8_t*>(mappedVoid);
			std::vector<VkBufferImageCopy> copies;
			for (uint32_t i = 0; i < count; ++i)
			{
				const Level& level = levels[i];
				const uint8_t* src = static_cast<const uint8_t*>(data[i].data);
				for (uint32_t slice = 0; slice < level.depth; ++slice)
					for (uint32_t row = 0; row < level.rows; ++row)
						std::memcpy(mapped + level.offset + (static_cast<uint64_t>(slice) * level.rows + row) * level.rowBytes,
							src + static_cast<uint64_t>(slice) * data[i].slicePitch + static_cast<uint64_t>(row) * data[i].rowPitch, level.rowBytes);
				VkBufferImageCopy copy{};
				copy.bufferOffset = level.offset;
				copy.imageSubresource = { Aspect(), i, 0, 1 };
				copy.imageExtent = { level.width, level.height, level.depth };
				copies.push_back(copy);
			}
			mDevice.ExecuteAsync([&](VkCommandBuffer cmd)
			{
				Barrier(cmd, ResourceState::CopyDest);
				vkCmdCopyBufferToImage(cmd, staging, mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(copies.size()), copies.data());
				Barrier(cmd, finalState);
			}, staging, stagingMemory);
		}

		void VulkanTexture::UploadRegion(uint32_t x, uint32_t y, uint32_t z, uint32_t width, uint32_t height, uint32_t depth,
			const void* data, uint32_t bytesPerTexel)
		{
			if (width == 0 || height == 0 || depth == 0) return;
			const uint64_t size = static_cast<uint64_t>(width) * height * depth * bytesPerTexel;
			VkBuffer staging;
			VmaAllocation stagingMemory;
			void* mapped;
			mDevice.CreateStaging(size, staging, stagingMemory, mapped);
			std::memcpy(mapped, data, static_cast<size_t>(size));
			const ResourceState restore = mState;
			mDevice.ExecuteAsync([&](VkCommandBuffer cmd)
			{
				Barrier(cmd, ResourceState::CopyDest);
				VkBufferImageCopy copy{};
				copy.imageSubresource = { Aspect(), 0, 0, 1 };
				copy.imageOffset = { static_cast<int32_t>(x), static_cast<int32_t>(y), static_cast<int32_t>(z) };
				copy.imageExtent = { width, height, depth };
				vkCmdCopyBufferToImage(cmd, staging, mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
				Barrier(cmd, restore);
			}, staging, stagingMemory);
		}
	}

	std::unique_ptr<Device> CreateVulkanDevice(const DeviceOptions& options, void* nativeWindow, uint32_t width, uint32_t height, std::string& error)
	{
		auto device = std::make_unique<VulkanDevice>();
		try
		{
			if (!device->Initialize(options, nativeWindow, width, height, error)) return nullptr;
		}
		catch (const std::exception& e)
		{
			error = e.what();
			return nullptr;
		}
		return device;
	}
}

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
