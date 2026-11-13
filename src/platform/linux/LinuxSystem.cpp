#include "platform/PlatformSystem.h"

#include <unistd.h>

namespace neurus::platform
{

std::tm LocalTime(std::time_t time)
{
	std::tm result{};
	// localtime_r writes into the caller's buffer; there is no shared static.
	localtime_r(&time, &result);
	return result;
}

bool IsTerminal(std::FILE* stream)
{
	return stream != nullptr && ::isatty(::fileno(stream)) != 0;
}

bool BundleProvidesAppIcon()
{
	// No app bundle exists on Linux, so the runtime window icon is the only
	// icon the window system can show.
	return false;
}

} // namespace neurus::platform
