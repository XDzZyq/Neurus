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

#include "editor/events/UIEvents.h"

using namespace neurus;

TEST(UIEventsScript, RequestRunScriptEmitsCode)
{
	UIEvents& ui = UIEvents::instance();

	QString received;
	auto conn = QObject::connect(&ui, &UIEvents::runScriptRequested,
	                             [&received](const QString& code) { received = code; });

	ui.requestRunScript(QStringLiteral("x = 1"));
	QObject::disconnect(conn);

	EXPECT_EQ(received, QStringLiteral("x = 1"));
}

TEST(UIEventsScript, RequestRunScriptFileEmitsPath)
{
	UIEvents& ui = UIEvents::instance();

	QString received;
	auto conn = QObject::connect(&ui, &UIEvents::runScriptFileRequested,
	                             [&received](const QString& path) { received = path; });

	ui.requestRunScriptFile(QStringLiteral("scripts/demo.py"));
	QObject::disconnect(conn);

	EXPECT_EQ(received, QStringLiteral("scripts/demo.py"));
}
