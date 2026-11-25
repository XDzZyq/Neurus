#include "ui/panels/Viewport.hpp"

#include <QSplitter>
#include <QWindow>
#include <gtest/gtest.h>

TEST(ViewportTest, ShrinkingDockFromLeftKeepsNativeRightEdgeInside)
{
	QWidget window;
	window.resize(500, 200);
	QWidget dock(&window);
	dock.setGeometry(50, 0, 300, 100);
	QWidget content(&dock);
	content.resize(dock.size());
	neurus::Viewport viewport(&content);
	viewport.resize(content.size());
	window.show();

	auto* native = viewport.windowHandle();
	auto* parent = viewport.nativeParentWidget();
	ASSERT_NE(native, nullptr);
	ASSERT_NE(parent, nullptr);
	const auto dockRight = [&] { return dock.mapTo(parent, QPoint{}).x() + dock.width(); };
	ASSERT_EQ(native->x() + native->width(), dockRight());

	// The ancestor move arrives before the dock layout fits its children.
	dock.setGeometry(100, 0, 250, 100);
	EXPECT_LE(native->x() + native->width(), dockRight());
	content.resize(dock.size());
	viewport.resize(content.size());
	EXPECT_EQ(native->geometry(), QRect(viewport.mapTo(parent, QPoint{}), viewport.size()));
}

TEST(ViewportTest, NativeWindowFollowsDockAtMinimumWidth)
{
	QSplitter splitter(Qt::Horizontal);
	QWidget left, dock, right;
	splitter.addWidget(&left);
	splitter.addWidget(&dock);
	splitter.addWidget(&right);
	splitter.setChildrenCollapsible(false);
	dock.setFixedWidth(60);
	neurus::Viewport viewport(&dock);
	viewport.setFixedSize(60, 100);
	splitter.resize(500, 100);
	splitter.setSizes({100, 60, 300});
	splitter.show();

	auto* native = viewport.windowHandle();
	auto* parent = viewport.nativeParentWidget();
	ASSERT_NE(native, nullptr);
	ASSERT_NE(parent, nullptr);
	const QPoint before = viewport.mapTo(&splitter, QPoint{});
	ASSERT_EQ(native->position(), viewport.mapTo(parent, QPoint{}));

	splitter.setSizes({200, 60, 200});
	ASSERT_NE(viewport.mapTo(&splitter, QPoint{}), before);
	ASSERT_EQ(viewport.size(), QSize(60, 100));
	EXPECT_EQ(native->position(), viewport.mapTo(parent, QPoint{}));
}
