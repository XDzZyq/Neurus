#pragma once

#include <string>
#include <string_view>

namespace neurus::ipc
{

/**
 * @brief Generates a 256-bit session token as 64 lowercase hex characters.
 *
 * Uses the operating system CSPRNG. Never reuse a token across launches.
 */
std::string GenerateToken();

/**
 * @brief Compares two strings without an early exit.
 * @param left First value.
 * @param right Second value.
 * @return True when both have the same length and bytes.
 */
bool ConstantTimeEquals(std::string_view left, std::string_view right);

} // namespace neurus::ipc
