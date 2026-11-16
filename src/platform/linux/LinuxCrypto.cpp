#include "platform/PlatformCrypto.h"

#include "core/Log.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>

namespace neurus::platform
{

bool FillSecureRandom(void* data, std::size_t size)
{
	auto* bytes = static_cast<unsigned char*>(data);
	std::size_t offset = 0;
	while (offset < size)
	{
		const ssize_t filled = ::getrandom(bytes + offset, size - offset, 0);
		if (filled < 0)
		{
			if (errno == EINTR)
				continue;
			if (errno != ENOSYS)
			{
				NEURUS_ERR("getrandom failed: " << std::strerror(errno));
				return false;
			}
			break;
		}
		if (filled == 0)
			break;
		offset += static_cast<std::size_t>(filled);
	}
	if (offset == size)
		return true;

	// A kernel without getrandom(2) still exposes the same generator here.
	const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
	{
		NEURUS_ERR("Cannot open /dev/urandom: " << std::strerror(errno));
		return false;
	}
	while (offset < size)
	{
		const ssize_t filled = ::read(fd, bytes + offset, size - offset);
		if (filled < 0)
		{
			if (errno == EINTR)
				continue;
			NEURUS_ERR("Cannot read /dev/urandom: " << std::strerror(errno));
			::close(fd);
			return false;
		}
		if (filled == 0)
		{
			NEURUS_ERR("Cannot read /dev/urandom: unexpected end of file.");
			::close(fd);
			return false;
		}
		offset += static_cast<std::size_t>(filled);
	}
	::close(fd);
	return true;
}

} // namespace neurus::platform
