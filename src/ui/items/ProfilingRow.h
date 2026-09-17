/**
 * @file ProfilingRow.h
 * @brief Dirty-checked QTreeWidgetItem wrappers for the Profiling panel.
 *
 * Two row types share this file:
 * - ProfilingRow: one per-pass child row (parented to the Frame head).
 * - ProfilingHead: the bold top-level "Frame" totals row (parented to the tree).
 *
 * Both cache the last *displayed* value of each column and only call
 * QTreeWidgetItem::setText when the visible value changed - so the
 * steady-state per-frame repopulate (where only the smoothed timings drift)
 * skips the model dataChanged notifications for the stable columns
 * (name, draws, dispatches) and for timing columns whose EMA drift rounds to
 * the same "X.XX" string.
 *
 * Mirrors the OutlinerRow lazy-update pattern (see ui-system.instructions.md ->
 * "Lazy Updates via Logical State Tracking"): cache the previous value, gate
 * the write behind an equality check.
 *
 * The wrapped QTreeWidgetItem is created once in each constructor; rows are
 * recycled across frames, never destroyed.
 *
 * @note UI Layer - no Vulkan or Renderer dependencies; only Qt + the pass name
 *       string carried in from the Vulkan-free ProfilingData POD.
 */

#pragma once

#include <QString>

#include <cstdint>
#include <string>

class QTreeWidget;
class QTreeWidgetItem;

namespace neurus
{

/**
 * @brief One per-pass row in the Profiling tree (pool-compatible).
 *
 * Columns: 0 = Pass name, 1 = CPU ms, 2 = GPU ms, 3 = Draws, 4 = Dispatches.
 * All setters are dirty-checked; setText/setHidden only fire on a displayed
 * value change.
 */
class ProfilingRow
{
public:
	/**
	 * @brief Constructs a hidden row parented to @p parent.
	 * @param parent Persistent Frame totals QTreeWidgetItem (non-owning).
	 *
	 * Creates the wrapped QTreeWidgetItem once. The row starts hidden; call
	 * setHidden(false) to reveal it (typically from the Populate loop).
	 */
	explicit ProfilingRow(QTreeWidgetItem* parent);

	~ProfilingRow() = default;

	ProfilingRow(const ProfilingRow&) = delete;
	ProfilingRow& operator=(const ProfilingRow&) = delete;

	/**
	 * @brief Updates all five columns, dirty-checking each independently.
	 *
	 * Timing columns compare on centi-ms (2-decimal) resolution so a sub-0.005ms
	 * EMA drift does not trigger a write. The name column compares the raw
	 * std::string so the per-frame QString::fromStdString allocation is skipped
	 * when the pass name is unchanged (the common steady-state case).
	 *
	 * @param name        Pass name (column 0).
	 * @param cpuMs       Smoothed CPU time in ms (column 1).
	 * @param gpuMs       Smoothed GPU time in ms (column 2); ignored when @p gpuShown is false.
	 * @param gpuShown    Whether GPU timings are available this frame (column 2 shows "--" when false).
	 * @param draws       Draw-call count (column 3).
	 * @param dispatches  Dispatch count (column 4).
	 */
	void setPass(const std::string& name, double cpuMs, double gpuMs, bool gpuShown,
	             uint32_t draws, uint32_t dispatches);

	/**
	 * @brief Toggles row visibility; no-op if unchanged.
	 */
	void setHidden(bool hidden);

	/** @brief The wrapped tree item (non-owning; owned by the QTreeWidget). */
	QTreeWidgetItem* item() const { return m_item; }

private:
	QTreeWidgetItem* m_item;

