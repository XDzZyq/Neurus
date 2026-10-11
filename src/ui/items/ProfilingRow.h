/**
 * @file ProfilingRow.h
 * @brief Dirty-checked QTreeWidgetItem wrappers for the Profiling panel.
 *
 * ProfilingRow is one per-pass child row; ProfilingHead is the bold top-level
 * "Frame" totals row. Both cache the last displayed value of each column and
 * call QTreeWidgetItem::setText only when the visible value changed.
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
	 */
	explicit ProfilingRow(QTreeWidgetItem* parent);

	~ProfilingRow() = default;

	ProfilingRow(const ProfilingRow&) = delete;
	ProfilingRow& operator=(const ProfilingRow&) = delete;

	/**
	 * @brief Updates all five columns, dirty-checking each independently.
	 *
	 * Timing columns compare on centi-ms resolution, so a sub-0.005ms EMA
	 * drift does not trigger a write.
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
	// Centi-ms uses -1 as the "not yet seeded" sentinel (ms is always >= 0);
	// draw/dispatch counts use UINT32_MAX.
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
 */
class ProfilingHead
{
public:
	/**
	 * @brief Constructs the head row as a top-level item of @p tree.
	 *
	 * Applies a bold font to column 0 and starts visible + expanded.
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
	 * Timing columns compare on centi-ms resolution, so a sub-0.005ms EMA
	 * drift does not trigger a write.
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
	 * Sets col 0 to the cached placeholder text and clears cols 1-4. No-op if
	 * already in NoData mode, so it is safe to call every frame.
	 */
	void setNoData();

	/**
	 * @brief Toggles row visibility; no-op if unchanged.
	 */
	void setHidden(bool hidden);

	/**
	 * @brief Stores both column-0 labels and re-applies the current mode's one.
	 *
	 * The labels are kept rather than only written because setFrame() and
	 * setNoData() rewrite column 0 on every mode transition; keeping them lets
	 * a language switch survive the next flip.
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
	// retranslate(); the English fallbacks keep the row readable until then.
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