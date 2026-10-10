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
 * One slot per frame in flight: a slot is refilled while another may still be in
 * the GPU's hands, which is the whole reason the ring exists. Slots are created
 * on demand — `Update(frameIndex, list)` grows the ring to cover the frame
 * indices the renderer actually uses, so no frames-in-flight count has to be
 * threaded through RenderCache's constructor.
 *
 * Capacity grows with the geometry. A slot's buffer is replaced when the list
 * outgrows it, which swaps the VkBuffer handle, so consumers must re-write their
 * descriptors from the cache every frame (DebugPass does, exactly as LightingPass
 * does for the light SSBOs). That is legal precisely because a frame waits on its
 * own fence before recording: the slot being rewritten is not in flight.
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
	 * Called once per frame by RenderCache::UpdateDebugDraw() before any pass
	 * records. An unchanged revision returns immediately, leaving the buffers and
	 * the cached counts as they were — the common case, since debug objects are
	 * stateful. An empty list still updates the counts to zero so a cleared
	 * overlay disappears.
	 *
	 * @param frameIndex  Frame-in-flight index; the ring grows to include it.
	 * @param list        This frame's flattened debug geometry.
	 */
	void Update(uint32_t frameIndex, const DebugDrawList& list);

	/**
	 * @brief What a frame's slot currently holds, as DebugPass needs to draw it.
	 *
	 * The buffers are null and the counts zero until the first Update() for that
	 * frame index; a consumer that finds no buffer simply draws nothing.
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
	/**
	 * @brief One ring slot: the buffers a single in-flight frame reads from.
	 *
	 * The cached counts live here alongside the buffers: when the revision
	 * matches, GetFrame() reports them without the list being consulted again.
	 */
	struct Slot
	{
		std::unique_ptr<CPUBuffer> segments;
		std::unique_ptr<CPUBuffer> points;

		/**
		 * @brief DebugDrawList::revision this slot's buffers currently hold.
		 *
		 * UINT64_MAX means "never uploaded". Zero would be wrong: a list that
		 * legitimately sits at revision 0 would be skipped forever.
		 */
		uint64_t uploadedRevision = UINT64_MAX;

		uint32_t segmentCount = 0;
		uint32_t pointCount = 0;
		uint32_t xraySegmentStart = 0;
		uint32_t xrayPointStart = 0;
	};

	/**
	 * @brief Ensures @p slot's @p buffer holds at least @p bytes, reallocating if not.
	 *
	 * Grows to the next power of two at or above the request (and never shrinks)
	 * so a list that creeps upwards does not reallocate every frame. A request of
	 * 0 still allocates the minimum when the buffer does not exist yet: a frame
	 * that has any debug geometry at all writes both descriptor bindings, and a
	 * null buffer handle is not something a descriptor may name.
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
