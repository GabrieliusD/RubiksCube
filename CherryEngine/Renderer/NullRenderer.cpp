// The renderer on platforms without a graphics backend yet (Android until the Vulkan
// backend, Dice/Docs/Android.md A3): resources are bookkept so handles, bounds and materials
// work, ImGui runs its frames, and nothing is drawn.
#include "Renderer/Renderer.h"
#include "Core/Log.h"
#include "imgui.h"
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <thread>
#include <vector>

using namespace DirectX;

namespace
{
	template <typename Tag, typename T>
	struct Pool
	{
		struct Slot { T value{}; uint32_t generation = 1; bool alive = false; };
		std::vector<Slot> slots;
		std::vector<uint32_t> free;

		Handle<Tag> Add(T value)
		{
			uint32_t index;
			if (!free.empty()) { index = free.back(); free.pop_back(); }
			else { index = static_cast<uint32_t>(slots.size()); slots.emplace_back(); }
			Slot& slot = slots[index];
			slot.value = std::move(value);
			slot.alive = true;
			Handle<Tag> handle;
			handle.index = index;
			handle.generation = slot.generation;
			return handle;
		}
		T* Get(Handle<Tag> handle)
		{
			if (!handle.IsValid() || handle.index >= slots.size()) return nullptr;
			Slot& slot = slots[handle.index];
			return slot.alive && slot.generation == handle.generation ? &slot.value : nullptr;
		}
		const T* Get(Handle<Tag> handle) const { return const_cast<Pool*>(this)->Get(handle); }
		void Remove(Handle<Tag> handle)
		{
			if (!Get(handle)) return;
			Slot& slot = slots[handle.index];
			slot.alive = false;
			slot.value = T{};
			++slot.generation;
			free.push_back(handle.index);
		}
	};

	struct NullMesh { BoundingBox bounds; size_t bytes = 0; };
	struct NullShader { std::string error; };
}

struct Renderer::Impl
{
	Pool<MeshTag, NullMesh> meshes;
	Pool<MaterialTag, MaterialDesc> materials;
	Pool<TextureTag, int> textures;
	Pool<VoxelVolumeTag, XMUINT3> volumes;
	Pool<SurfaceShaderTag, NullShader> shaders;
	Pool<LightVolumeTag, int> lightVolumes;
	RenderSettings settings;
	std::vector<ParticleInstance> particles;
	std::vector<MeshInstance> meshInstances;
	DebugDraw debug;
	RenderStats stats;
	std::string adapterName = "none (no graphics backend on this platform yet)";
	std::string shaderError;
	uint32_t width = 0, height = 0, viewWidth = 1, viewHeight = 1;
	bool imguiOwned = false;
	std::chrono::steady_clock::time_point nextFrame{};
};

