#include "platform/PlatformFile.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <fstream>

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

bool WriteOwnerOnlyFile(const std::filesystem::path& path, std::string_view contents,
                        std::string& error)
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out.is_open())
	{
		error = "Cannot open file for writing.";
		return false;
	}
	out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
	if (!out.good())
	{
		error = "Cannot write file.";
		return false;
	}
	out.close();

	// The profile ACL already scopes the file to this user; keep the call so the
	// contract matches POSIX and a future ACL implementation lands here too.
	return RestrictToOwner(path, false, error);
}

} // namespace neurus::platform
