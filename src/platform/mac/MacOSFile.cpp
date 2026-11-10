#include "platform/PlatformFile.h"

#include <cerrno>
#include <fcntl.h>
#include <system_error>
#include <unistd.h>

namespace neurus::platform
{

bool CreateExclusiveFile(const std::filesystem::path& path, std::string_view contents)
{
	const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
	if (fd < 0)
		return false;

	std::size_t offset = 0;
	while (offset < contents.size())
	{
		const ssize_t written = ::write(fd, contents.data() + offset, contents.size() - offset);
		if (written <= 0)
		{
			::close(fd);
			return false;
		}
		offset += static_cast<std::size_t>(written);
	}
	::close(fd);
	return true;
}

bool RestrictToOwner(const std::filesystem::path& path, bool directory, std::string& error)
{
	std::error_code code;
	const auto permissions = directory
		? std::filesystem::perms::owner_all
		: std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
	std::filesystem::permissions(path, permissions,
	                             std::filesystem::perm_options::replace, code);
	if (code)
	{
		error = code.message();
		return false;
	}
	return true;
}

} // namespace neurus::platform
