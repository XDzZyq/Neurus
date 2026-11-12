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
	// Finder and the Dock draw the bundle icon; a window icon would replace it.
	return true;
}

} // namespace neurus::platform
