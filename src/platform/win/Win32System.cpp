#include "platform/PlatformSystem.h"

#include <io.h>

namespace neurus::platform
{

std::tm LocalTime(std::time_t time)
{
	std::tm result{};
	// localtime_s writes into the caller's buffer; a failure leaves it zeroed.
	localtime_s(&result, &time);
	return result;
}

bool IsTerminal(std::FILE* stream)
{
	return stream != nullptr && ::_isatty(::_fileno(stream)) != 0;
}

bool BundleProvidesAppIcon()
{
	// Explorer and the taskbar read the executable's embedded icon, not a bundle.
	return false;
}

} // namespace neurus::platform
