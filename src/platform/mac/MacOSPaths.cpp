#include "platform/PlatformPaths.h"

#include <cstdlib>

namespace neurus {

std::filesystem::path HomeDirectory()
{
	const char* home = std::getenv("HOME");
	if (home && *home)
		return home;
	return std::filesystem::current_path();
}

std::filesystem::path NeurusConfigDirectory()
{
	return HomeDirectory() / ".neurus";
}

std::filesystem::path SessionsDirectory()
{
	return NeurusConfigDirectory() / "sessions";
}

} // namespace neurus
