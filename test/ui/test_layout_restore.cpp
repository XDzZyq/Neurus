/**
 * @file test_layout_restore.cpp
 * @brief Regression tests: a restored layout must not resurrect full screen.
 *
 * `UIManager::ExportLayout()` is `QWidget::saveGeometry()` plus the ADS dock
 * state, and `saveGeometry()` carries the window *state* (maximized, full
 * screen) alongside the rectangle. `restoreGeometry()` re-applies both, so a
 * project saved while the window happened to be full screen started the app
 * full screen — and a full-screen QMainWindow has no frame at all: no minimize,
 * maximize or close button, with nothing in the menus to undo it. Alt+F4 and
 * File → Exit were the only ways out.
 *
 * Measured on the shipped blob, on Windows: `WS_CAPTION`, `WS_MINIMIZEBOX`,
 * `WS_MAXIMIZEBOX` and `WS_THICKFRAME` all clear, the frame at exactly the
 * monitor size (1707x1067), while the same window with the blob emptied
 * restored framed at 1600x900.
 *
 * What these tests pin:
 * - `ApplyLayout()` clears a full-screen state carried by the blob, so a
 *   restored window keeps its frame and its buttons.
 * - An ordinary blob restores without entering full screen.
 * - View → Full Screen exists and works both ways — it is the only way back
 *   once the window has no frame.
 * - The check mark follows the state even when the transition did not come from
 *   the menu (macOS's green button), which is what `changeEvent()` is for.
 */

#include <gtest/gtest.h>

#include <QAction>
#include <QKeySequence>
#include <QString>

#include <memory>
#include <string>

#include "ui/UIManager.h"

using namespace neurus;

namespace {

class LayoutRestoreTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_win = std::make_unique<UIManager>(QStringLiteral("en"), 60, QString());
		m_win->show();
	}
	void TearDown() override { m_win.reset(); }

	/// Leaves full screen without going through the menu's toggle.
	void LeaveFullScreen()
	{
		m_win->setWindowState(m_win->windowState() & ~Qt::WindowFullScreen);
	}

	/// Finds View → Full Screen by its standard shortcut rather than by text,
	/// which is translated.
	QAction* FindFullScreenAction() const
	{
		for (QAction* action : m_win->findChildren<QAction*>())
		{
			if (action->isCheckable() &&
			    action->shortcut() == QKeySequence(QKeySequence::FullScreen))
			{
				return action;
			}
		}
		return nullptr;
	}

	std::unique_ptr<UIManager> m_win;
};

} // namespace

TEST_F(LayoutRestoreTest, FullScreenStateIsNotRestoredFromTheLayoutBlob)
{
	m_win->showFullScreen();
	ASSERT_TRUE(m_win->isFullScreen());

	const std::string blob = m_win->ExportLayout();
	LeaveFullScreen();
	ASSERT_FALSE(m_win->isFullScreen());

	m_win->ApplyLayout(blob);
	EXPECT_FALSE(m_win->isFullScreen())
	    << "A blob saved in full screen brought back a window with no frame and "
	       "no button to leave it.";
}

TEST_F(LayoutRestoreTest, AnOrdinaryBlobRestoresWithoutEnteringFullScreen)
{
	const std::string blob = m_win->ExportLayout();
	m_win->ApplyLayout(blob);
	EXPECT_FALSE(m_win->isFullScreen());
}

TEST_F(LayoutRestoreTest, ViewMenuOffersAWorkingFullScreenToggle)
{
	QAction* toggle = FindFullScreenAction();
	ASSERT_NE(toggle, nullptr)
	    << "View > Full Screen is the only way out of a frame-less window.";

	EXPECT_FALSE(toggle->isChecked());
	toggle->setChecked(true);
	EXPECT_TRUE(m_win->isFullScreen());
	EXPECT_TRUE(toggle->isChecked());

	toggle->setChecked(false);
	EXPECT_FALSE(m_win->isFullScreen());
}

TEST_F(LayoutRestoreTest, FullScreenToggleFollowsTheWindowState)
{
	QAction* toggle = FindFullScreenAction();
	ASSERT_NE(toggle, nullptr);

	// Entering full screen without touching the menu must still tick the box:
	// the state can arrive from the platform, not only from the action.
	m_win->showFullScreen();
	ASSERT_TRUE(m_win->isFullScreen());
	EXPECT_TRUE(toggle->isChecked());

	LeaveFullScreen();
	ASSERT_FALSE(m_win->isFullScreen());
	EXPECT_FALSE(toggle->isChecked());
}
