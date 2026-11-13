#include "platform/PlatformPaths.h"

#include <cerrno>
#include <cstdlib>
#include <vector>

#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

namespace neurus {

std::filesystem::path HomeDirectory()
{
	const char* home = std::getenv("HOME");
	if (home && *home)
		return home;

	// A service or container process may not export HOME; the password
	// database still names the account's directory.
	std::size_t size = 1024;
	for (;;)
	{
		std::vector<char> buffer(size);
		passwd entry{};
		passwd* result = nullptr;
		const int status = ::getpwuid_r(::getuid(), &entry, buffer.data(), buffer.size(),
		                                &result);
		if (status == 0)
		{
			if (result != nullptr && result->pw_dir != nullptr && *result->pw_dir)
				return result->pw_dir;
			break;
		}
		if (status != ERANGE)
			break;
		size *= 2;
	}
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
