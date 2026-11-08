/**
 * @file test_run_script_menu.cpp
 * @brief The Tools menu exposes a "Run Script" action.
 *
 * Its trigger is exercised manually (it opens a file dialog and requests a run
 * over UIEvents); this pins that the action exists and is translated.
 */

#include <gtest/gtest.h>

#include <memory>

#include <QAction>
#include <QString>

#include "ui/UIManager.h"

using namespace neurus;

TEST(RunScriptMenu, ToolsMenuHasRunScriptAction)
{
	auto win = std::make_unique<UIManager>(QStringLiteral("en"), 60, QString());

	auto* action = win->findChild<QAction*>(QStringLiteral("runScriptAction"));
	ASSERT_NE(action, nullptr);
	EXPECT_FALSE(action->text().isEmpty());
}
