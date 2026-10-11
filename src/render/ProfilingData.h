/**
 * @file ProfilingData.h
 * @brief Vulkan-free data structures for per-frame rendering profiling.
 *
 * Produced by the Renderer (DeferredRenderer collects per-pass counters and
 * CPU/GPU timings while recording), returned from DeferredRenderer::DrawFrame(),
 * stored by the Editor, and consumed by the UI overlay. Pure POD - no Vulkan,
 * Qt, or scene dependencies so any layer can read it.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace neurus {

/**
 * @brief One pass's CPU/GPU timing + counters for a single frame.
 *
 * @c gpuMs is only meaningful when the device supports timestamp queries
 * (see FrameProfile::gpuTimingAvailable); otherwise it stays 0.
 */
struct PassProfile
{
	std::string name;        ///< PassIO name (e.g. "GeometryPass").
	double      cpuMs = 0.0; ///< CPU command-recording time for this pass.
	double      gpuMs = 0.0; ///< GPU time for this pass (0 when unavailable).
	uint32_t    drawCalls  = 0;
	uint32_t    dispatches = 0;
};

/**
 * @brief Per-frame rendering profile: CPU + GPU timing and draw-call metrics.
 *
 * GPU timings are read back after the frame's fence signals, so they lag the
 * CPU/counter data by one to two frames (standard for fence-based timestamp
 * readback).
 */
struct FrameProfile
{
	std::vector<PassProfile> passes; ///< One entry per pass, execution order.
	double   cpuRecordMs = 0.0;      ///< Total recordFrame CPU time (ms).
	double   gpuTotalMs  = 0.0;      ///< GPU frame time, first->last timestamp (ms).
	uint32_t drawCalls   = 0;        ///< Sum of per-pass draw calls.
	uint32_t dispatches  = 0;        ///< Sum of per-pass dispatches.
	uint32_t passCount   = 0;        ///< Number of passes executed this frame.
	bool     gpuTimingAvailable = false; ///< Device supports timestamp queries.
	bool     gpuReady    = false;    ///< GPU timestamps were read back this frame.
};

} // namespace neurus