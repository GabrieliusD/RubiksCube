#include "Graphics/DescriptorHeap.h"
#include <mutex>
#include "D3DCore.h"


bool DescriptorHeap::initialize(UINT32 capacity, bool is_shader_visible) 
{
	std::lock_guard lock{ _mutex };
	ID3D12Device* const device{ D3DCore::Device().Get()};
	if (_type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV || _type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
	{
		is_shader_visible = false;
	}
	release();

	D3D12_DESCRIPTOR_HEAP_DESC desc{};
	desc.Flags = is_shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	desc.NumDescriptors = capacity;
	desc.Type = _type;
	HRESULT hr = S_OK;
	DXCall(hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&_heap)));

	if (FAILED(hr))
	{
		release();
		return false;
	}

	_free_handles = std::move(std::make_unique<UINT32[]>(capacity));
	_capacity = capacity;
	_size = 0;
	for (UINT32 i = 0; i < capacity; ++i)
	{
		_free_handles[i] = i;
	}

	for (UINT32 i = 0; i < 3; ++i)
	{
		_deferred_free_indices[i].clear();
	}

	_descriptor_size = device->GetDescriptorHandleIncrementSize(_type);
	_cpu_start = _heap->GetCPUDescriptorHandleForHeapStart();
	_gpu_start = is_shader_visible ? _heap->GetGPUDescriptorHandleForHeapStart() : D3D12_GPU_DESCRIPTOR_HANDLE{ 0 };


	return true;
}

void DescriptorHeap::release()
{
	//core::deferred_release(_heap);
}

descriptor_handle DescriptorHeap::allocate()
{
	std::lock_guard lock{ _mutex };

	const UINT32 index{ _free_handles[_size] };
	const UINT32 offset{ index * _descriptor_size };
	++_size;

	descriptor_handle handle;
	handle.index = index;
	handle.cpu.ptr = _cpu_start.ptr + offset;
	if (is_shader_visible())
	{
		handle.gpu.ptr = _gpu_start.ptr + offset;
	}

	return handle;

	return descriptor_handle();
}

void DescriptorHeap::free(descriptor_handle& handle)
{
	if (handle.is_valid()) return;
	std::lock_guard lock{ _mutex };

	const UINT32 frame_index = 0;
	_deferred_free_indices[frame_index].push_back(handle.index);
	//core::set_deferred_releases_flag();

	handle = {};
}
void DescriptorHeap::process_deferred_free(UINT32 frame_idx)
{
	std::lock_guard lock{ _mutex };

	std::vector<UINT32>& indices = _deferred_free_indices[frame_idx];
	if (!indices.empty())
	{
		for (auto index : indices)
		{
			--_size;
			_free_handles[_size] = index;
		}
		indices.clear();
	}
}

