#include "platform/PlatformCrypto.hpp"

#include <stdlib.h>

namespace neurus::platform
{

bool FillSecureRandom(void* data, std::size_t size)
{
	arc4random_buf(data, size);
	return true;
}

} // namespace neurus::platform
