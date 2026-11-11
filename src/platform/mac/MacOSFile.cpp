#include "platform/PlatformFile.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
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

bool WriteOwnerOnlyFile(const std::filesystem::path& path, std::string_view contents,
                        std::string& error)
{
	// The mode only applies to a file created here; a pre-existing one keeps
	// whatever it had, so fchmod below tightens it before the first byte lands.
	const int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
	if (fd < 0)
	{
		error = std::strerror(errno);
		return false;
	}
	if (::fchmod(fd, 0600) != 0)
	{
		error = std::strerror(errno);
		::close(fd);
		return false;
	}

	std::size_t offset = 0;
	while (offset < contents.size())
	{
		const ssize_t written = ::write(fd, contents.data() + offset, contents.size() - offset);
		if (written < 0)
		{
			if (errno == EINTR)
				continue;
			error = std::strerror(errno);
			::close(fd);
			return false;
		}
		if (written == 0)
		{
			error = "short write";
			::close(fd);
			return false;
		}
		offset += static_cast<std::size_t>(written);
	}

	if (::close(fd) != 0)
	{
		error = std::strerror(errno);
		return false;
	}
	return true;
}

} // namespace neurus::platform