Renderer::Renderer() : mImpl(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;

MeshHandle Renderer::CreateMesh(const MeshData& data, MeshFormat format)
{
	NullMesh mesh;
	XMFLOAT3 lo{ FLT_MAX, FLT_MAX, FLT_MAX }, hi{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
	for (const Vertex& v : data.vertices)
	{
		lo = { std::min(lo.x, v.position.x), std::min(lo.y, v.position.y), std::min(lo.z, v.position.z) };
		hi = { std::max(hi.x, v.position.x), std::max(hi.y, v.position.y), std::max(hi.z, v.position.z) };
	}
	if (!data.vertices.empty()) BoundingBox::CreateFromPoints(mesh.bounds, XMLoadFloat3(&lo), XMLoadFloat3(&hi));
	mesh.bytes = data.vertices.size() * (format == MeshFormat::Compact ? 28 : sizeof(Vertex)) + data.indices.size() * sizeof(uint32_t);
	return mImpl->meshes.Add(mesh);
}
size_t Renderer::GetMeshBytes(MeshHandle mesh) const { const NullMesh* m = mImpl->meshes.Get(mesh); return m ? m->bytes : 0; }
void Renderer::DestroyMesh(MeshHandle mesh) { mImpl->meshes.Remove(mesh); }
const BoundingBox* Renderer::GetMeshBounds(MeshHandle mesh) const { const NullMesh* m = mImpl->meshes.Get(mesh); return m ? &m->bounds : nullptr; }

TextureHandle Renderer::LoadTexture(const std::wstring&, bool) { return mImpl->textures.Add(0); }
TextureHandle Renderer::CreateTexture(uint32_t, uint32_t, const uint8_t*, bool) { return mImpl->textures.Add(0); }
void Renderer::DestroyTexture(TextureHandle texture) { mImpl->textures.Remove(texture); }

MaterialHandle Renderer::CreateMaterial(const MaterialDesc& desc) { return mImpl->materials.Add(desc); }
void Renderer::SetMaterial(MaterialHandle material, const MaterialDesc& desc) { if (MaterialDesc* m = mImpl->materials.Get(material)) *m = desc; }
const MaterialDesc* Renderer::GetMaterial(MaterialHandle material) const { return mImpl->materials.Get(material); }
void Renderer::DestroyMaterial(MaterialHandle material) { mImpl->materials.Remove(material); }

SurfaceShaderHandle Renderer::CreateSurfaceShader(const std::string&, const std::string&, const SurfaceShaderOptions&, std::function<void(bool)> done)
{
	const SurfaceShaderHandle handle = mImpl->shaders.Add({});
	if (done) done(true);
	return handle;
}
void Renderer::SetSurfaceShaderSource(SurfaceShaderHandle, const std::string&, const SurfaceShaderOptions&, std::function<void(bool)> done) { if (done) done(true); }
bool Renderer::IsSurfaceShaderCompiling(SurfaceShaderHandle) const { return false; }
void Renderer::DestroySurfaceShader(SurfaceShaderHandle shader) { mImpl->shaders.Remove(shader); }
const std::string& Renderer::SurfaceShaderError(SurfaceShaderHandle shader) const
{
	static const std::string none;
	const NullShader* s = mImpl->shaders.Get(shader);
	return s ? s->error : none;
}

VoxelVolumeHandle Renderer::CreateVoxelVolume(const VoxelVolumeDesc& desc) { return mImpl->volumes.Add(desc.size); }
bool Renderer::UpdateVoxelVolume(VoxelVolumeHandle volume, const XMUINT3&, const XMUINT3&, const uint8_t*) { return mImpl->volumes.Get(volume) != nullptr; }
void Renderer::DestroyVoxelVolume(VoxelVolumeHandle volume) { mImpl->volumes.Remove(volume); }
XMUINT3 Renderer::GetVoxelVolumeSize(VoxelVolumeHandle volume) const { const XMUINT3* s = mImpl->volumes.Get(volume); return s ? *s : XMUINT3{ 0, 0, 0 }; }

LightVolumeHandle Renderer::CreateLightVolume(const XMUINT3&, const uint8_t*) { return mImpl->lightVolumes.Add(0); }
bool Renderer::UpdateLightVolume(LightVolumeHandle volume, const XMUINT3&, const XMUINT3&, const uint8_t*) { return mImpl->lightVolumes.Get(volume) != nullptr; }
void Renderer::DestroyLightVolume(LightVolumeHandle volume) { mImpl->lightVolumes.Remove(volume); }

RenderSettings& Renderer::Settings() { return mImpl->settings; }
std::vector<ParticleInstance>& Renderer::Particles() { return mImpl->particles; }
std::vector<MeshInstance>& Renderer::MeshInstances() { return mImpl->meshInstances; }
DebugDraw& Renderer::Debug() { return mImpl->debug; }
const RenderStats& Renderer::Stats() const { return mImpl->stats; }
const std::string& Renderer::AdapterName() const { return mImpl->adapterName; }
void Renderer::ReloadShaders() {}
const std::string& Renderer::ShaderError() const { return mImpl->shaderError; }

void Renderer::SetGraphicsApi(GraphicsApi) {}
void Renderer::BuildAllShaderVariants() {}
bool Renderer::IsCompilingShaders() const { return false; }
GraphicsApi Renderer::GetGraphicsApi() const { return GraphicsApi::D3D12; }
const char* GraphicsApiName(GraphicsApi api) { return api == GraphicsApi::Vulkan ? "Vulkan" : "D3D12"; }

bool Renderer::Initialize(void*, uint32_t width, uint32_t height, std::string&)
{
	Impl& r = *mImpl;
	r.width = r.viewWidth = std::max(width, 1u);
	r.height = r.viewHeight = std::max(height, 1u);
	if (!ImGui::GetCurrentContext())
	{
		ImGui::CreateContext();
		r.imguiOwned = true;
	}
	ImGuiIO& io = ImGui::GetIO();
	io.BackendRendererName = "cherry_null";
	io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
	CHERRY_WARN("No graphics backend on this platform yet: nothing is drawn");
	return true;
}

void Renderer::Shutdown()
{
	if (mImpl->imguiOwned && ImGui::GetCurrentContext()) ImGui::DestroyContext();
	mImpl->imguiOwned = false;
}

void Renderer::OnWindowResized(uint32_t width, uint32_t height) { mImpl->width = width; mImpl->height = height; }
void Renderer::SetNativeWindow(void*, uint32_t width, uint32_t height) { OnWindowResized(width, height); }
void Renderer::SetViewSize(uint32_t width, uint32_t height) { mImpl->viewWidth = std::max(width, 1u); mImpl->viewHeight = std::max(height, 1u); }
uint32_t Renderer::ViewWidth() const { return mImpl->viewWidth; }
uint32_t Renderer::ViewHeight() const { return mImpl->viewHeight; }
unsigned long long Renderer::ViewTextureId() const { return 0; }
unsigned long long Renderer::RenderPreview(MeshHandle, MaterialHandle, uint32_t, float, float) { return 0; }

void Renderer::BeginFrame(float, float deltaTime)
{
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2(static_cast<float>(std::max(mImpl->width, 1u)), static_cast<float>(std::max(mImpl->height, 1u)));
	io.DeltaTime = deltaTime > 0.0f ? deltaTime : 1.0f / 60.0f;
	ImGui::NewFrame();
}

void Renderer::RenderScene(Scene&, const RenderView&)
{
	mImpl->stats = {};
	mImpl->stats.instancesSubmitted = static_cast<uint32_t>(mImpl->meshInstances.size());
	mImpl->meshInstances.clear();
	mImpl->particles.clear();
}

void Renderer::EndFrame(bool)
{
	ImGui::Render();
	// Stand in for vsync (60 Hz), so a game without a backend doesn't spin a core.
	if (mImpl->settings.vsync)
	{
		using clock = std::chrono::steady_clock;
		const clock::time_point now = clock::now();
		if (mImpl->nextFrame < now - std::chrono::milliseconds(50)) mImpl->nextFrame = now;
		mImpl->nextFrame += std::chrono::microseconds(16667);
		std::this_thread::sleep_until(mImpl->nextFrame);
	}
	// No backend uploads ImGui's textures (the font atlas): mark them handled so ImGui stops asking.
	if (ImDrawData* data = ImGui::GetDrawData(); data && data->Textures)
	{
		for (ImTextureData* texture : *data->Textures)
		{
			if (texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_WantUpdates) texture->SetStatus(ImTextureStatus_OK);
			else if (texture->Status == ImTextureStatus_WantDestroy) texture->SetStatus(ImTextureStatus_Destroyed);
		}
	}
}
