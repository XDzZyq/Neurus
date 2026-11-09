#include "platform/PlatformPaths.h"

#include <cstdlib>

namespace neurus {

std::filesystem::path HomeDirectory()
{
#ifdef _WIN32
	char* value = nullptr;
	size_t length = 0;
	if (_dupenv_s(&value, &length, "USERPROFILE") == 0 && value && *value)
	{
		std::filesystem::path home(value);
		std::free(value);
		return home;
	}
	std::free(value);
#else
	const char* home = std::getenv("HOME");
	if (home && *home)
		return home;
#endif
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
