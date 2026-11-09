#include "ipc/IpcServer.h"

#include "command/CommandDispatcher.h"
#include "command/CommandJson.h"
#include "command/CommandTypes.h"
#include "command/Commands.h"
#include "ipc/Frame.h"
#include "ipc/RandomToken.h"

#include <asio.hpp>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace neurus::ipc
{
namespace
{

using command::CommandError;
using command::CommandErrorCode;
using command::CommandRequest;
using command::CommandResult;
using command::Json;
using command::WireResponse;

using tcp = asio::ip::tcp;

constexpr std::size_t kMaxConnections = 8;

std::uint32_t ReadBigEndian32(const char* data)
{
	return (static_cast<std::uint32_t>(static_cast<unsigned char>(data[0])) << 24)
		| (static_cast<std::uint32_t>(static_cast<unsigned char>(data[1])) << 16)
		| (static_cast<std::uint32_t>(static_cast<unsigned char>(data[2])) << 8)
		| static_cast<std::uint32_t>(static_cast<unsigned char>(data[3]));
}

class Connection;

} // namespace

struct IpcServer::Impl
{
	explicit Impl(IpcServerOptions serverOptions)
		: options(std::move(serverOptions))
	{
	}

	bool Start(std::uint16_t requestedPort, std::string& error);
	void Stop();
	void AcceptNext();

	void PostCommand(const std::shared_ptr<Connection>& connection,
	                 std::uint64_t id, const CommandRequest& request);
	void QueueWrite(const std::shared_ptr<Connection>& connection,
	                const WireResponse& response, bool closeAfter);
	void QueueError(const std::shared_ptr<Connection>& connection,
	                std::uint64_t id, CommandErrorCode code, std::string message);
	void EndWrite();
	void NotifyPending();

	IpcServerOptions          options;
	std::atomic<int>          activeConnections{0};
	asio::io_context          io;
	asio::ip::tcp::acceptor   acceptor{io};
	std::thread               thread;
	command::CommandDispatcher dispatcher;
	std::atomic<bool>         running{false};
	std::uint16_t             port = 0;

	std::mutex                writeMutex;
	std::condition_variable   writeCondition;
	std::size_t               pendingWrites = 0;

	std::mutex                pendingMutex;
	std::condition_variable   pendingCondition;
};

namespace
{

class Connection : public std::enable_shared_from_this<Connection>
{
public:
	Connection(tcp::socket socket, IpcServer::Impl& server)
		: m_socket(std::move(socket))
		, m_server(server)
	{
		++m_server.activeConnections;
	}

	~Connection()
	{
		--m_server.activeConnections;
	}

	void Start()
	{
		auto self = shared_from_this();
		ReadFrame([self](FrameStatus status, std::string body)
		{
			self->OnHello(status, std::move(body));
		});
	}

	void Write(const WireResponse& response, bool closeAfter)
	{
		m_outgoing.push_back(Outgoing{EncodeFrame(command::EncodeResponse(response)), closeAfter});
		if (!m_writing)
			StartWrite();
	}

	void Close()
	{
		asio::error_code ignored;
		m_socket.shutdown(tcp::socket::shutdown_both, ignored);
		m_socket.close(ignored);
	}

private:
	using FrameHandler = std::function<void(FrameStatus, std::string)>;

	struct Outgoing
	{
		std::string frame;
		bool        closeAfter = false;
	};

	void StartWrite()
	{
		if (m_outgoing.empty())
		{
			m_writing = false;
			return;
		}

		m_writing = true;
		m_output = std::move(m_outgoing.front().frame);
		const bool closeAfter = m_outgoing.front().closeAfter;
		auto self = shared_from_this();
		asio::async_write(m_socket, asio::buffer(m_output),
		                  [self, closeAfter](const asio::error_code& error, std::size_t)
		                  {
			                  self->m_server.EndWrite();
			                  if (!self->m_outgoing.empty())
				                  self->m_outgoing.pop_front();
			                  self->m_writing = false;
			                  if (error)
			                  {
				                  self->Close();
				                  return;
			                  }
			                  if (!self->m_outgoing.empty())
				                  self->StartWrite();
			                  else if (closeAfter)
				                  self->Close();
		                  });
	}

	void ReadFrame(FrameHandler handler)
	{
		auto self = shared_from_this();
		auto header = std::make_shared<std::array<char, 4>>();
		asio::async_read(m_socket, asio::buffer(*header),
		                 [self, header, handler = std::move(handler)](const asio::error_code& error,
		                                                             std::size_t) mutable
		                 {
			                 if (error)
			                 {
				                 handler(FrameStatus::Invalid, {});
				                 return;
			                 }
			                 const std::uint32_t length = ReadBigEndian32(header->data());
			                 if (length == 0)
			                 {
				                 handler(FrameStatus::Invalid, {});
				                 return;
			                 }
			                 if (length > kMaxRequestBytes)
			                 {
				                 handler(FrameStatus::TooLarge, {});
				                 return;
			                 }
			                 auto body = std::make_shared<std::string>(length, '\0');
			                 asio::async_read(
				                 self->m_socket, asio::buffer(*body),
				                 [self, body, handler = std::move(handler)](
					                 const asio::error_code& bodyError, std::size_t) mutable
				                 {
					                 if (bodyError)
					                 {
						                 handler(FrameStatus::Invalid, {});
						                 return;
					                 }
					                 handler(FrameStatus::Ok, std::move(*body));
				                 });
		                 });
	}

	void OnHello(FrameStatus status, std::string body)
	{
		if (status != FrameStatus::Ok)
		{
			m_server.QueueError(shared_from_this(), 0,
			                    status == FrameStatus::TooLarge ? CommandErrorCode::FrameTooLarge
			                                                    : CommandErrorCode::FrameInvalid,
			                    "Invalid hello frame.");
			return;
		}

		CommandError error{};
		const auto request = command::DecodeRequest(body, error);
		if (!request)
		{
			m_server.QueueError(shared_from_this(), 0, error.code, error.message);
			return;
		}
		if (request->request.name != command::kSessionHello)
		{
			m_server.QueueError(shared_from_this(), request->id, CommandErrorCode::Unauthorized,
			                    "The first command must be session.hello.");
			return;
		}

		const int version = request->request.args.value("protocol_version", 0);
		const std::string token = request->request.args.value("token", std::string{});
		if (version != command::kProtocolVersion)
		{
			m_server.QueueError(shared_from_this(), request->id,
			                    CommandErrorCode::ProtocolVersionMismatch,
			                    "Unsupported protocol version.");
			return;
		}
		if (!ConstantTimeEquals(token, m_server.options.token))
		{
			m_server.QueueError(shared_from_this(), request->id, CommandErrorCode::Unauthorized,
			                    "Invalid session token.");
			return;
		}

		Json commands = Json::array();
		for (const auto& descriptor : m_server.options.catalog.All())
			commands.push_back(descriptor.name);

		m_server.QueueWrite(
			shared_from_this(),
			{request->id,
			 CommandResult::Success({{"protocol_version", command::kProtocolVersion},
			                         {"app_version", m_server.options.appVersion},
			                         {"commands", commands}})},
			false);

		auto self = shared_from_this();
		ReadFrame([self](FrameStatus commandStatus, std::string commandBody)
		{
			self->OnCommand(commandStatus, std::move(commandBody));
		});
	}

	void OnCommand(FrameStatus status, std::string body)
	{
		if (status != FrameStatus::Ok)
		{
			m_server.QueueError(shared_from_this(), 0,
			                    status == FrameStatus::TooLarge ? CommandErrorCode::FrameTooLarge
			                                                    : CommandErrorCode::FrameInvalid,
			                    "Invalid command frame.");
			return;
		}

		CommandError error{};
		const auto request = command::DecodeRequest(body, error);
		if (!request)
		{
			m_server.QueueError(shared_from_this(), 0, error.code, error.message);
			return;
		}

		const auto* descriptor = m_server.options.catalog.Find(request->request.name);
		if (descriptor == nullptr)
		{
			Json available = Json::array();
			for (const auto& entry : m_server.options.catalog.All())
				available.push_back(entry.name);
			m_server.QueueWrite(
				shared_from_this(),
				{request->id,
				 CommandResult::Failure(CommandErrorCode::InvalidArgument,
				                        "Unknown command: " + request->request.name,
				                        {{"commands", available}})},
				true);
			return;
		}
		for (const auto& key : descriptor->requiredArgs)
		{
			if (!request->request.args.contains(key))
			{
				m_server.QueueError(shared_from_this(), request->id,
				                    CommandErrorCode::InvalidArgument,
				                    "Missing required argument: " + key);
				return;
			}
		}

		m_server.PostCommand(shared_from_this(), request->id, request->request);
	}

	tcp::socket     m_socket;
	IpcServer::Impl& m_server;
	std::string     m_output;
	std::deque<Outgoing> m_outgoing;
	bool            m_writing = false;
};

} // namespace

bool IpcServer::Impl::Start(std::uint16_t requestedPort, std::string& error)
{
	asio::error_code code;
	acceptor.open(tcp::v4(), code);
	if (code)
	{
		error = "Cannot open the loopback listener: " + code.message();
		return false;
	}
	acceptor.set_option(tcp::acceptor::reuse_address(true), code);
	acceptor.bind(tcp::endpoint(asio::ip::make_address_v4("127.0.0.1"), requestedPort), code);
	if (code)
	{
		error = "Cannot bind the loopback listener: " + code.message();
		return false;
	}
	acceptor.listen(static_cast<int>(kMaxConnections), code);
	if (code)
	{
		error = "Cannot listen on the loopback listener: " + code.message();
		return false;
	}
	port = acceptor.local_endpoint().port();
	running = true;
	AcceptNext();
	thread = std::thread([this] { io.run(); });
	return true;
}

void IpcServer::Impl::Stop()
{
	running = false;
	io.stop();
	if (thread.joinable())
		thread.join();
	asio::error_code ignored;
	acceptor.close(ignored);
	io.restart();
}

void IpcServer::Impl::AcceptNext()
{
	if (!running)
		return;
	acceptor.async_accept(
		[this](const asio::error_code& error, tcp::socket socket)
		{
			if (!error)
			{
				if (activeConnections.load() >= static_cast<int>(kMaxConnections))
				{
					asio::error_code ignored;
					socket.close(ignored);
				}
				else
				{
					auto connection = std::make_shared<Connection>(std::move(socket), *this);
					connection->Start();
				}
			}
			AcceptNext();
		});
}

void IpcServer::Impl::PostCommand(const std::shared_ptr<Connection>& connection,
                                  std::uint64_t id, const CommandRequest& request)
{
	const bool accepted = dispatcher.Post(
		id, request,
		[this, connection](std::uint64_t responseId, const CommandResult& result)
		{
			QueueWrite(connection, {responseId, result}, true);
		});
	if (!accepted)
	{
		QueueError(connection, id, CommandErrorCode::ServerBusy,
		           "The command queue is full.");
		return;
	}
	NotifyPending();
}

void IpcServer::Impl::QueueWrite(const std::shared_ptr<Connection>& connection,
                                 const WireResponse& response, bool closeAfter)
{
	{
		std::lock_guard<std::mutex> lock(writeMutex);
		++pendingWrites;
	}
	asio::post(io, [this, connection, response, closeAfter]
	{
		connection->Write(response, closeAfter);
	});
}

void IpcServer::Impl::QueueError(const std::shared_ptr<Connection>& connection,
                                 std::uint64_t id, CommandErrorCode code, std::string message)
{
	QueueWrite(connection, {id, CommandResult::Failure(code, std::move(message))}, true);
}

void IpcServer::Impl::EndWrite()
{
	{
		std::lock_guard<std::mutex> lock(writeMutex);
		if (pendingWrites > 0)
			--pendingWrites;
	}
	writeCondition.notify_all();
}

void IpcServer::Impl::NotifyPending()
{
	pendingCondition.notify_all();
}

// --- IpcServer forwarding -------------------------------------------------

IpcServer::IpcServer(IpcServerOptions options)
	: m_impl(std::make_unique<Impl>(std::move(options)))
{
}

IpcServer::~IpcServer()
{
	Stop();
}

bool IpcServer::Start(std::uint16_t port, std::string& error)
{
	return m_impl->Start(port, error);
}

void IpcServer::Stop()
{
	m_impl->Stop();
}

std::uint16_t IpcServer::Port() const
{
	return m_impl->port;
}

std::size_t IpcServer::DrainOnMainThread()
{
	if (m_impl->options.executor == nullptr)
		return 0;
	return m_impl->dispatcher.Drain(*m_impl->options.executor);
}

bool IpcServer::WaitForWrites(std::chrono::milliseconds timeout)
{
	std::unique_lock<std::mutex> lock(m_impl->writeMutex);
	return m_impl->writeCondition.wait_for(lock, timeout,
	                                       [this] { return m_impl->pendingWrites == 0; });
}

bool IpcServer::WaitForPending(std::chrono::milliseconds timeout)
{
	std::unique_lock<std::mutex> lock(m_impl->pendingMutex);
	return m_impl->pendingCondition.wait_for(
		lock, timeout, [this] { return m_impl->dispatcher.PendingCount() > 0; });
}

std::size_t IpcServer::PendingCommands() const
{
	return m_impl->dispatcher.PendingCount();
}

} // namespace neurus::ipc
