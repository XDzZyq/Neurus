#include "shared/TestWindow.h"

namespace neurus::test
{

struct TestWindow::Impl
{
};

TestWindow::TestWindow()
	: m_impl(std::make_unique<Impl>())
{
}

TestWindow::~TestWindow() = default;

bool TestWindow::IsValid() const
{
	// Headless surface creation needs a Cocoa event loop, which the test binary
	// does not run; callers skip the GPU test on macOS.
	return false;
}

NativeWindowHandle TestWindow::Handle() const
{
	return nullptr;
}

} // namespace neurus::test
