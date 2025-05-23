#pragma once
#include "Common.h"
#include <mutex>
#include <vector>
#include <d3dx12.h>

class DescriptorHeap;
struct descriptor_handle
{
	D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
	D3D12_GPU_DESCRIPTOR_HANDLE gpu{};

	constexpr bool is_valid() { return cpu.ptr != 0; }
	constexpr bool is_shader_visible() { return gpu.ptr != 0; }

#ifdef _DEBUG
private:
	friend class DescriptorHeap;
	DescriptorHeap* container{ nullptr };
	UINT32 index{ 0 };
#endif
};
class DescriptorHeap
{
public:
	explicit DescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE type) : _type{ type } {}
	DISABLE_COPY_AND_MOVE(DescriptorHeap);
	~DescriptorHeap()
	{

	}

	bool initialize(UINT32 capacity, bool is_shader_visiblee);
	void release();

	[[nodiscard]] descriptor_handle allocate();
	void free(descriptor_handle& handle);

	constexpr D3D12_DESCRIPTOR_HEAP_TYPE type() const { return _type; }
	constexpr D3D12_CPU_DESCRIPTOR_HANDLE cpu_start() const { return _cpu_start; }
	constexpr D3D12_GPU_DESCRIPTOR_HANDLE gpu_start() const { return _gpu_start; }
	constexpr ID3D12DescriptorHeap* const heap() const { return _heap; }
	constexpr UINT32 capacity() const { return _capacity; }
	constexpr UINT32 size() const { return _size; }
	constexpr UINT32 descriptor_size() const { return _descriptor_size; }
	constexpr bool is_shader_visible() const { return _gpu_start.ptr != 0; }

	void process_deferred_free(UINT32 frame_idx);
private:
	ID3D12DescriptorHeap* _heap;
	D3D12_CPU_DESCRIPTOR_HANDLE _cpu_start{};
	D3D12_GPU_DESCRIPTOR_HANDLE _gpu_start{};
	std::unique_ptr<UINT32[]> _free_handles;
	std::vector<UINT32> _deferred_free_indices[3];
	std::mutex _mutex{};
	UINT32 _capacity = 0;
	UINT32 _size = 0;
	UINT32 _descriptor_size = 0;
	const D3D12_DESCRIPTOR_HEAP_TYPE _type{};
};