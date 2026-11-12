/**
 * @file test_script_console_panel.cpp
 * @brief Smoke tests for the Script Console dock panel.
 *
 * The panel is driven by `UIContext::scriptConsole` and emits run requests;
 * these tests cover the drain/cursor logic and the Ctrl+Enter run gesture
 * without an engine.
 */

#include <gtest/gtest.h>

#include <QtTest/QTest>

#include <QString>

#include "script/ScriptConsole.h"
#include "ui/UIContext.h"
#include "ui/items/CodeEditor.h"
#include "ui/panels/ScriptConsolePanel.h"

using namespace neurus;

TEST(ScriptConsolePanelTest, RefreshWithDefaultContextDoesNotCrash)
{
	ScriptConsolePanel panel;
	UIContext ctx;  // every pointer null
	EXPECT_NO_THROW(panel.Refresh(ctx));
}

TEST(ScriptConsolePanelTest, RefreshDrainsConsoleBufferIdempotently)
{
	ScriptConsolePanel panel;
	ScriptConsoleBuffer buffer;
	buffer.Append(ScriptLineKind::Output, "hello\n");
	buffer.Append(ScriptLineKind::Error, "boom\n");

	UIContext ctx;
	ctx.scriptConsole = &buffer;
	EXPECT_NO_THROW(panel.Refresh(ctx));
	EXPECT_NO_THROW(panel.Refresh(ctx));
}

TEST(ScriptConsolePanelTest, CtrlEnterEmitsRunSnippetWithInput)
{
	ScriptConsolePanel panel;
	auto* input = panel.findChild<CodeEditor*>(QStringLiteral("scriptConsoleInput"));
	ASSERT_NE(input, nullptr);
	input->setCode("x = 1");

	QString received;
	QObject::connect(&panel, &ScriptConsolePanel::runSnippetRequested,
	                 [&received](const QString& code) { received = code; });

	QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
	EXPECT_EQ(received, QStringLiteral("x = 1"));
}