	// --- Cached displayed state (dirty-check inputs) ---
	// Centi-ms values use -1 as the "not yet seeded" sentinel (ms is always >= 0).
	// Draw/dispatch counts use UINT32_MAX as the sentinel (unreachable in practice).
	std::string m_name;              ///< Last written column 0 text.
	int         m_cpuCenti    = -1;  ///< Last written col 1 value in centi-ms.
	int         m_gpuCenti    = -1;  ///< Last written col 2 value in centi-ms.
	bool        m_gpuShown    = false; ///< Whether col 2 last showed a number (true) or "--" (false).
	uint32_t    m_draws       = UINT32_MAX; ///< Last written col 3.
	uint32_t    m_dispatches  = UINT32_MAX; ///< Last written col 4.
	bool        m_hidden      = true;  ///< Last applied hidden state.
};

/**
 * @brief The bold "Frame" totals row at the top of the Profiling tree.
 *
 * Columns: 0 = label ("Frame" or "No profiling data yet"), 1 = CPU ms,
 * 2 = GPU ms, 3 = Draws, 4 = Dispatches. All updates are dirty-checked.
 *
 * Two display modes:
 * - Frame:  col 0 = "Frame", cols 1-4 show numeric totals (dirty-checked).
 * - NoData: col 0 = "No profiling data yet", cols 1-4 cleared.
 */
class ProfilingHead
{
public:
	/**
	 * @brief Constructs the head row as a top-level item of @p tree.
	 *
	 * Creates the QTreeWidgetItem, applies a bold font to column 0, and
	 * starts visible + expanded. No text is written until setFrame() or
	 * setNoData() is called.
	 *
	 * @param tree The owning QTreeWidget (the head is a top-level item).
	 */
	explicit ProfilingHead(QTreeWidget* tree);

	~ProfilingHead() = default;

	ProfilingHead(const ProfilingHead&) = delete;
	ProfilingHead& operator=(const ProfilingHead&) = delete;

	/**
	 * @brief Shows the "Frame" totals with dirty-checked numeric columns.
	 *
	 * Timing columns compare on centi-ms (2-decimal) resolution so a
	 * sub-0.005ms EMA drift does not trigger a write.
	 *
	 * @param cpuMs       Smoothed CPU record time (column 1).
	 * @param gpuMs       Smoothed GPU frame time (column 2); ignored when @p gpuShown is false.
	 * @param gpuShown    Whether GPU timings are available (column 2 shows "--" when false).
	 * @param draws       Total draw calls (column 3).
	 * @param dispatches  Total dispatches (column 4).
	 */
	void setFrame(double cpuMs, double gpuMs, bool gpuShown,
	              uint32_t draws, uint32_t dispatches);

	/**
	 * @brief Switches to the "No profiling data yet" state.
	 *
	 * Sets col 0 to the cached placeholder text (see retranslate) and clears
	 * cols 1-4. No-op if already in NoData mode, so it is safe to call every
	 * frame while the renderer is warming up.
	 */
	void setNoData();

	/**
	 * @brief Toggles row visibility; no-op if unchanged.
	 */
	void setHidden(bool hidden);

	/**
	 * @brief Stores both column-0 labels and re-applies the current mode's one.
	 *
	 * The labels are kept rather than only written, because setFrame() and
	 * setNoData() rewrite column 0 on every mode transition — writing a literal
	 * there would undo the translation the next time the mode flipped. Calling
	 * this before the first setFrame()/setNoData() (the panel does so from its
	 * constructor) is what seeds them.
	 *
	 * @param frameLabel  Translated "Frame" label.
	 * @param noDataLabel Translated "No profiling data yet" label.
	 */
	void retranslate(const QString& frameLabel, const QString& noDataLabel);

	/** @brief The wrapped tree item (for parenting child rows via ProfilingRow). */
	QTreeWidgetItem* item() const { return m_item; }

private:
	QTreeWidgetItem* m_item;

	enum class Mode { Uninitialized, NoData, Frame };
	Mode m_mode = Mode::Uninitialized;

	// Column-0 text for each mode, in the active language. Seeded by
	// retranslate(); the English fallbacks keep the row readable if a caller
	// ever reaches setFrame()/setNoData() first.
	QString m_frameLabel  = QStringLiteral("Frame");
	QString m_noDataLabel = QStringLiteral("No profiling data yet");

	// Centi-ms values use -1 as "not yet seeded"; draw/dispatch use UINT32_MAX.
	int      m_cpuCenti   = -1;
	int      m_gpuCenti   = -1;
	bool     m_gpuShown   = false;
	uint32_t m_draws      = UINT32_MAX;
	uint32_t m_dispatches = UINT32_MAX;
	bool     m_hidden     = false;
};

} // namespace neurus