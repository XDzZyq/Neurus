/**
 * @file LogPanel.h
 * @brief Dock panel that browses realtime log output.
 *
 * Reads the core LogBuffer via UIContext::log on each Refresh() (poll-on-
 * refresh, matching the ProfilingPanel pattern). Provides live per-type
 * counts, an all/info/errors filter + text search, auto-scroll/pause
 * toggles, clear, and *.log export. Emits errorNotified() when new
 * NEURUS_ERR entries appear; UIManager routes it to the status bar.
 */

#pragma once

#include <QListView>
#include <cstddef>

#include "ui/delegates/LogDelegate.h"
#include "ui/models/LogFilterProxy.h"
#include "ui/models/LogModel.h"
#include "ui/panels/UIPanel.h"

class QComboBox;
class QEvent;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QToolButton;
class QWidget;

namespace neurus
{

/**
 * @brief Realtime log viewer dock.
 * @note Reads core LogBuffer only - no renderer coupling.
 */
class LogPanel : public UIPanel
{
	Q_OBJECT

public:
	static constexpr PanelType kType = PanelType::Log;

	explicit LogPanel(QWidget* parent = nullptr);

	void Refresh(const UIContext& ctx) override;

	/** @brief Re-applies toolbar texts / filter items in the active language. */
	void Retranslate() override;

	bool eventFilter(QObject* watched, QEvent* event) override;

signals:
	/**
	 * @brief Emitted when new Error entries are appended.
	 * @param delta Number of new errors since last Refresh().
	 * @param firstMessage Message of the most recent new error (for preview).
	 */
	void errorNotified(int delta, const QString& firstMessage);

private:
	void BuildToolbar();
	void MaybeScrollToBottom();

	void OnFilterChanged(int index);
	void OnSearchChanged(const QString& text);
	void OnAutoScrollToggled(bool checked);
	void OnPauseToggled(bool checked);
	void OnClearClicked();
	void OnExportClicked();

	QWidget*     m_searchWrap = nullptr;
	QLabel*      m_stats = nullptr;
	QComboBox*   m_filter = nullptr;
	QLineEdit*   m_search = nullptr;
	QToolButton* m_autoScrollBtn = nullptr;
	QToolButton* m_pauseBtn = nullptr;
	QToolButton* m_clearBtn = nullptr;
	QToolButton* m_exportBtn = nullptr;
	QHBoxLayout* m_toolbar = nullptr;

	LogModel       m_model;
	LogFilterProxy m_proxy;
	LogDelegate    m_delegate;
	QListView      m_view;

	bool        m_paused = false;
	std::size_t m_lastErrorCount = 0;
	std::size_t m_lastInfoCount = 0;
};

} // namespace neurus
