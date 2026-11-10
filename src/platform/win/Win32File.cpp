#include "platform/PlatformFile.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace neurus::platform
{

bool CreateExclusiveFile(const std::filesystem::path& path, std::string_view contents)
{
	const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
	                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle == INVALID_HANDLE_VALUE)
		return false;

	DWORD written = 0;
	const bool ok = WriteFile(handle, contents.data(), static_cast<DWORD>(contents.size()),
	                          &written, nullptr) != 0;
	CloseHandle(handle);
	return ok;
}

bool RestrictToOwner(const std::filesystem::path& /*path*/, bool /*directory*/,
                     std::string& /*error*/)
{
	// The per-user profile already carries the ACL; there is no portable chmod.
	return true;
}

} // namespace neurus::platform
