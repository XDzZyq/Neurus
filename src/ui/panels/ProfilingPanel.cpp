#include "ProfilingPanel.h"

#include "UIContext.h"
#include "render/ProfilingData.h"
#include "ui/utils/I18n.h"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QStringList>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace neurus
{

// =========================================================================
// Constructor - tree widget with per-pass / frame-total columns
// =========================================================================

ProfilingPanel::ProfilingPanel(QWidget* parent)
	: UIPanel(PanelType::Profiling, "Profiling", parent)
{
	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(4, 4, 4, 4);

	m_tree = new QTreeWidget(this);
	m_tree->setColumnCount(5);
	m_tree->setHeaderLabels(QStringList{
		"Pass", "CPU (ms)", "GPU (ms)", "Draws", "Dispatches" });
	m_tree->setRootIsDecorated(true);
	m_tree->setUniformRowHeights(true);
	m_tree->setAlternatingRowColors(true);
	m_tree->setAllColumnsShowFocus(true);
	m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);

	// This is a read-only telemetry view: kill selection, focus, and hover
	// highlighting so the rows don't react to the mouse like an interactive list.
	m_tree->setSelectionMode(QAbstractItemView::NoSelection);
	m_tree->setFocusPolicy(Qt::NoFocus);
	m_tree->setMouseTracking(false);
	m_tree->viewport()->setAttribute(Qt::WA_Hover, false);

	layout->addWidget(m_tree);

	m_frameHead = std::make_unique<ProfilingHead>(m_tree);

	// Apply the active language (headers were built with English literals).
	Retranslate();
}

// =========================================================================
// Retranslate - re-apply column headers + head label
// =========================================================================

void ProfilingPanel::Retranslate()
{
	auto& i18n = I18n::instance();

	m_tree->setHeaderLabels(QStringList{
		i18n.translate("Pass"), i18n.translate("CPU (ms)"), i18n.translate("GPU (ms)"),
		i18n.translate("Draws"), i18n.translate("Dispatches") });
	m_frameHead->retranslate(i18n.translate("Frame"),
	                         i18n.translate("No profiling data yet"));
	m_tree->resizeColumnToContents(0);
}

// =========================================================================
// Refresh - copy latest profile, rebuild only on change
// =========================================================================

void ProfilingPanel::Refresh(const UIContext& ctx)
{
	const auto* profile = static_cast<const FrameProfile*>(ctx.profile);
	if (!profile)
	{
		// No renderer profile yet: hide every pooled row and reset detection.
		m_frameHead->setHidden(true);
		for (auto& row : m_rowPool)
			row->setHidden(true);
		m_hasProfile = false;
		m_frameCpuMsEma = -1.0;
		m_frameGpuMsEma = -1.0;
		return;
	}

	const bool changed = !m_hasProfile ||
		m_lastCpuMs != profile->cpuRecordMs ||
		m_lastGpuMs != profile->gpuTotalMs ||
		m_lastPassCount != profile->passCount;
	m_lastCpuMs = profile->cpuRecordMs;
	m_lastGpuMs = profile->gpuTotalMs;
	m_lastPassCount = profile->passCount;
	m_hasProfile = true;

	if (!changed)
		return;

	Populate(*profile);
}

// =========================================================================
// EnsureRowPool - grow the per-pass child-row pool (recycle, never destroy)
// =========================================================================

bool ProfilingPanel::EnsureRowPool(std::size_t needed)
{
	// Pool never shrinks: hide the surplus rows instead of destroying them so
	// they can be recycled if the pass count grows again.
	const bool resized = needed != m_rowPool.size();
	if (needed < m_rowPool.size())
	{
		for (std::size_t i = needed; i < m_rowPool.size(); ++i)
			m_rowPool[i]->setHidden(true);
		return resized;
	}
	while (m_rowPool.size() < needed)
		m_rowPool.push_back(std::make_unique<ProfilingRow>(m_frameHead->item()));

	return resized;
}

// =========================================================================
// Populate - Frame totals root row + one recycled child row per pass
// =========================================================================

void ProfilingPanel::Populate(const FrameProfile& profile)
{
	if (profile.passCount == 0)
	{
		m_frameHead->setNoData();
		for (auto& row : m_rowPool)
			row->setHidden(true);
		// Reset smoothing so timings reseed cleanly once data returns.
		m_frameCpuMsEma = -1.0;
		m_frameGpuMsEma = -1.0;
		m_passCpuMsEma.clear();
		m_passGpuMsEma.clear();
		return;
	}

	// GPU timestamps need device support AND a fence-signaled readback;
	// until then only CPU data is available.
	const bool gpuShown = profile.gpuTimingAvailable && profile.gpuReady;

	// Reset EMA state when the pass set changes so stale per-pass values don't
	// bleed across a pipeline change. -1.0 marks each slot as "not yet seeded".
	if (m_passCpuMsEma.size() != profile.passes.size())
	{
		m_passCpuMsEma.assign(profile.passes.size(), -1.0);
		m_passGpuMsEma.assign(profile.passes.size(), -1.0);
	}

	// EMA step: a negative prev means "not yet seeded", so the first sample is
	// taken verbatim instead of ramping up from zero (ms is always >= 0).
	const auto ema = [](double prev, double sample)
	{
		return prev < 0.0 ? sample : kEmaAlpha * sample + (1.0 - kEmaAlpha) * prev;
	};

	m_frameCpuMsEma = ema(m_frameCpuMsEma, profile.cpuRecordMs);
	if (gpuShown)
		m_frameGpuMsEma = ema(m_frameGpuMsEma, profile.gpuTotalMs);

	// Frame totals row - dirty-checked columns (skips setText when the
	// displayed value is unchanged, including sub-0.005ms EMA drift).
	m_frameHead->setFrame(m_frameCpuMsEma, m_frameGpuMsEma, gpuShown,
	                      profile.drawCalls, profile.dispatches);

	const bool resized = EnsureRowPool(profile.passes.size());
	for (std::size_t i = 0; i < profile.passes.size(); ++i)
	{
		const auto& pass = profile.passes[i];

		m_passCpuMsEma[i] = ema(m_passCpuMsEma[i], pass.cpuMs);
		if (gpuShown)
			m_passGpuMsEma[i] = ema(m_passGpuMsEma[i], pass.gpuMs);

		// Reveal the row (dirty-checked; free if already visible) - needed when
		// the pass count grows back after a shrink hid surplus rows.
		m_rowPool[i]->setHidden(false);
		// Per-column dirty check inside setPass skips setText for stable
		// columns (name/draws/dispatches) and for timing values whose EMA
		// drift rounds to the same displayed string.
		m_rowPool[i]->setPass(pass.name, m_passCpuMsEma[i], m_passGpuMsEma[i],
		                      gpuShown, pass.drawCalls, pass.dispatches);
	}

	if (resized) {
		// Fit content columns; keep numeric columns at a fixed readable width.
		m_tree->resizeColumnToContents(0);
		m_tree->resizeColumnToContents(3);
		m_tree->resizeColumnToContents(4);
	}
}

} // namespace neurus
