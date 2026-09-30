/**
 * @file DebugCache.cpp
 * @brief Revision-gated upload of the debug overlay's geometry into per-frame SSBOs.
 */

#include "DebugCache.h"

#include "../../scene/DebugDrawList.h"

#include <algorithm>

namespace neurus {

namespace {

/// @brief Smallest allocation: also what an empty kind gets, so its binding is never null.
constexpr vk::DeviceSize kMinCapacity = 4096;

/// @brief Next power of two at or above @p bytes, floored at kMinCapacity.
vk::DeviceSize RoundUpCapacity(vk::DeviceSize bytes)
{
	vk::DeviceSize capacity = kMinCapacity;
	while (capacity < bytes)
	{
		capacity *= 2;
	}
	return capacity;
}

} // namespace

DebugCache::DebugCache(const vk::raii::Device& device,
                       const vk::raii::PhysicalDevice& physicalDevice)
	: m_device(&device)
	, m_physicalDevice(&physicalDevice)
{
}

void DebugCache::EnsureCapacity(std::unique_ptr<CPUBuffer>& buffer,
                                vk::DeviceSize bytes,
                                const char* debugName)
{
	if (buffer && buffer->Capacity() >= bytes)
	{
		return;
	}

	// Replacing the buffer swaps the VkBuffer handle, which is why consumers
	// re-write their descriptors from this cache every frame. Safe here because
	// Update() only ever touches a slot whose frame has already been waited on.
	buffer = std::make_unique<CPUBuffer>(*m_device,
	                                     *m_physicalDevice,
	                                     RoundUpCapacity(bytes),
	                                     vk::BufferUsageFlagBits::eStorageBuffer,
	                                     debugName);
}

void DebugCache::Update(uint32_t frameIndex, const DebugDrawList& list)
{
	if (frameIndex >= m_slots.size())
	{
		m_slots.resize(frameIndex + 1);
	}

	Slot& slot = m_slots[frameIndex];

	// The common case: the list has not been rebuilt since this slot last saw it,
	// so the buffers already hold exactly these bytes.
	if (slot.uploadedRevision == list.revision)
	{
		return;
	}

	const auto segmentCount = static_cast<uint32_t>(list.segments.size());
	const auto pointCount = static_cast<uint32_t>(list.points.size());

	const vk::DeviceSize segmentBytes = segmentCount * sizeof(DebugSegment);
	const vk::DeviceSize pointBytes = pointCount * sizeof(DebugPointSprite);

	EnsureCapacity(slot.segments, segmentBytes, "Debug Segment SSBO");
	EnsureCapacity(slot.points, pointBytes, "Debug Point SSBO");

	if (segmentBytes > 0)
	{
		slot.segments->Write(list.segments.data(), segmentBytes);
	}

	if (pointBytes > 0)
	{
		slot.points->Write(list.points.data(), pointBytes);
	}

	slot.segmentCount = segmentCount;
	slot.pointCount = pointCount;

	// Clamp the partition offsets to the counts: a malformed list that claims an
	// x-ray range beyond its own geometry would otherwise turn into an
	// out-of-bounds draw.
	slot.xraySegmentStart = std::min(list.xraySegmentStart, segmentCount);
	slot.xrayPointStart = std::min(list.xrayPointStart, pointCount);

	slot.uploadedRevision = list.revision;
}

DebugCache::FrameView DebugCache::GetFrame(uint32_t frameIndex) const
{
	if (frameIndex >= m_slots.size())
	{
		return {};
	}

	const Slot& slot = m_slots[frameIndex];

	FrameView view;
	view.segments = slot.segments.get();
	view.points = slot.points.get();
	view.segmentCount = slot.segmentCount;
	view.pointCount = slot.pointCount;
	view.xraySegmentStart = slot.xraySegmentStart;
	view.xrayPointStart = slot.xrayPointStart;
	return view;
}

} // namespace neurus
