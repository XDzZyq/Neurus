#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace neurus::ipc
{

/// Maximum request body accepted by the server (1 MiB).
inline constexpr std::size_t kMaxRequestBytes = 1u << 20;

/// Maximum response body produced by the server (4 MiB).
inline constexpr std::size_t kMaxResponseBytes = 4u << 20;

/// Result of pulling one frame out of a decoder.
enum class FrameStatus
{
	Ok,
	NeedMore,
	Invalid,
	TooLarge
};

/**
 * @brief Encodes a body as a 4-byte big-endian length followed by the body.
 * @param body UTF-8 JSON payload.
 * @return The framed bytes.
 */
std::string EncodeFrame(std::string_view body);

/**
 * @brief Incremental decoder for length-prefixed frames.
 *
 * Accepts arbitrary read chunks, including partial headers and several
 * coalesced frames, and never allocates beyond its configured cap.
 */
class FrameDecoder
{
public:
	/**
	 * @param maxBodyBytes Maximum accepted body size.
	 */
	explicit FrameDecoder(std::size_t maxBodyBytes);

	/**
	 * @brief Appends bytes read from the transport.
	 * @param bytes Newly received bytes.
	 */
	void Append(std::string_view bytes);

	/**
	 * @brief Extracts the next complete frame.
	 * @param body Receives the body when the status is FrameStatus::Ok.
	 * @return Ok, NeedMore, Invalid or TooLarge.
	 */
	FrameStatus Next(std::string& body);

	/// @return Number of bytes buffered but not yet consumed.
	std::size_t Buffered() const;

private:
	std::size_t m_maxBodyBytes;
	std::string m_buffer;
};

} // namespace neurus::ipc
