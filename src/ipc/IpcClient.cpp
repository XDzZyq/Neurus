#include "ipc/IpcClient.h"

#include "command/Commands.h"
#include "ipc/Frame.h"

#include <asio.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

namespace neurus::ipc
{
namespace
{

using command::CommandError;
using command::CommandErrorCode;

using tcp = asio::ip::tcp;

std::uint32_t ReadBigEndian32(const char* data)
{
	return (static_cast<std::uint32_t>(static_cast<unsigned char>(data[0])) << 24)
		| (static_cast<std::uint32_t>(static_cast<unsigned char>(data[1])) << 16)
		| (static_cast<std::uint32_t>(static_cast<unsigned char>(data[2])) << 8)
		| static_cast<std::uint32_t>(static_cast<unsigned char>(data[3]));
}

bool ReadExact(tcp::socket& socket, asio::io_context& io, asio::mutable_buffer buffer,
               std::chrono::milliseconds timeout, CommandError& error)
{
	asio::error_code operationError;
	bool completed = false;
	asio::async_read(socket, buffer,
	                 [&](const asio::error_code& code, std::size_t)
	                 {
		                 operationError = code;
		                 completed = true;
	                 });
	io.restart();
	io.run_for(timeout);
	if (!completed)
	{
		error = {CommandErrorCode::Timeout, "Timed out waiting for the server."};
		asio::error_code ignored;
		socket.close(ignored);
		return false;
	}
	if (operationError)
	{
		error = {CommandErrorCode::AppUnreachable, operationError.message()};
		return false;
	}
	return true;
}

bool WriteExact(tcp::socket& socket, asio::io_context& io, std::string_view body,
                std::chrono::milliseconds timeout, CommandError& error)
{
	const std::string frame = EncodeFrame(body);
	asio::error_code operationError;
	bool completed = false;
	asio::async_write(socket, asio::buffer(frame),
	                  [&](const asio::error_code& code, std::size_t)
	                  {
		                  operationError = code;
		                  completed = true;
	                  });
	io.restart();
	io.run_for(timeout);
	if (!completed)
	{
		error = {CommandErrorCode::Timeout, "Timed out sending to the server."};
		asio::error_code ignored;
		socket.close(ignored);
		return false;
	}
	if (operationError)
	{
		error = {CommandErrorCode::AppUnreachable, operationError.message()};
		return false;
	}
	return true;
}

bool ReadFrame(tcp::socket& socket, asio::io_context& io, std::string& body,
               std::chrono::milliseconds timeout, CommandError& error)
{
	std::array<char, 4> header{};
	if (!ReadExact(socket, io, asio::buffer(header), timeout, error))
		return false;

	const std::uint32_t length = ReadBigEndian32(header.data());
	if (length == 0)
	{
		error = {CommandErrorCode::FrameInvalid, "Server sent an empty frame."};
		return false;
	}
	if (length > kMaxResponseBytes)
	{
		error = {CommandErrorCode::FrameTooLarge, "Server response exceeded the cap."};
		return false;
	}

	body.assign(length, '\0');
	return ReadExact(socket, io, asio::buffer(body), timeout, error);
}

bool Transact(tcp::socket& socket, asio::io_context& io, const std::string& requestBody,
              std::chrono::milliseconds timeout, std::string& responseBody, CommandError& error)
{
	if (!WriteExact(socket, io, requestBody, timeout, error))
		return false;
	return ReadFrame(socket, io, responseBody, timeout, error);
}

} // namespace

IpcClient::IpcClient(IpcClientOptions options)
	: m_options(std::move(options))
{
}

std::optional<command::CommandResult> IpcClient::Send(const command::CommandRequest& request,
                                                      std::uint64_t id,
                                                      CommandError& error) const
{
	asio::io_context io;
	tcp::socket socket(io);

	asio::error_code connectError;
	socket.connect(tcp::endpoint(asio::ip::make_address_v4("127.0.0.1"), m_options.port),
	               connectError);
	if (connectError)
	{
		error = {CommandErrorCode::AppUnreachable, connectError.message()};
		return std::nullopt;
	}

	const command::WireRequest hello{
		1,
		{command::kSessionHello,
		 {{"protocol_version", command::kProtocolVersion}, {"token", m_options.token}}},
	};

	std::string responseBody;
	if (!Transact(socket, io, command::EncodeRequest(hello), m_options.timeout, responseBody, error))
		return std::nullopt;

	auto helloResponse = command::DecodeResponse(responseBody, error);
	if (!helloResponse)
		return std::nullopt;
	if (!helloResponse->result.ok)
	{
		error = {helloResponse->result.code, helloResponse->result.message};
		return std::nullopt;
	}

	if (!Transact(socket, io, command::EncodeRequest({id, request}), m_options.timeout,
	              responseBody, error))
		return std::nullopt;

	auto response = command::DecodeResponse(responseBody, error);
	if (!response)
		return std::nullopt;
	if (response->id != id)
	{
		error = {CommandErrorCode::FrameInvalid, "Response id does not match the request."};
		return std::nullopt;
	}
	return response->result;
}

} // namespace neurus::ipc
