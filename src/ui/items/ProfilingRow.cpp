/**
 * @file ProfilingRow.cpp
 * @brief ProfilingRow + ProfilingHead implementations - dirty-checked lazy
 *        column updates for the Profiling panel.
 *
 * Each column caches the last *displayed* value and only invokes
 * QTreeWidgetItem::setText when that displayed value changes. The timing
 * columns compare on centi-ms (2-decimal) resolution so a sub-0.005ms EMA
 * drift - which rounds to the same "X.XX" string - skips both the QString
 * allocation and the QTreeModel dataChanged notification.
 */

#include "items/ProfilingRow.h"

#include <QFont>
#include <QString>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <cmath>

namespace neurus
{

// =========================================================================
// ProfilingRow - constructor (hidden child item)
// =========================================================================

ProfilingRow::ProfilingRow(QTreeWidgetItem* parent)
	: m_item(new QTreeWidgetItem(parent))
{
	m_item->setHidden(true);
}

// =========================================================================
// ProfilingRow::setPass - per-column dirty check; only setText on change
// =========================================================================

void ProfilingRow::setPass(const std::string& name, double cpuMs, double gpuMs,
                           bool gpuShown, uint32_t draws, uint32_t dispatches)
{
	// Column 0 - pass name. Stable across frames; skip the fromStdString
	// allocation + setText when the name is unchanged.
	if (m_name != name)
	{
		m_name = name;
		m_item->setText(0, QString::fromStdString(name));
	}

	// Column 1 - CPU ms. Round to centi-ms so sub-0.005ms EMA drift does not
	// trigger a write. Same centi value guarantees the same 2-decimal QString.
	const int cpuCenti = static_cast<int>(std::round(cpuMs * 100.0));
	if (m_cpuCenti != cpuCenti)
	{
		m_cpuCenti = cpuCenti;
		m_item->setText(1, QString::number(cpuMs, 'f', 2));
	}

	// Column 2 - GPU ms, or "--" when timings aren't ready. Track the
	// shown/hidden transition separately from the numeric dirty check.
	if (!gpuShown)
	{
		if (m_gpuShown)
		{
			m_gpuShown = false;
			m_item->setText(2, QStringLiteral("--"));
		}
	}
	else
	{
		const int gpuCenti = static_cast<int>(std::round(gpuMs * 100.0));
		if (!m_gpuShown || m_gpuCenti != gpuCenti)
		{
			m_gpuShown = true;
			m_gpuCenti = gpuCenti;
			m_item->setText(2, QString::number(gpuMs, 'f', 2));
		}
	}

	// Column 3 - draw calls. Stable across frames; skip when unchanged.
	if (m_draws != draws)
	{
		m_draws = draws;
		m_item->setText(3, QString::number(draws));
	}

	// Column 4 - dispatches. Stable across frames; skip when unchanged.
	if (m_dispatches != dispatches)
	{
		m_dispatches = dispatches;
		m_item->setText(4, QString::number(dispatches));
	}
}

// =========================================================================
// ProfilingRow::setHidden - visibility dirty check
// =========================================================================

void ProfilingRow::setHidden(bool hidden)
{
	if (m_hidden == hidden)
		return;
	m_hidden = hidden;
	m_item->setHidden(hidden);
}

// =========================================================================
// ProfilingHead - constructor (top-level item, bold font, visible + expanded)
// =========================================================================

ProfilingHead::ProfilingHead(QTreeWidget* tree)
	: m_item(new QTreeWidgetItem(tree))
{
	QFont boldFont = m_item->font(0);
	boldFont.setBold(true);
	m_item->setFont(0, boldFont);

	m_item->setHidden(false);
	m_item->setExpanded(true);
}

// =========================================================================
// ProfilingHead::setFrame - "Frame" label + dirty-checked numeric columns
// =========================================================================

void ProfilingHead::setFrame(double cpuMs, double gpuMs, bool gpuShown,
                              uint32_t draws, uint32_t dispatches)
{
	// Column 0 - label. Only write on a mode transition, and write the cached
	// translated label: a literal here would re-English the row on every
	// NoData->Frame flip that happened after a language switch.
	if (m_mode != Mode::Frame)
	{
		m_mode = Mode::Frame;
		m_item->setText(0, m_frameLabel);
	}

	// Column 1 - CPU ms (centi-ms dirty check).
	const int cpuCenti = static_cast<int>(std::round(cpuMs * 100.0));
	if (m_cpuCenti != cpuCenti)
	{
		m_cpuCenti = cpuCenti;
		m_item->setText(1, QString::number(cpuMs, 'f', 2));
	}

	// Column 2 - GPU ms, or "--" when timings aren't ready.
	if (!gpuShown)
	{
		if (m_gpuShown)
		{
			m_gpuShown = false;
			m_item->setText(2, QStringLiteral("--"));
		}
	}
	else
	{
		const int gpuCenti = static_cast<int>(std::round(gpuMs * 100.0));
		if (!m_gpuShown || m_gpuCenti != gpuCenti)
		{
			m_gpuShown = true;
			m_gpuCenti = gpuCenti;
			m_item->setText(2, QString::number(gpuMs, 'f', 2));
		}
	}

	// Column 3 - draw calls.
	if (m_draws != draws)
	{
		m_draws = draws;
		m_item->setText(3, QString::number(draws));
	}

	// Column 4 - dispatches.
	if (m_dispatches != dispatches)
	{
		m_dispatches = dispatches;
		m_item->setText(4, QString::number(dispatches));
	}
}

// =========================================================================
// ProfilingHead::setNoData - "No profiling data yet" + clear numeric columns
// =========================================================================

void ProfilingHead::retranslate(const QString& frameLabel,
                                   const QString& noDataLabel)
{
	m_frameLabel  = frameLabel;
	m_noDataLabel = noDataLabel;

	// Re-text col 0 for the current mode without touching the cached numeric
	// state (which stays valid across language switches). Uninitialized needs
	// no write: setFrame()/setNoData() will pick the label up from the members.
	switch (m_mode)
	{
	case Mode::Frame:  m_item->setText(0, m_frameLabel);  break;
	case Mode::NoData: m_item->setText(0, m_noDataLabel); break;
	default:           break;
	}
}

void ProfilingHead::setNoData()
{
	if (m_mode == Mode::NoData)
		return;
	m_mode = Mode::NoData;

	m_item->setText(0, m_noDataLabel);
	for (int col = 1; col < 5; ++col)
		m_item->setText(col, QString());

	// Reset cached state so the next setFrame() writes every column.
	m_cpuCenti   = -1;
	m_gpuCenti   = -1;
	m_gpuShown   = false;
	m_draws      = UINT32_MAX;
	m_dispatches = UINT32_MAX;
}

// =========================================================================
// ProfilingHead::setHidden - visibility dirty check
// =========================================================================

void ProfilingHead::setHidden(bool hidden)
{
	if (m_hidden == hidden)
		return;
	m_hidden = hidden;
	m_item->setHidden(hidden);
}

} // namespace neurus