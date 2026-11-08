#include "ui/panels/ScriptConsolePanel.h"

#include <QEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QToolButton>
#include <QVBoxLayout>

#include "script/ScriptConsole.h"
#include "ui/items/CodeEditor.h"
#include "ui/utils/I18n.h"

namespace neurus
{

ScriptConsolePanel::ScriptConsolePanel(QWidget* parent)
	: UIPanel(PanelType::ScriptConsole, "Script Console", parent)
{
	auto* root = new QVBoxLayout(this);
	root->setContentsMargins(4, 4, 4, 4);
	root->setSpacing(4);

	auto* toolbar = new QHBoxLayout();
	toolbar->setContentsMargins(0, 0, 0, 0);
	toolbar->setSpacing(4);

	m_runBtn = new QToolButton(this);
	m_runFileBtn = new QToolButton(this);
	m_clearBtn = new QToolButton(this);
	toolbar->addWidget(m_runBtn);
	toolbar->addWidget(m_runFileBtn);
	toolbar->addStretch(1);
	toolbar->addWidget(m_clearBtn);
	root->addLayout(toolbar);

	m_output = new CodeEditor(this);
	m_output->setReadOnly(true);
	m_output->setLanguage(CodeEditor::Language::Python);
	m_output->setLineNumbersVisible(false);
	m_output->setObjectName(QStringLiteral("scriptConsoleOutput"));
	root->addWidget(m_output, 1);

	m_input = new CodeEditor(this);
	m_input->setLanguage(CodeEditor::Language::Python);
	m_input->setObjectName(QStringLiteral("scriptConsoleInput"));
	m_input->setMinimumHeight(72);
	m_input->setMaximumHeight(180);
	m_input->installEventFilter(this);
	root->addWidget(m_input, 0);

	connect(m_runBtn, &QToolButton::clicked, this, &ScriptConsolePanel::OnRunClicked);
	connect(m_runFileBtn, &QToolButton::clicked, this, &ScriptConsolePanel::OnRunFileClicked);
	connect(m_clearBtn, &QToolButton::clicked, this, &ScriptConsolePanel::OnClearClicked);

	Retranslate();
}

void ScriptConsolePanel::Refresh(const neurus::UIContext& ctx)
{
	if (ctx.scriptConsole == nullptr)
	{
		return;
	}

	const ScriptConsoleBuffer& buffer = *ctx.scriptConsole;
	const std::size_t size = buffer.Size();
	const uint64_t newest = size == 0 ? 0 : buffer.At(size - 1).seq;

	// Nothing new since the last render (and not an empty-buffer reset).
	if (m_hasCursor && size != 0 && newest == m_lastSeq)
	{
		return;
	}

	// Rebuild from the buffer's current window. The ring evicts old lines, so
	// this is wrap-safe (a count-based cursor would freeze once the buffer is
	// full) and clear-safe, at the cost of one bounded redraw per change.
	m_output->clear();
	for (std::size_t i = 0; i < size; ++i)
	{
		const ScriptLine line = buffer.At(i);
		if (line.seq > m_clearSeq)
		{
			AppendOutputLine(QString::fromStdString(line.text));
		}
	}

	m_lastSeq = newest;
	m_hasCursor = size != 0;
}

void ScriptConsolePanel::Retranslate()
{
	auto& i18n = I18n::instance();
	m_runBtn->setText(i18n.translate("Run"));
	m_runBtn->setToolTip(i18n.translateCtx("Run the snippet (Ctrl+Enter)", "Tooltip"));
	m_runFileBtn->setText(i18n.translate("Run File..."));
	m_clearBtn->setText(i18n.translate("Clear"));
	m_input->setPlaceholderText(i18n.translateCtx("Python snippet", "Placeholder"));
}

bool ScriptConsolePanel::eventFilter(QObject* watched, QEvent* event)
{
	if (watched == m_input && event->type() == QEvent::KeyPress)
	{
		auto* key = static_cast<QKeyEvent*>(event);
		const bool isEnter = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
		if (isEnter && key->modifiers().testFlag(Qt::ControlModifier))
		{
			OnRunClicked();
			return true;
		}
	}
	return UIPanel::eventFilter(watched, event);
}

void ScriptConsolePanel::OnRunClicked()
{
	const QString code = QString::fromStdString(m_input->getCode());
	if (code.trimmed().isEmpty())
	{
		return;
	}
	emit runSnippetRequested(code);
}

void ScriptConsolePanel::OnRunFileClicked()
{
	const QString path = QFileDialog::getOpenFileName(
		this, I18n::instance().translate("Run Script"), QString(),
		QStringLiteral("Python scripts (*.py);;All files (*)"));
	if (!path.isEmpty())
	{
		emit runFileRequested(path);
	}
}

void ScriptConsolePanel::OnClearClicked()
{
	// Hide everything shown so far; later lines (higher seq) still appear. The
	// engine buffer keeps its history.
	m_output->clear();
	m_clearSeq = m_lastSeq;
}

void ScriptConsolePanel::AppendOutputLine(const QString& text)
{
	QString line = text;
	while (line.endsWith(QLatin1Char('\n')) || line.endsWith(QLatin1Char('\r')))
	{
		line.chop(1);
	}
	// A bare newline (Python's `print` writes text and its terminator
	// separately) must not produce a blank row.
	if (line.isEmpty())
	{
		return;
	}
	m_output->appendPlainText(line);
}

}  // namespace neurus
