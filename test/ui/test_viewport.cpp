#include "ui/panels/Viewport.hpp"

#include <QSplitter>
#include <QWindow>
#include <gtest/gtest.h>

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
