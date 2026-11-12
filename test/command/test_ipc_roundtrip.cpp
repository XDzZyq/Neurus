/**
 * @file test_ipc_roundtrip.cpp
 * @brief Real loopback server/client protocol tests.
 */

#include <gtest/gtest.h>

#include <asio.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "command/CommandTypes.h"
#include "command/Commands.h"
#include "ipc/Frame.h"
#include "ipc/IpcClient.h"
#include "ipc/IpcServer.h"
#include "ipc/RandomToken.h"

using namespace neurus::command;
using namespace neurus::ipc;

namespace
{

class FakeExecutor final : public ICommandExecutor
{
public:
	CommandResult Execute(const CommandRequest& request) override
	{
		++calls;
		if (request.name == kSessionStatus)
			return CommandResult::Success({{"session", "test-session"}, {"pid", 1234}});
		if (request.name == kSessionClose)
			return CommandResult::Success({{"session", "test-session"}, {"pid", 1234}});
		return CommandResult::Failure(CommandErrorCode::InvalidArgument, "unknown");
	}

	std::atomic<int> calls{0};
};

class IpcRoundtripTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_token = GenerateToken();
		m_server = std::make_unique<IpcServer>(
			IpcServerOptions{m_token, &m_executor, MakeCommandCatalog(), "0.1.0"});
		std::string error;
		ASSERT_TRUE(m_server->Start(0, error)) << error;
	}

	void TearDown() override
	{
		if (m_server)
			m_server->Stop();
	}

	std::optional<CommandResult> RunCommand(IpcClient& client, const CommandRequest& request,
	                                        CommandError& error)
	{
		std::optional<CommandResult> result;
		std::thread caller([&] { result = client.Send(request, 2, error); });

		if (!m_server->WaitForPending(std::chrono::seconds(2)))
		{
			m_server->Stop();
			caller.join();
			return std::nullopt;
		}
		while (m_server->PendingCommands() > 0)
			m_server->DrainOnMainThread();
		caller.join();
		m_server->WaitForWrites(std::chrono::seconds(2));
		return result;
	}

	std::string m_token;
	FakeExecutor m_executor;
	std::unique_ptr<IpcServer> m_server;
};

std::string RawExchange(std::uint16_t port, const std::string& requestBytes)
{
	asio::io_context io;
	asio::ip::tcp::socket socket(io);
	socket.connect(asio::ip::tcp::endpoint(asio::ip::make_address_v4("127.0.0.1"), port));

	asio::write(socket, asio::buffer(requestBytes));

	std::array<char, 4> header{};
	asio::read(socket, asio::buffer(header));
	const auto length = (static_cast<std::uint32_t>(static_cast<unsigned char>(header[0])) << 24)
		| (static_cast<std::uint32_t>(static_cast<unsigned char>(header[1])) << 16)
		| (static_cast<std::uint32_t>(static_cast<unsigned char>(header[2])) << 8)
		| static_cast<std::uint32_t>(static_cast<unsigned char>(header[3]));
	std::string body(length, '\0');
	asio::read(socket, asio::buffer(body));
	return body;
}

} // namespace

TEST_F(IpcRoundtripTest, HelloThenStatus)
{
	IpcClient client({m_server->Port(), m_token, std::chrono::seconds(2)});
	CommandError error{};
	auto result = RunCommand(client, {kSessionStatus, {}}, error);
	ASSERT_TRUE(result.has_value()) << error.message;
	EXPECT_TRUE(result->ok);
	EXPECT_EQ(result->details["session"], "test-session");
	EXPECT_EQ(m_executor.calls.load(), 1);
}

TEST_F(IpcRoundtripTest, WrongTokenIsUnauthorizedAndExecutorNeverRuns)
{
	IpcClient client({m_server->Port(), "wrong-token", std::chrono::seconds(2)});
	CommandError error{};
	auto result = client.Send({kSessionStatus, {}}, 1, error);

	EXPECT_FALSE(result.has_value());
	EXPECT_EQ(error.code, CommandErrorCode::Unauthorized);
	EXPECT_EQ(m_executor.calls.load(), 0);
}

TEST_F(IpcRoundtripTest, UnknownCommandIsRejectedWithoutDispatching)
{
	IpcClient client({m_server->Port(), m_token, std::chrono::seconds(2)});
	CommandError error{};
	auto result = client.Send({"nonsense.command", {}}, 3, error);

	ASSERT_TRUE(result.has_value()) << error.message;
	EXPECT_FALSE(result->ok);
	EXPECT_EQ(result->code, CommandErrorCode::InvalidArgument);
	EXPECT_EQ(m_executor.calls.load(), 0);
}

TEST_F(IpcRoundtripTest, OversizedFrameIsRejected)
{
	// Send only the header: the body is larger than the cap, and the server
	// rejects it before the client could finish writing.
	const std::string header{'\x00', '\x10', '\x00', '\x01'};
	const auto response = RawExchange(m_server->Port(), header);

	CommandError error{};
	const auto decoded = DecodeResponse(response, error);
	ASSERT_TRUE(decoded.has_value()) << error.message;
	EXPECT_FALSE(decoded->result.ok);
	EXPECT_EQ(decoded->result.code, CommandErrorCode::FrameTooLarge);
}

TEST_F(IpcRoundtripTest, NoResponseBeforeTheMainThreadDrains)
{
	IpcClient client({m_server->Port(), m_token, std::chrono::milliseconds(150)});
	CommandError error{};
	auto result = client.Send({kSessionStatus, {}}, 4, error);

	EXPECT_FALSE(result.has_value());
	EXPECT_EQ(error.code, CommandErrorCode::Timeout);
	EXPECT_EQ(m_executor.calls.load(), 0);
}

TEST_F(IpcRoundtripTest, WaitForWritesReturnsAfterResponse)
{
	IpcClient client({m_server->Port(), m_token, std::chrono::seconds(2)});
	CommandError error{};
	auto result = RunCommand(client, {kSessionStatus, {}}, error);

	ASSERT_TRUE(result.has_value()) << error.message;
	EXPECT_TRUE(m_server->WaitForWrites(std::chrono::seconds(2)));
}
