#include "shared/TestWindow.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace neurus::test
{

struct TestWindow::Impl
{
	HWND window = nullptr;

	~Impl()
	{
		if (window != nullptr)
			DestroyWindow(window);
	}
};

TestWindow::TestWindow()
	: m_impl(std::make_unique<Impl>())
{
	const HINSTANCE instance = GetModuleHandle(nullptr);

	WNDCLASSEX windowClass{};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.lpfnWndProc = DefWindowProc;
	windowClass.hInstance = instance;
	windowClass.lpszClassName = L"NeurusTestWindow";
	RegisterClassEx(&windowClass);

	m_impl->window = CreateWindowEx(
		0, L"NeurusTestWindow", L"Test", WS_POPUP, 0, 0, 640, 480,
		nullptr, nullptr, instance, nullptr);
}

TestWindow::~TestWindow() = default;

bool TestWindow::IsValid() const
{
	return m_impl->window != nullptr;
}

NativeWindowHandle TestWindow::Handle() const
{
	return static_cast<NativeWindowHandle>(m_impl->window);
}

} // namespace neurus::test
