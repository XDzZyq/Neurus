#pragma once

#include "command/CommandDescriptor.h"
#include "command/CommandTypes.h"
#include "command/ICommandExecutor.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace neurus::ipc
{

/// Server configuration for one named session.
struct IpcServerOptions
{
	std::string               token;          ///< Per-session secret from GenerateToken().
	command::ICommandExecutor* executor = nullptr;  ///< Called only on the main thread.
	command::CommandCatalog   catalog;        ///< Commands this server accepts.
	std::string               appVersion;     ///< Reported by the hello handshake.
};

/**
 * @brief Loopback command server with a main-thread drain.
 *
 * Starts one Asio thread that only accepts, reads and writes. Command
 * execution happens exclusively in DrainOnMainThread(), so editor state is
 * never touched from the I/O thread.
 */
class IpcServer
{
public:
	struct Impl;

	explicit IpcServer(IpcServerOptions options);
	~IpcServer();

	IpcServer(const IpcServer&) = delete;
	IpcServer& operator=(const IpcServer&) = delete;

	/**
	 * @brief Binds the loopback listener and starts the I/O thread.
	 * @param port Port to bind, or 0 for an ephemeral port.
	 * @param error Receives a reason on failure.
	 */
	bool Start(std::uint16_t port, std::string& error);

	/// Stops accepting, stops the I/O thread and closes the listener.
	void Stop();

	/// @return The bound loopback port.
	std::uint16_t Port() const;

	/**
	 * @brief Executes pending commands on the calling thread.
	 * @return Number of commands executed (at most 16).
	 */
	std::size_t DrainOnMainThread();

	/**
	 * @brief Waits until every queued response has been written.
	 * @param timeout Upper bound on the wait.
	 * @return True when no write is pending.
	 */
	bool WaitForWrites(std::chrono::milliseconds timeout);

	/**
	 * @brief Waits until at least one command is queued for the main thread.
	 * @param timeout Upper bound on the wait.
	 */
	bool WaitForPending(std::chrono::milliseconds timeout);

	/// @return Number of commands waiting for the main-thread drain.
	std::size_t PendingCommands() const;

private:
	std::unique_ptr<Impl> m_impl;
};

} // namespace neurus::ipc
