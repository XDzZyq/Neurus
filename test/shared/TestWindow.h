#pragma once

#include "platform/PlatformSurface.h"

#include <memory>

namespace neurus::test
{

/**
 * @brief Native window used by GPU tests that need a presentation surface.
 *
 * Windows creates a hidden Win32 window. macOS reports invalid: headless
 * surface creation needs a Cocoa event loop that the test binary does not run.
 */
class TestWindow
{
public:
	TestWindow();
	~TestWindow();

	TestWindow(const TestWindow&) = delete;
	TestWindow& operator=(const TestWindow&) = delete;

	/// @return True when a native window was created.
	bool IsValid() const;

	/// @return The native handle, or nullptr when IsValid() is false.
	NativeWindowHandle Handle() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace neurus::test
