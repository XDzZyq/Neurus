#include "platform/PlatformCrypto.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

namespace neurus::platform
{

bool FillSecureRandom(void* data, std::size_t size)
{
	const NTSTATUS status = BCryptGenRandom(
		nullptr, static_cast<PUCHAR>(data), static_cast<ULONG>(size),
		BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	return status >= 0;
}

} // namespace neurus::platform
