/**
 * @file DebugCache.h
 * @brief Cross-frame GPU storage for the debug overlay's segment and point SSBOs.
 *
 * The renderer-side half of the debug overlay (issue #22). `DebugDrawList` is the
 * Vulkan-free payload the Editor flattens; DebugCache is where it lands on the
 * GPU, and DebugPass only reads what it finds here. Keeping the buffers in
 * RenderCache rather than inside the pass puts them where every other
 * cross-frame GPU resource already lives (MeshGPU, EnvironmentGPU,
 * LightingCache), so the pass owns nothing but pipelines and descriptors and the
 * upload happens once per frame instead of once per pass that wants the data.
 *
 * Why host-visible memory (CPUBuffer) and not a device-local GPUBuffer: the
 * Editor re-flattens the list on every `RenderResetEvent`, and CameraController
 * raises one on every camera change, so a drag re-uploads on every frame. Each
 * GPUBuffer write is a staging copy plus a queue submit plus a waitIdle, and one
 * waitIdle costs more than issue #22's whole 0.5 ms budget for 10,000 segments.
 * A revision check skips the memcpy on the frames where nothing moved, so the
 * cost on a still viewport is one integer compare either way.
 */

#pragma once

#include "../buffers/CPUBuffer.h"

#include <vulkan/vulkan_raii.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace neurus {

struct DebugDrawList;

/**
 * @brief Owns the per-frame-in-flight debug geometry buffers and their counts.
 *
 * One slot per frame in flight, so a slot can be refilled while another may
 * still be in the GPU's hands. Slots are created on demand by Update();
 * capacity grows with the geometry and never shrinks. A replaced buffer swaps
 * its VkBuffer handle, so consumers re-write descriptors from the cache every
 * frame - legal because each frame waits on its own fence before recording.
 *
 * Non-copyable (owns GPU buffers), non-movable (CPUBuffer holds a mapping).
 */
class DebugCache
{
public:
	/**
	 * @brief Creates an empty cache; buffers are allocated on the first Update().
	 *
	 * @param device          Logical device (borrowed, must outlive this object).
	 * @param physicalDevice  Physical device for memory type queries.
	 */
	DebugCache(const vk::raii::Device& device,
	           const vk::raii::PhysicalDevice& physicalDevice);

	DebugCache(const DebugCache&) = delete;
	DebugCache& operator=(const DebugCache&) = delete;

	/**
	 * @brief Copies @p list into @p frameIndex's buffers if its revision moved on.
	 *
	 * Called once per frame before any pass records. An unchanged revision
	 * returns immediately, leaving the buffers and cached counts untouched; an
	 * empty list still zeroes the counts so a cleared overlay disappears.
	 *
	 * @param frameIndex  Frame-in-flight index; the ring grows to include it.
	 * @param list        This frame's flattened debug geometry.
	 */
	void Update(uint32_t frameIndex, const DebugDrawList& list);

	/**
	 * @brief What a frame's slot currently holds.
	 *
	 * The buffers are null and the counts zero until the first Update() for
	 * that frame index; a consumer that finds no buffer draws nothing.
	 */
	struct FrameView
	{
		const CPUBuffer* segments = nullptr;  ///< Segment SSBO, or null if never filled.
		const CPUBuffer* points = nullptr;    ///< Point SSBO, or null if never filled.
		uint32_t segmentCount = 0;            ///< Segments resident in the buffer.
		uint32_t pointCount = 0;              ///< Points resident in the buffer.
		uint32_t xraySegmentStart = 0;        ///< First x-ray segment within segmentCount.
		uint32_t xrayPointStart = 0;          ///< First x-ray point within pointCount.
	};

	/**
	 * @brief Returns what @p frameIndex's slot holds (all-empty for an unused index).
	 * @param frameIndex Frame-in-flight index, as passed to Update().
	 */
	FrameView GetFrame(uint32_t frameIndex) const;

	/** @brief Number of ring slots currently allocated (== highest frame index + 1). */
	size_t GetFrameCount() const { return m_slots.size(); }

private:
	/** @brief One ring slot: the buffers and cached counts a single in-flight frame reads. */
	struct Slot
	{
		std::unique_ptr<CPUBuffer> segments;
		std::unique_ptr<CPUBuffer> points;

		/**
		 * @brief DebugDrawList::revision this slot's buffers currently hold.
		 *
		 * UINT64_MAX means "never uploaded", since zero would wrongly skip a
		 * list that legitimately sits at revision 0.
		 */
		uint64_t uploadedRevision = UINT64_MAX;

		uint32_t segmentCount = 0;
		uint32_t pointCount = 0;
		uint32_t xraySegmentStart = 0;
		uint32_t xrayPointStart = 0;
	};

	/**
	 * @brief Ensures @p buffer holds at least @p bytes, reallocating if not.
	 *
	 * Grow-only, to the next power of two at or above the request. A request
	 * of 0 still allocates the minimum, because a frame with any geometry at
	 * all writes both descriptor bindings and a null buffer is not bindable.
	 *
	 * @param buffer     Slot member to grow in place.
	 * @param bytes      Required capacity; the floor is the minimum allocation.
	 * @param debugName  Debug name for a newly created buffer.
	 */
	void EnsureCapacity(std::unique_ptr<CPUBuffer>& buffer,
	                    vk::DeviceSize bytes,
	                    const char* debugName);

	// --- References (non-owning) ---
	const vk::raii::Device* m_device;
	const vk::raii::PhysicalDevice* m_physicalDevice;

	// --- Ring of per-frame-in-flight buffers ---
	std::vector<Slot> m_slots;
};

} // namespace neurus
