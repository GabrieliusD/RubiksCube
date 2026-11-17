#pragma once
#include "Renderer/RenderTypes.h"
#include <vector>
#include <optional>

// Slot array addressed by generational handles. Destroyed slots bump their
// generation so stale handles are detected instead of aliasing new resources.
template <typename T, typename HandleT>
class ResourcePool
{
public:
	HandleT Add(T&& value)
	{
		uint32_t index;
		if (!mFree.empty())
		{
			index = mFree.back();
			mFree.pop_back();
			mItems[index] = std::move(value);
		}
		else
		{
			index = static_cast<uint32_t>(mItems.size());
			mItems.emplace_back(std::move(value));
			mGenerations.push_back(0);
		}
		HandleT handle;
		handle.index = index;
		handle.generation = mGenerations[index];
		return handle;
	}

	bool IsValid(HandleT handle) const
	{
		return handle.index < mItems.size() && mItems[handle.index].has_value() && mGenerations[handle.index] == handle.generation;
	}

	T* Get(HandleT handle)
	{
		return IsValid(handle) ? &*mItems[handle.index] : nullptr;
	}

	const T* Get(HandleT handle) const
	{
		return IsValid(handle) ? &*mItems[handle.index] : nullptr;
	}

	void Remove(HandleT handle)
	{
		if (!IsValid(handle)) return;
		mItems[handle.index].reset();
		++mGenerations[handle.index];
		mFree.push_back(handle.index);
	}

	void Clear()
	{
		mItems.clear();
		mGenerations.clear();
		mFree.clear();
	}

	// Number of slots, including empty ones (useful for GPU arrays indexed by slot).
	uint32_t SlotCount() const { return static_cast<uint32_t>(mItems.size()); }
	T* AtSlot(uint32_t slot) { return slot < mItems.size() && mItems[slot] ? &*mItems[slot] : nullptr; }

private:
	std::vector<std::optional<T>> mItems;
	std::vector<uint32_t> mGenerations;
	std::vector<uint32_t> mFree;
};
