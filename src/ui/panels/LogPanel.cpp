#include "ui/panels/LogPanel.h"

#include <fstream>

#include <QComboBox>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QScrollBar>
#include <QStyle>
#include <QTextStream>
#include <QToolButton>
#include <QVBoxLayout>

#include "core/Log.h"
#include "ui/utils/I18n.h"

namespace neurus
{

// --- QSS loading (static one-shot, mirrors ShaderEditorPanel pattern) ---
namespace
{

QString LoadLogPanelStyle()
{
	static const QString s_style = []() {
		QFile file(QStringLiteral(":/qml/logpanel.qss"));
		if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
			return QString();
		QTextStream stream(&file);
		const QString css = stream.readAll();
		file.close();
		return css;
	}();
	return s_style;
}

} // namespace

// Status-bar counter format — one spelling shared by every update site.
// N_ registers it with the extractor without translating it here.
static constexpr const char* kStatsFormat = N_("INFO %1 · ERROR %2");

LogPanel::LogPanel(QWidget* parent)
	: UIPanel(PanelType::Log, "Log", parent)
{
	auto* root = new QVBoxLayout(this);
	root->setContentsMargins(4, 4, 4, 4);
	root->setSpacing(4);

	BuildToolbar();
	root->addLayout(m_toolbar);

	m_proxy.setSourceModel(&m_model);
	m_view.setModel(&m_proxy);
	m_view.setItemDelegate(&m_delegate);
	m_view.setObjectName(QStringLiteral("logView"));
	m_view.setUniformItemSizes(true);
	m_view.setSelectionMode(QAbstractItemView::NoSelection);
	m_view.setFocusPolicy(Qt::NoFocus);
	m_view.setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	root->addWidget(&m_view, 1);

	setStyleSheet(LoadLogPanelStyle());

	// Apply the active language (toolbar was built with English literals).
	Retranslate();
}

// =========================================================================
// Retranslate - re-apply toolbar texts in the active language
// =========================================================================

void LogPanel::Retranslate()
{
	auto& i18n = I18n::instance();

	// Filter combo: re-text items in place (indices preserved, so the
	// level-filter mapping in OnFilterChanged stays valid).
	m_filter->setItemText(0, i18n.translate("All"));
	m_filter->setItemText(1, i18n.translate("Info"));
	m_filter->setItemText(2, i18n.translate("Errors"));

	m_search->setPlaceholderText(i18n.translateCtx("Search...", "Placeholder"));
	m_stats->setText(i18n.translate(kStatsFormat)
	                     .arg(m_lastInfoCount).arg(m_lastErrorCount));
	m_autoScrollBtn->setText(i18n.translate("Auto-scroll"));
	m_pauseBtn->setText(i18n.translate("Pause"));
	m_clearBtn->setText(i18n.translate("Clear"));
	m_exportBtn->setText(i18n.translate("Export..."));
}

void LogPanel::BuildToolbar()
{
	m_toolbar = new QHBoxLayout();
	m_toolbar->setContentsMargins(0, 0, 0, 0);
	m_toolbar->setSpacing(4);

	m_filter = new QComboBox(this);
	m_filter->addItems({QStringLiteral("All"), QStringLiteral("Info"),
	                    QStringLiteral("Errors")});
	m_filter->setCurrentIndex(0);
	m_filter->setObjectName(QStringLiteral("logToolBtn"));
	m_toolbar->addWidget(m_filter);

	// Search bar: a bordered container holding the search field (stretch)
	// and the live INFO/ERROR counts as a right-side suffix, so the stats
	// read as part of the search control rather than a separate header.
	m_searchWrap = new QWidget(this);
	m_searchWrap->setObjectName(QStringLiteral("logSearchWrap"));
	m_searchWrap->setAttribute(Qt::WA_StyledBackground, true);

	auto* searchLayout = new QHBoxLayout(m_searchWrap);
	searchLayout->setContentsMargins(6, 0, 8, 0);
	searchLayout->setSpacing(6);

	m_search = new QLineEdit(m_searchWrap);
	m_search->setPlaceholderText(QStringLiteral("Search..."));
	m_search->setClearButtonEnabled(true);
	m_search->setObjectName(QStringLiteral("logSearch"));
	m_search->installEventFilter(this);
	searchLayout->addWidget(m_search, 1);

	m_stats = new QLabel(QStringLiteral("INFO 0 · ERROR 0"), m_searchWrap);
	m_stats->setObjectName(QStringLiteral("logStats"));
	m_stats->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
	searchLayout->addWidget(m_stats);

	m_toolbar->addWidget(m_searchWrap, 1);

	m_autoScrollBtn = new QToolButton(this);
	m_autoScrollBtn->setText(QStringLiteral("Auto-scroll"));
	m_autoScrollBtn->setCheckable(true);
	m_autoScrollBtn->setChecked(true);
	m_autoScrollBtn->setObjectName(QStringLiteral("logToolBtn"));
	m_toolbar->addWidget(m_autoScrollBtn);

	m_pauseBtn = new QToolButton(this);
	m_pauseBtn->setText(QStringLiteral("Pause"));
	m_pauseBtn->setCheckable(true);
	m_pauseBtn->setChecked(false);
	m_pauseBtn->setObjectName(QStringLiteral("logToolBtn"));
	m_toolbar->addWidget(m_pauseBtn);

	m_clearBtn = new QToolButton(this);
	m_clearBtn->setText(QStringLiteral("Clear"));
	m_clearBtn->setObjectName(QStringLiteral("logToolBtn"));
	m_toolbar->addWidget(m_clearBtn);

	m_exportBtn = new QToolButton(this);
	m_exportBtn->setText(QStringLiteral("Export..."));
	m_exportBtn->setObjectName(QStringLiteral("logToolBtn"));
	m_toolbar->addWidget(m_exportBtn);

	connect(m_filter, &QComboBox::currentIndexChanged,
	        this, &LogPanel::OnFilterChanged);
	connect(m_search, &QLineEdit::textChanged,
	        this, &LogPanel::OnSearchChanged);
	connect(m_autoScrollBtn, &QToolButton::toggled,
	        this, &LogPanel::OnAutoScrollToggled);
	connect(m_pauseBtn, &QToolButton::toggled,
	        this, &LogPanel::OnPauseToggled);
	connect(m_clearBtn, &QToolButton::clicked,
	        this, &LogPanel::OnClearClicked);
	connect(m_exportBtn, &QToolButton::clicked,
	        this, &LogPanel::OnExportClicked);
}

void LogPanel::Refresh(const UIContext& ctx)
{
	const auto* buffer = static_cast<const LogBuffer*>(ctx.log);
	if (!buffer)
		return;

	// Auto-scroll only re-pins when the user is already at the bottom;
	// if they scrolled up, don't yank them back down every frame.
	const bool wasAtBottom = m_view.verticalScrollBar()->value() >= m_view.verticalScrollBar()->maximum();

	if (!m_paused)
	{
		m_model.Refresh(buffer);
		m_delegate.setSourcePad(m_model.MaxSourceChars());
	}

	const std::size_t info = buffer->InfoCount();
	const std::size_t errors = buffer->ErrorCount();

	// Error notifier: fire only when the count grew (throttled to one
	// signal per Refresh window; repeated errors accumulate in delta).
	if (errors > m_lastErrorCount)
	{
		const std::size_t delta = errors - m_lastErrorCount;
		QString firstMsg;
		const std::size_t size = buffer->Size();
		const std::size_t limit = (delta < size) ? delta : size;
		for (std::size_t k = 0; k < limit; ++k)
		{
			const LogEntry e = buffer->At(size - 1 - k);
			if (e.level == LogLevel::Error)
			{
				firstMsg = QString::fromStdString(e.message);
				break;
			}
		}
		emit errorNotified(static_cast<int>(delta), firstMsg);
	}

	// Stats suffix inside the search bar (dirty-checked).
	if (info != m_lastInfoCount || errors != m_lastErrorCount)
	{
		m_lastInfoCount = info;
		m_lastErrorCount = errors;
		m_stats->setText(I18n::instance().translate(kStatsFormat)
		                     .arg(info).arg(errors));
	}

	if (m_autoScrollBtn->isChecked() && wasAtBottom)
		MaybeScrollToBottom();
}

void LogPanel::MaybeScrollToBottom()
{
	if (!m_autoScrollBtn->isChecked())
		return;
	auto* sb = m_view.verticalScrollBar();
	sb->setValue(sb->maximum());
}

void LogPanel::OnFilterChanged(int index)
{
	switch (index)
	{
	case 1: m_proxy.setLevelFilter(LogFilterProxy::LevelFilter::InfoOnly); break;
	case 2: m_proxy.setLevelFilter(LogFilterProxy::LevelFilter::ErrorsOnly); break;
	default: m_proxy.setLevelFilter(LogFilterProxy::LevelFilter::All); break;
	}
}

void LogPanel::OnSearchChanged(const QString& text)
{
	m_proxy.setSearchText(text);
}

void LogPanel::OnAutoScrollToggled(bool checked)
{
	Q_UNUSED(checked);
	MaybeScrollToBottom();
}

void LogPanel::OnPauseToggled(bool checked)
{
	m_paused = checked;
	if (!m_paused)
	{
		m_model.Refresh(&LogBuffer::instance());
		m_delegate.setSourcePad(m_model.MaxSourceChars());
	}
}

void LogPanel::OnClearClicked()
{
	LogBuffer::instance().Clear();
	m_lastErrorCount = 0;
	m_lastInfoCount = 0;
	m_model.Refresh(&LogBuffer::instance());
	m_delegate.setSourcePad(m_model.MaxSourceChars());
	m_stats->setText(I18n::instance().translate(kStatsFormat)
	                     .arg(0).arg(0));
}

bool LogPanel::eventFilter(QObject* watched, QEvent* event)
{
	// Mirror the search field's focus onto the surrounding search bar so
	// the whole control shows a single focus ring (QSS dynamic property).
	if (watched == m_search
	    && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut))
	{
		const bool focused = (event->type() == QEvent::FocusIn);
		m_searchWrap->setProperty("focusState", focused ? QStringLiteral("true")
		                                               : QStringLiteral("false"));
		m_searchWrap->style()->unpolish(m_searchWrap);
		m_searchWrap->style()->polish(m_searchWrap);
	}
	return QWidget::eventFilter(watched, event);
}

