/**
 * @file test_floating_window.cpp
 * @brief Regression tests: full screen forbids tearing panels into windows.
 *
 * A torn-off panel is a separate top-level window, and a separate window cannot
 * be used inside another window's full-screen Space. macOS gives ADS's plain
 * Qt::Window floating container FullScreenPrimary collection behaviour
 * (qcocoawindow.mm, setWindowFlags), and the window server then refuses to let
 * the user move it — the panel appears but is frozen in place. Adding
 * Qt::CustomizeWindowHint to flip that to FullScreenAuxiliary was tried first
 * and did not help in practice, so full screen now forbids the gesture instead.
 *
 * What these tests pin:
 * - Full screen clears DockWidgetFloatable, so ADS's drop handler
 *   (FloatingDragPreview::createFloatingWidget) creates no window.
 * - DockWidgetMovable survives, so re-docking inside the window still works.
 *   Locking both would be a much larger behaviour change than the bug requires.
 * - Leaving full screen restores floatability; the lock is a mask, not a
 *   mutation of the per-widget flags.
 * - A panel already floating when full screen begins gets docked back rather
 *   than stranded on the desktop Space.
 *
 * showFullScreen() is Qt's own full-screen path, not a native macOS Space — but
 * it raises the same QEvent::WindowStateChange that drives the lock, which is
 * the logic under test. The window-server behaviour itself is not testable
 * headlessly.
 */

#include <gtest/gtest.h>

#include <QString>

#include <DockManager.h>
#include <DockWidget.h>
#include <FloatingDockContainer.h>

#include "ui/UIManager.h"
#include "ui/panels/UIPanel.h"

using namespace neurus;

namespace {

/** @brief The Log panel: a normal dock, unlike the non-floatable Viewport. */
constexpr PanelType kFloatable = PanelType::Log;

class FloatingWindowTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_win = std::make_unique<UIManager>(QStringLiteral("en"), 60, QString());
		m_dock = m_win->GetDock(kFloatable);
		ASSERT_NE(m_dock, nullptr);
	}
	void TearDown() override { m_win.reset(); }

	bool CanFloat() const
	{
		return m_dock->features().testFlag(ads::CDockWidget::DockWidgetFloatable);
	}
	bool CanMove() const
	{
		return m_dock->features().testFlag(ads::CDockWidget::DockWidgetMovable);
	}
	int FloatingWindowCount() const
	{
		return int(m_dock->dockManager()->floatingWidgets().size());
	}

	std::unique_ptr<UIManager> m_win;
	ads::CDockWidget*          m_dock = nullptr;
};

TEST_F(FloatingWindowTest, PanelsFloatFreelyWhenNotFullScreen)
{
	EXPECT_TRUE(CanFloat());

	m_dock->setFloating();
	EXPECT_TRUE(m_dock->isFloating());
	EXPECT_EQ(FloatingWindowCount(), 1);
}

TEST_F(FloatingWindowTest, FullScreenClearsFloatableButKeepsMovable)
{
	m_win->showFullScreen();
	ASSERT_TRUE(m_win->isFullScreen());

	EXPECT_FALSE(CanFloat()) << "full screen must forbid tearing a panel off";
	EXPECT_TRUE(CanMove()) << "re-docking inside the window must still work";
}

// The lock is what ADS consults at drop time, so this is the behavioural
// equivalent of "drag it out and let go": nothing separates.
TEST_F(FloatingWindowTest, TearingOffIsRefusedWhileFullScreen)
{
	m_win->showFullScreen();
	ASSERT_TRUE(m_win->isFullScreen());

	m_dock->setFloating();

	EXPECT_FALSE(m_dock->isFloating());
	EXPECT_EQ(FloatingWindowCount(), 0);
	EXPECT_NE(m_dock->dockAreaWidget(), nullptr) << "the panel must stay docked";
}

TEST_F(FloatingWindowTest, LeavingFullScreenRestoresFloatability)
{
	m_win->showFullScreen();
	ASSERT_FALSE(CanFloat());

	m_win->showNormal();
	ASSERT_FALSE(m_win->isFullScreen());

	EXPECT_TRUE(CanFloat());
	m_dock->setFloating();
	EXPECT_TRUE(m_dock->isFloating());
}

// Entering full screen with a panel already in its own window: the window would
// stay behind on the desktop Space, so the panel is docked back instead.
TEST_F(FloatingWindowTest, EnteringFullScreenDocksAlreadyFloatingPanels)
{
	m_dock->setFloating();
	ASSERT_TRUE(m_dock->isFloating());

	m_win->showFullScreen();
	ASSERT_TRUE(m_win->isFullScreen());

	EXPECT_FALSE(m_dock->isFloating());
	EXPECT_NE(m_dock->dockAreaWidget(), nullptr);
}

// The lock masks features rather than rewriting them, so a maximize/restore
// cycle (which raises the same WindowStateChange event) must not leak state.
TEST_F(FloatingWindowTest, MaximizeDoesNotAffectFloatability)
{
	m_win->showMaximized();
	EXPECT_TRUE(CanFloat());

	m_win->showNormal();
	EXPECT_TRUE(CanFloat());
}

} // namespace
