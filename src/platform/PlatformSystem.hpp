#pragma once

#include <cstdio>
#include <ctime>

namespace neurus::platform
{

/**
 * @brief Converts @p time to local calendar time.
 *
 * Thread-safe wrapper over the platform's reentrant conversion, so callers do
 * not need `#ifdef` blocks for `localtime_s` versus `localtime_r`.
 * @param time Seconds since the epoch, as produced by std::time().
 * @return The local calendar time; zeroed when the conversion fails.
 */
std::tm LocalTime(std::time_t time);

/**
 * @brief Returns whether @p stream is an interactive terminal.
 * @param stream Stream to test, usually stdout or stderr.
 * @return True when ANSI colour is safe because the stream is a TTY.
 */
bool IsTerminal(std::FILE* stream);

/**
 * @brief Returns whether the OS draws the app icon from the installed bundle.
 *
 * True on macOS, where the Dock shows the bundle's `.icns` and a runtime window
 * icon would override it; false on Windows, where the executable's embedded
 * icon needs the runtime `setWindowIcon` fallback.
 */
bool BundleProvidesAppIcon();

} // namespace neurus::platform
