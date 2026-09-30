/**
 * @file GizmoCache.h
 * @brief Cross-frame GPU storage for the modal transform gizmo's segment and point SSBOs.
 *
 * The renderer-side half of the transform gizmo, and a structural sibling of
 * DebugCache: `GizmoDrawList` is the Vulkan-free payload the Editor builds,
 * GizmoCache is where it lands on the GPU, and GizmoPass only reads what it finds
 * here. The buffers live in RenderCache rather than inside the pass for the same
 * reason every other cross-frame GPU resource does (MeshGPU, EnvironmentGPU,
 * LightingCache, DebugCache): the pass then owns nothing but pipelines and
 * descriptors, and the upload happens once per frame from the one place allowed to
 * write the cache.
 *
 * Host-visible memory (CPUBuffer) for the same reason DebugCache uses it, only more
 * so: a gesture rebuilds its guide geometry on every cursor move, so this buffer is
 * rewritten on essentially every frame a modal is live. A device-local GPUBuffer
 * write is a staging copy plus a submit plus a waitIdle, which is far more than a
 * ~200-byte memcpy is worth.
 *
 * ## Deliberately simpler than DebugCache, in two ways
 *
 *  1. **No revision gate.** DebugCache compares `DebugDrawList::revision` to skip
 *     the memcpy on a still viewport, which pays off for thousands of retained
 *     scene-debug primitives. A gizmo guide peaks at a few dozen arc chords and one
 *     point, and it changes whenever the cursor or camera moves — which is when a
 *     gesture is live and when it is not, nothing is drawn at all. So
 *     `GizmoDrawList` carries no revision counter and Update() writes
 *     unconditionally: the compare would cost more reasoning than the copy it saves.
 *  2. **No x-ray partition.** GizmoPass has no depth attachment and disables depth
 *     testing for the whole pass, so every guide is already x-ray and there is no
 *     `xraySegmentStart`/`xrayPointStart` to record or clamp.
 *
 * `EnsureCapacity` is kept verbatim from DebugCache, 4096-byte floor included. That
 * floor is load-bearing rather than incidental: it is what guarantees both buffers
 * are non-null once Update() has run for a frame index, and a null VkBuffer is not
 * something a descriptor may name. Reusing it also removes the overflow case a
 * fixed-capacity allocation would have to clamp.
 */

#pragma once

#include "../buffers/CPUBuffer.h"

#include <vulkan/vulkan_raii.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace neurus {

struct GizmoDrawList;

/**
 * @brief Owns the per-frame-in-flight gizmo geometry buffers and their counts.
 *
 * One slot per frame in flight: a slot is refilled while another may still be in
 * the GPU's hands, which is the whole reason the ring exists. Slots are created on
 * demand — `Update(frameIndex, list)` grows the ring to cover the frame indices the
 * renderer actually uses, so no frames-in-flight count has to be threaded through
 * RenderCache's constructor.
 *
 * Capacity grows with the geometry and never shrinks. A slot's buffer is replaced
 * when the list outgrows it, which swaps the VkBuffer handle, so consumers must
 * re-write their descriptors from the cache every frame (GizmoPass does, exactly as
 * DebugPass and LightingPass do). That is legal precisely because a frame waits on
 * its own fence before recording: the slot being rewritten is not in flight.
 *
 * Non-copyable (owns GPU buffers), non-movable (CPUBuffer holds a mapping).
 */
class GizmoCache
{
public:
	/**
	 * @brief Creates an empty cache; buffers are allocated on the first Update().
	 *
	 * @param device          Logical device (borrowed, must outlive this object).
	 * @param physicalDevice  Physical device for memory type queries.
	 */
	GizmoCache(const vk::raii::Device& device,
	           const vk::raii::PhysicalDevice& physicalDevice);

	GizmoCache(const GizmoCache&) = delete;
	GizmoCache& operator=(const GizmoCache&) = delete;

	/**
	 * @brief Copies @p list into @p frameIndex's buffers, unconditionally.
	 *
	 * Called once per frame by RenderCache::UpdateGizmoDraw() before any pass
	 * records. Unlike DebugCache::Update() there is no revision gate — see the file
	 * comment. An empty list still updates the counts to zero, which is what makes a
	 * finished gesture's guide disappear.
	 *
	 * @param frameIndex  Frame-in-flight index; the ring grows to include it.
	 * @param list        This frame's gizmo guide geometry.
	 */
	void Update(uint32_t frameIndex, const GizmoDrawList& list);

	/**
	 * @brief What a frame's slot currently holds, as GizmoPass needs to draw it.
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
	};

	/**
	 * @brief Returns what @p frameIndex's slot holds (all-empty for an unused index).
	 * @param frameIndex Frame-in-flight index, as passed to Update().
	 */
	FrameView GetFrame(uint32_t frameIndex) const;

	/** @brief Number of ring slots currently allocated (== highest frame index + 1). */
	size_t GetFrameCount() const { return m_slots.size(); }

private:
	/// @brief One ring slot: the buffers and counts a single in-flight frame reads.
	struct Slot
	{
		std::unique_ptr<CPUBuffer> segments;
		std::unique_ptr<CPUBuffer> points;

		uint32_t segmentCount = 0;
		uint32_t pointCount = 0;
	};

	/**
	 * @brief Ensures @p buffer holds at least @p bytes, reallocating if not.
	 *
	 * Grows to the next power of two at or above the request (and never shrinks) so
	 * a list that creeps upwards does not reallocate every frame. A request of 0
	 * still allocates the minimum when the buffer does not exist yet: a frame with
	 * any gizmo geometry at all writes both descriptor bindings, and a null buffer
	 * handle is not something a descriptor may name.
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
