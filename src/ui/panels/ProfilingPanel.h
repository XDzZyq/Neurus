/**
 * @file ProfilingPanel.h
 * @brief GPU rendering profiling dock panel (real-time per-pass timings).
 *
 * Replaces the Debug-menu viewport overlay: displays the FrameProfile
 * returned by DeferredRenderer::DrawFrame() as a tree - a bold "Frame"
 * totals row (CPU record ms, GPU frame ms, total draws/dispatches) with one
 * child row per pass (CPU ms, GPU ms, draws, dispatches).
 *
 * Architecture:
 * - UIPanel subclass (matches Outliner / RenderConfigPanel pattern)
 * - QTreeWidget with 5 columns inside a QVBoxLayout
 * - Refresh() copies the latest FrameProfile from UIContext and rebuilds the
 *   tree only when the frame totals actually changed (no per-frame churn)
 * - Displayed timings (CPU/GPU ms) are smoothed with a per-value exponential
 *   moving average so the numbers don't jitter frame to frame; draw/dispatch
 *   counts are shown raw
 * - No Vulkan or Renderer headers - reads the Vulkan-free ProfilingData POD
 *   through the opaque UIContext::frameProfile pointer
 *
 * @note UI Layer - communicates via UIContext, no direct Renderer coupling.
 */

#pragma once

#include "UIPanel.h"
#include "items/ProfilingRow.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class QTreeWidget;
class QTreeWidgetItem;

namespace neurus
{

struct FrameProfile;

class ProfilingPanel : public UIPanel
{
	Q_OBJECT

public:
	static constexpr PanelType kType = PanelType::Profiling;

	explicit ProfilingPanel(QWidget* parent = nullptr);
	~ProfilingPanel() override = default;

	ProfilingPanel(const ProfilingPanel&) = delete;
	ProfilingPanel& operator=(const ProfilingPanel&) = delete;

	/**
	 * @brief Per-frame refresh from UIContext.
	 *
	 * Reads the opaque frameProfile pointer and repopulates the tree when the
	 * frame totals change. GPU timestamps lag the CPU data by 1-2 frames by
	 * design (fence-based readback); the panel just renders what it is given.
	 *
	 * @param ctx Read-only UI context carrying Editor/Project state.
	 */
	void Refresh(const UIContext& ctx) override;

	/** @brief Re-applies column headers and the head-row label in the active language. */
	void Retranslate() override;

private:
	/** @brief Repopulates the tree from the latest profile, recycling rows. */
	void Populate(const FrameProfile& profile);

	/**
	 * @brief Ensures the pass-row pool has at least @p needed child rows,
	 *        creating new ones under the persistent Frame item as necessary.
	 *
	 * Rows are recycled across frames (never destroyed), mirroring the
	 * Outliner/OutlinerRow pool pattern — no per-frame `new QTreeWidgetItem`.
	 */
	bool EnsureRowPool(std::size_t needed);

	QTreeWidget* m_tree = nullptr;

	/// Persistent bold "Frame" totals row - dirty-checked columns.
	std::unique_ptr<ProfilingHead> m_frameHead;

	/// Persistent per-pass child rows — grows as needed, never shrinks.
	/// Each ProfilingRow wraps a QTreeWidgetItem and dirty-checks its columns
	/// so the steady-state per-frame repopulate skips redundant setText calls
	/// (see ui-system.instructions.md -> "Lazy Updates via Logical State Tracking").
	std::vector<std::unique_ptr<ProfilingRow>> m_rowPool;

	// --- Change detection: repopulate only when a new profile arrived ---
	bool     m_hasProfile   = false;
	double   m_lastCpuMs    = 0.0;
	double   m_lastGpuMs    = 0.0;
	uint32_t m_lastPassCount = 0;

	// --- EMA smoothing of displayed timings (noise reduction) ---
	// State persists across frames and is reseeded when the pass set changes or
	// the profile is lost. Smaller alpha = smoother but laggier. A negative
	// value means "not yet seeded" (ms is always >= 0), so the next sample is
	// taken verbatim instead of ramping up from zero. GPU EMAs only advance on
	// frames where GPU timings are actually ready.
	static constexpr double kEmaAlpha = 0.1;

	double              m_frameCpuMsEma = -1.0;
	double              m_frameGpuMsEma = -1.0;
	std::vector<double> m_passCpuMsEma; ///< Per-pass; -1.0 until first sample.
	std::vector<double> m_passGpuMsEma; ///< Per-pass; -1.0 until first sample.
};

} // namespace neurus
