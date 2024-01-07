#pragma once
#include <d3d12.h>
#include <memory>

struct descriptor_handle {
	D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
	D3D12_GPU_DESCRIPTOR_HANDLE gpu{};

	constexpr bool is_valid() const { return cpu.ptr != 0; }
	constexpr bool is_shader_visible() const { return gpu.ptr != 0; }
};

class descriptor_heap
{
public:
	explicit descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE type) : _type(type){}
	~descriptor_heap() { }
	bool initialize(unsigned int capacity, bool is_shader_visible, ID3D12Device *device);
	void release();

	[[nodiscard]] descriptor_handle allocate();
	void free(descriptor_handle& handle);

	constexpr D3D12_DESCRIPTOR_HEAP_TYPE type() const { return _type; }
	constexpr D3D12_CPU_DESCRIPTOR_HANDLE cpu_start() const { return _cpu_start; }
	constexpr D3D12_GPU_DESCRIPTOR_HANDLE gpu_start() const { return _gpu_start; }
	constexpr ID3D12DescriptorHeap* const heap() const { return _heap; }
	constexpr unsigned int capacity() const { return _capacity; }
	constexpr unsigned int size() const { return _size; }
	constexpr unsigned int descriptor_size() const { return _descriptor_size; }
private:
	ID3D12DescriptorHeap* _heap;
	D3D12_CPU_DESCRIPTOR_HANDLE _cpu_start;
	D3D12_GPU_DESCRIPTOR_HANDLE _gpu_start;
	std::unique_ptr<unsigned int[]> _free_handles{};
	unsigned int _capacity{ 0 };
	unsigned int _size{ 0 };
	unsigned int _descriptor_size{};
	const D3D12_DESCRIPTOR_HEAP_TYPE _type{};
	bool _is_shader_visible;
};

