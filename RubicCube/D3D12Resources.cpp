#include "D3D12Resources.h"

bool descriptor_heap::initialize(unsigned int capacity, bool is_shader_visible, ID3D12Device* device)
{

    release();
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Flags = is_shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    desc.NumDescriptors = capacity;
    desc.Type = _type;
    desc.NodeMask = 0;

    device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&_heap));

    return true;

    _free_handles = std::move(std::make_unique<unsigned int[]>(capacity));
    _capacity = capacity;
    _size = 0;
    for (unsigned int i = 0; i < capacity; i++) _free_handles[i] = i;

    _descriptor_size = device->GetDescriptorHandleIncrementSize(_type);
    _cpu_start = _heap->GetCPUDescriptorHandleForHeapStart();
    _gpu_start = is_shader_visible ? _heap->GetGPUDescriptorHandleForHeapStart() : D3D12_GPU_DESCRIPTOR_HANDLE{ 0 };
    _is_shader_visible = is_shader_visible;

}

void descriptor_heap::release()
{
}

descriptor_handle descriptor_heap::allocate()
{
    if (_heap)
    {
        const unsigned int index{ _free_handles[_size] };
        const unsigned int offset{ index * _descriptor_size };
        _size++;

        descriptor_handle handle;
        handle.cpu.ptr = _cpu_start.ptr + offset;
        if(_is_shader_visible)
            handle.gpu.ptr = _gpu_start.ptr + offset;
        return handle;
    }
    return descriptor_handle();
}

void descriptor_heap::free(descriptor_handle& handle)
{
    if (!handle.is_valid()) return;
}
