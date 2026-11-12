#include "ipc/RandomToken.h"

#include "platform/PlatformCrypto.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace neurus::ipc
{
namespace
{

constexpr const char* kHexDigits = "0123456789abcdef";

} // namespace

std::string GenerateToken()
{
	std::array<std::uint8_t, 32> bytes{};
	if (!platform::FillSecureRandom(bytes.data(), bytes.size()))
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
