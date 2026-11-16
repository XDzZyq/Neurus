/**
 * @file GPUProfiler.hpp
 * @brief Section-based GPU timestamp profiler owned by the RenderGraph.
 *
 * The profiler brackets each frame and each pass with GPU timestamp queries:
 *   BeginFrame → (BeginPass/EndPass)* → EndFrame → Resolve
 *
 * The RenderGraph drives BeginPass/EndPass around every pass->Record(), so
 * passes stay completely unaware of GPU profiling. DeferredRenderer drives
 * BeginFrame/EndFrame and calls Resolve() once the frame slot's fence has
 * signaled.
 *
 * Query layout per frame slot (queriesPerFrame = 2 + 2 * kMaxPasses):
 *   slotBase + 0            frame begin (eTopOfPipe)
 *   slotBase + 1            frame end   (eBottomOfPipe)
 *   slotBase + 2 + 2*i + 0  pass i begin
 *   slotBase + 2 + 2*i + 1  pass i end
 *
 * The profiler owns its own slot cursor (advanced in EndFrame), which stays in
 * lockstep with the renderer's frame-in-flight index because BeginFrame/
 * EndFrame are called exactly once per successfully recorded frame. Resolve()
 * reads the slot about to be reused this frame (its fence has already
 * signaled), giving results a 1-2 frame lag by design. A per-slot "written"
 * guard skips never-recorded slots so the first reads never touch an
 * uninitialized query (VUID-vkGetQueryPoolResults-None-09401).
 *
 * Opt-in: the pool is only created when the device reports
 * `timestampComputeAndGraphics` and the graphics queue family supports
 * timestamps. When unavailable every method is a safe no-op and Resolve()
 * returns false.
 */

#pragma once

#include <vulkan/vulkan_raii.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace neurus {

class GPUProfiler
{
public:
	/** @brief Upper bound on instrumented passes per frame slot. */
	static constexpr uint32_t kMaxPasses = 64;

	/** @brief One resolved pass timing (name + GPU ms), returned by Sections(). */
	struct SectionTime
	{
		std::string name;
		double      gpuMs = 0.0;
	};

	GPUProfiler() = default;
	~GPUProfiler() = default;

	GPUProfiler(const GPUProfiler&)            = delete;
	GPUProfiler& operator=(const GPUProfiler&) = delete;
	GPUProfiler(GPUProfiler&&) noexcept = default;
	GPUProfiler& operator=(GPUProfiler&&) noexcept = default;

	/**
	 * @brief Creates the timestamp query pool.
	 *
	 * Safe to call once at renderer construction; leaves Available() == false
	 * when the device or graphics queue family does not support timestamps.
	 */
	void Initialize(const vk::raii::Device& device,
	                const vk::raii::PhysicalDevice& physicalDevice,
	                uint32_t graphicsQueueFamily,
	                uint32_t maxFramesInFlight);

	/** @brief True when the device supports GPU timestamp queries. */
	bool Available() const { return m_available; }

	/**
	 * @brief Begins a frame: resets the current slot's query range and writes
	 *        the frame-begin timestamp. Clears the slot's section list.
	 */
	void BeginFrame(vk::CommandBuffer cmd);

	/** @brief Opens a pass section and writes its begin timestamp. */
	void BeginPass(vk::CommandBuffer cmd, std::string_view name);

	/** @brief Closes the current pass section and writes its end timestamp. */
	void EndPass(vk::CommandBuffer cmd);

	/**
	 * @brief Ends a frame: writes the frame-end timestamp and advances the
	 *        internal slot cursor to the next frame-in-flight.
	 */
	void EndFrame(vk::CommandBuffer cmd);

	/**
	 * @brief Reads back the slot about to be reused this frame.
	 *
	 * Must be called after that slot's fence has signaled (before BeginFrame).
	 * Populates Sections() and FrameGpuMs() on success.
	 *
	 * @return false when results are not ready (startup / skipped slot) so the
	 *         caller keeps the previous profile.
	 */
	bool Resolve();

	/** @brief Per-pass GPU timings from the last successful Resolve(). */
	const std::vector<SectionTime>& Sections() const { return m_resolved; }

	/** @brief Total GPU frame time (ms) from the last successful Resolve(). */
	double FrameGpuMs() const { return m_resolvedFrameMs; }

private:
	/** @brief One instrumented pass: its two query slots within the pool. */
	struct Section
	{
		std::string name;
		uint32_t    beginQuery = 0;
		uint32_t    endQuery   = 0;
	};

	static uint32_t QueriesPerFrame() { return 2 + 2 * kMaxPasses; }
	uint32_t SlotBase(uint32_t slot) const { return slot * QueriesPerFrame(); }

	std::unique_ptr<vk::raii::QueryPool> m_pool;
	bool     m_available = false;
	double   m_timestampPeriodNs = 1.0;
	uint32_t m_maxFramesInFlight = 2;

	uint32_t m_currentSlot = 0; ///< Slot used by the frame currently recording.

	std::vector<std::vector<Section>> m_slotSections; ///< Per-slot open/closed sections.
	std::vector<uint8_t>              m_written;       ///< Per-slot: recorded at least once.

	// --- Resolve output (last successful readback) ---
	std::vector<SectionTime> m_resolved;
	double                   m_resolvedFrameMs = 0.0;
	mutable std::vector<uint64_t> m_results;           ///< Scratch readback buffer.
};

} // namespace neurus
