#include "ipc/Frame.h"

#include <cstring>

namespace neurus::ipc
{
namespace
{

std::uint32_t ReadBigEndian32(const char* data)
{
	return (static_cast<std::uint32_t>(static_cast<unsigned char>(data[0])) << 24)
		| (static_cast<std::uint32_t>(static_cast<unsigned char>(data[1])) << 16)
		| (static_cast<std::uint32_t>(static_cast<unsigned char>(data[2])) << 8)
		| static_cast<std::uint32_t>(static_cast<unsigned char>(data[3]));
}

} // namespace

std::string EncodeFrame(std::string_view body)
{
	std::string frame;
	frame.resize(4 + body.size());
	const auto length = static_cast<std::uint32_t>(body.size());
	frame[0] = static_cast<char>((length >> 24) & 0xFF);
	frame[1] = static_cast<char>((length >> 16) & 0xFF);
	frame[2] = static_cast<char>((length >> 8) & 0xFF);
	frame[3] = static_cast<char>(length & 0xFF);
	if (!body.empty())
		std::memcpy(frame.data() + 4, body.data(), body.size());
	return frame;
}

FrameDecoder::FrameDecoder(std::size_t maxBodyBytes)
	: m_maxBodyBytes(maxBodyBytes)
{
}

void FrameDecoder::Append(std::string_view bytes)
{
	m_buffer.append(bytes.data(), bytes.size());
}

FrameStatus FrameDecoder::Next(std::string& body)
{
	body.clear();
	if (m_buffer.size() < 4)
		return FrameStatus::NeedMore;

	const std::uint32_t length = ReadBigEndian32(m_buffer.data());
	if (length == 0)
	{
		m_buffer.clear();
		return FrameStatus::Invalid;
	}
	if (length > m_maxBodyBytes)
	{
		// The body can never be accepted; drop it and let the caller close.
		m_buffer.clear();
		return FrameStatus::TooLarge;
	}
	if (m_buffer.size() < 4 + static_cast<std::size_t>(length))
		return FrameStatus::NeedMore;

	body.assign(m_buffer, 4, length);
	m_buffer.erase(0, 4 + static_cast<std::size_t>(length));
	return FrameStatus::Ok;
}

std::size_t FrameDecoder::Buffered() const
{
	return m_buffer.size();
}

} // namespace neurus::ipc
