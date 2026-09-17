#include "platform/PlatformPaths.h"

#include <cstdlib>

namespace neurus {

std::filesystem::path HomeDirectory()
{
#ifdef _WIN32
	const char* home = std::getenv("USERPROFILE");
#else
	const char* home = std::getenv("HOME");
#endif
	if (home && *home)
		return home;
	return std::filesystem::current_path();
}

} // namespace neurus
