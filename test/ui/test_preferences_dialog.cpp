/**
 * @file test_preferences_dialog.cpp
 * @brief Script Sandbox and Script Preload rows in the Preferences dialog.
 */

#include <gtest/gtest.h>

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include <QCheckBox>
#include <QLineEdit>
#include <QSpinBox>

#include "ui/panels/PreferencesDialog.h"

using namespace neurus;

TEST(PreferencesDialogTest, SeedsScriptFieldsAndReadsThemBack)
{
	ScriptPreferencesUi script;
	script.readRoots = QStringLiteral("D:/project; D:/shared");
	script.writeRoots = QStringLiteral("D:/project/out");
	script.blockedModules = QStringLiteral("subprocess; ctypes");
	script.preload = QStringLiteral("numpy, json");
	script.allowNetwork = true;
	script.allowSubprocess = true;
	script.allowCtypes = true;
	script.timeLimitSeconds = 12;
	script.memoryLimitMb = 512;

	PreferencesDialog dialog(QStringLiteral("en"), 60, script,
	                         QStringLiteral("preferences.json"));
	const ScriptPreferencesUi shown = dialog.ScriptPreferences();
	EXPECT_EQ(shown.readRoots, script.readRoots);
	EXPECT_EQ(shown.writeRoots, script.writeRoots);
	EXPECT_EQ(shown.blockedModules, script.blockedModules);
	EXPECT_EQ(shown.preload, script.preload);
	EXPECT_TRUE(shown.allowNetwork);
	EXPECT_TRUE(shown.allowSubprocess);
	EXPECT_TRUE(shown.allowCtypes);
	EXPECT_EQ(shown.timeLimitSeconds, 12);
	EXPECT_EQ(shown.memoryLimitMb, 512);
}

TEST(PreferencesDialogTest, EditingAScriptFieldEmitsAChange)
{
	PreferencesDialog dialog(QStringLiteral("en"), 60, ScriptPreferencesUi{},
	                         QStringLiteral("preferences.json"));
	QSignalSpy spy(&dialog, &PreferencesDialog::scriptPreferencesChanged);

	auto* readRoots = dialog.findChild<QLineEdit*>(QStringLiteral("scriptReadRoots"));
	ASSERT_NE(readRoots, nullptr);
	readRoots->setText(QStringLiteral("D:/project"));
	EXPECT_EQ(dialog.ScriptPreferences().readRoots, QStringLiteral("D:/project"));

	auto* network = dialog.findChild<QCheckBox*>(QStringLiteral("scriptAllowNetwork"));
	ASSERT_NE(network, nullptr);
	network->setChecked(true);
	EXPECT_EQ(spy.count(), 1);
	EXPECT_TRUE(dialog.ScriptPreferences().allowNetwork);

	auto* timeLimit = dialog.findChild<QSpinBox*>(QStringLiteral("scriptTimeLimit"));
	ASSERT_NE(timeLimit, nullptr);
	timeLimit->setValue(5);
	EXPECT_EQ(spy.count(), 2);
	EXPECT_EQ(dialog.ScriptPreferences().timeLimitSeconds, 5);
}