void LogPanel::OnExportClicked()
{
	const QString path = QFileDialog::getSaveFileName(
	    this, I18n::instance().translateCtx("Save Log", "Dialog"), QStringLiteral("neurus.log"),
	    I18n::instance().translateCtx("Log Files (*.log)", "Dialog"));
	if (path.isEmpty())
		return;

	std::ofstream file(path.toStdString(), std::ios::binary);
	if (!file.is_open())
	{
		QMessageBox::warning(this, I18n::instance().translateCtx("Save Log", "Dialog"),
		                     I18n::instance().translateCtx("Failed to open file for writing.", "Dialog"));
		return;
	}

	const LogBuffer& buf = LogBuffer::instance();
	const std::size_t size = buf.Size();
	for (std::size_t i = 0; i < size; ++i)
	{
		const LogEntry e = buf.At(i);
		file << FormatLogTimestamp(e.timestamp).toStdString()
		     << " " << (e.level == LogLevel::Error ? "ERROR" : "INFO")
		     << " [" << e.func << ":" << e.line << "] "
		     << e.message << "\n";
	}
	file.close();
	if (!file.good())
	{
		QMessageBox::warning(this, I18n::instance().translateCtx("Save Log", "Dialog"),
		                     I18n::instance().translateCtx("Error writing log file.", "Dialog"));
		return;
	}
}

} // namespace neurus
