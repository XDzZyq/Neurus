#pragma once

#include "platform/PlatformProcess.hpp"

#include <charconv>
#include <chrono>

namespace neurus::resize_probe {

// Numeric values select a fixed-padding control for comparisons.
inline int FixedPadding()
{
	static const int padding = [] {
		const std::string mode = EnvironmentValue("NEURUS_RESIZE_PROBE");
		int pixels = 0;
		const char* end = mode.data() + mode.size();
		const auto [ptr, error] = std::from_chars(mode.data(), end, pixels);
		return error == std::errc{} && ptr == end && pixels >= 0 ? pixels : -1;
	}();
	return padding;
}

inline bool Enabled()
{
	static const bool enabled = FixedPadding() >= 0 || EnvironmentValue("NEURUS_RESIZE_PROBE") == "adaptive";
	return enabled;
}

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::duration<double, std::milli>;

inline Clock::time_point Timestamp(bool enabled)
{
	return enabled ? Clock::now() : Clock::time_point{};
}

} // namespace neurus::resize_probe
