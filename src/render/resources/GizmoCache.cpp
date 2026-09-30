/**
 * @file GizmoCache.cpp
 * @brief Unconditional upload of the gizmo's guide geometry into per-frame SSBOs.
 *
 * DebugCache.cpp's body with the revision gate and the x-ray clamps removed; see
 * GizmoCache.h for why both are absent rather than merely unused.
 */

#include "GizmoCache.h"

#include "scene/GizmoDrawList.h"

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

GizmoCache::GizmoCache(const vk::raii::Device& device,
                       const vk::raii::PhysicalDevice& physicalDevice)
	: m_device(&device)
	, m_physicalDevice(&physicalDevice)
{
}

void GizmoCache::EnsureCapacity(std::unique_ptr<CPUBuffer>& buffer,
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

void GizmoCache::Update(uint32_t frameIndex, const GizmoDrawList& list)
{
	if (frameIndex >= m_slots.size())
	{
		m_slots.resize(frameIndex + 1);
	}

	Slot& slot = m_slots[frameIndex];

	const auto segmentCount = static_cast<uint32_t>(list.segments.size());
	const auto pointCount = static_cast<uint32_t>(list.points.size());

	const vk::DeviceSize segmentBytes = segmentCount * sizeof(OverlaySegment);
	const vk::DeviceSize pointBytes = pointCount * sizeof(OverlayPointSprite);

	EnsureCapacity(slot.segments, segmentBytes, "Gizmo Segment SSBO");
	EnsureCapacity(slot.points, pointBytes, "Gizmo Point SSBO");

	if (segmentBytes > 0)
	{
		slot.segments->Write(list.segments.data(), segmentBytes);
	}

	if (pointBytes > 0)
	{
		slot.points->Write(list.points.data(), pointBytes);
	}

	// Written even when zero: this is what makes a finished gesture's guide vanish.
	slot.segmentCount = segmentCount;
	slot.pointCount = pointCount;
}

GizmoCache::FrameView GizmoCache::GetFrame(uint32_t frameIndex) const
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
	return view;
}

} // namespace neurus
