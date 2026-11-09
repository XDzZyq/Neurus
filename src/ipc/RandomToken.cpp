#include "ipc/RandomToken.h"

#include <array>
#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace neurus::ipc
{
namespace
{

constexpr const char* kHexDigits = "0123456789abcdef";

bool FillRandom(std::array<std::uint8_t, 32>& bytes)
{
#ifdef _WIN32
	const NTSTATUS status = BCryptGenRandom(
		nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
		BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	return status >= 0;
#else
	std::size_t offset = 0;
	const int fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0)
		return false;
	while (offset < bytes.size())
	{
		const ssize_t count = read(fd, bytes.data() + offset, bytes.size() - offset);
		if (count <= 0)
		{
			close(fd);
			return false;
		}
		offset += static_cast<std::size_t>(count);
	}
	close(fd);
	return true;
#endif
}

} // namespace

std::string GenerateToken()
{
	std::array<std::uint8_t, 32> bytes{};
	if (!FillRandom(bytes))
		return {};

	std::string token;
	token.reserve(bytes.size() * 2);
	for (const std::uint8_t byte : bytes)
	{
		token.push_back(kHexDigits[byte >> 4]);
		token.push_back(kHexDigits[byte & 0x0F]);
	}
	return token;
}

bool ConstantTimeEquals(std::string_view left, std::string_view right)
{
	// The length is public, so an early exit on it leaks nothing secret.
	if (left.size() != right.size())
		return false;

	unsigned char difference = 0;
	for (std::size_t i = 0; i < left.size(); ++i)
		difference |= static_cast<unsigned char>(left[i] ^ right[i]);
	return difference == 0;
}

} // namespace neurus::ipc
