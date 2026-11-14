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
	// The Linux build is headless: there is no surface to create, so callers
	// skip the GPU tests that need a native window.
	return false;
}

NativeWindowHandle TestWindow::Handle() const
{
	return nullptr;
}

} // namespace neurus::test
