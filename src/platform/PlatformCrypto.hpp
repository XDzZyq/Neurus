#pragma once

#include <cstddef>

namespace neurus::platform
{

/**
 * @brief Fills a buffer with cryptographically secure random bytes.
 * @param data Destination buffer.
 * @param size Number of bytes to generate.
 * @return False when the operating-system generator is unavailable.
 */
bool FillSecureRandom(void* data, std::size_t size);

} // namespace neurus::platform
