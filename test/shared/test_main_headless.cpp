// GTest entry point for the headless (Qt-free) build.
//
// The Qt build uses test_main.cpp, which creates a QApplication so the UIEvents
// signal bus and the QWidget tests work. A headless build has no Qt and no
// window system, so the runner only initialises GoogleTest.

#include <gtest/gtest.h>

int main(int argc, char** argv)
{
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
