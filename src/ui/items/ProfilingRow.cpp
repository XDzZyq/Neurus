/**
 * @file ProfilingRow.cpp
 * @brief ProfilingRow + ProfilingHead implementations - dirty-checked lazy
 *        column updates for the Profiling panel.
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
	// Column 0 - pass name. Skip the fromStdString + setText when unchanged.
	if (m_name != name)
	{
		m_name = name;
		m_item->setText(0, QString::fromStdString(name));
	}

	// Column 1 - CPU ms, rounded to centi-ms so a sub-0.005ms EMA drift does
	// not trigger a write.
	const int cpuCenti = static_cast<int>(std::round(cpuMs * 100.0));
	if (m_cpuCenti != cpuCenti)
	{
		m_cpuCenti = cpuCenti;
		m_item->setText(1, QString::number(cpuMs, 'f', 2));
	}

	// Column 2 - GPU ms, or "--" when timings aren't ready. The shown/hidden
	// transition is tracked separately from the numeric dirty check.
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
	// Column 0 - label. Only written on a mode transition, and from the cached
	// label, so a NoData->Frame flip after a language switch does not
	// re-English the row.
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
	// no write: setFrame()/setNoData() pick the label up from the members.
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