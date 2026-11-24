/**
 * @file test_ui_events_script.cpp
 * @brief The Script Console run requests travel over the UIEvents bus.
 *
 * The console panel emits its own signals (connected directly by the
 * Application); the UIEvents forms let other UI (the Tools menu) trigger a run
 * without holding the panel. This pins the bus side of that contract.
 */

#include <gtest/gtest.h>

#include <QString>

#include "ui/events/UIEvents.hpp"

using namespace neurus;

TEST(UIEventsScript, RunRequestsRouteSnippetsAndFilePaths)
{
	auto& ui = UIEvents::instance();
	QObject receiver;
	QString snippet, path;
	QObject::connect(&ui, &UIEvents::runScriptRequested, &receiver, [&](const QString& value) { snippet = value; });
	QObject::connect(&ui, &UIEvents::runScriptFileRequested, &receiver, [&](const QString& value) { path = value; });
	ui.requestRunScript(QStringLiteral("x = 1"));
	EXPECT_TRUE(path.isEmpty());
	ui.requestRunScriptFile(QStringLiteral("scripts/demo.py"));
	EXPECT_EQ(snippet, QStringLiteral("x = 1"));
	EXPECT_EQ(path, QStringLiteral("scripts/demo.py"));
}
