/**
 * @file ScriptConsolePanel.h
 * @brief Dock panel that runs Python snippets and a script file.
 *
 * Mirrors LogPanel's poll model: the panel never calls the editor, it drains
 * `UIContext::scriptConsole` in `Refresh()` and emits run requests that the
 * Application forwards to the ScriptEngine. Output lines are appended as the
 * engine's console buffer grows.
 */

#pragma once

#include <cstddef>

#include <QString>

#include "ui/panels/UIPanel.h"

class QEvent;
class QPlainTextEdit;
class QToolButton;

namespace neurus
{

class CodeEditor;

/// @brief Script Console dock: a Python snippet input, an output view, and
///        Run / Run File / Clear actions.
class ScriptConsolePanel : public UIPanel
{
	Q_OBJECT

public:
	static constexpr PanelType kType = PanelType::ScriptConsole;

	explicit ScriptConsolePanel(QWidget* parent = nullptr);

	/// @brief Drains newly-appended console lines into the output view.
	void Refresh(const neurus::UIContext& ctx) override;

	/// @brief Re-applies button/tooltip/placeholder texts in the active language.
	void Retranslate() override;

	bool eventFilter(QObject* watched, QEvent* event) override;

signals:
	/// @brief Emitted when the user runs the current input as a snippet.
	void runSnippetRequested(const QString& code);

	/// @brief Emitted when the user picks a script file to run.
	void runFileRequested(const QString& path);

private:
	void OnRunClicked();
	void OnRunFileClicked();
	void OnClearClicked();
	void AppendOutputLine(const QString& text);

	QToolButton*    m_runBtn = nullptr;
	QToolButton*    m_runFileBtn = nullptr;
	QToolButton*    m_clearBtn = nullptr;
	QPlainTextEdit* m_output = nullptr;
	CodeEditor*     m_input = nullptr;

	std::size_t m_lastLineCount = 0;  ///< Console lines already shown.
};

}  // namespace neurus
