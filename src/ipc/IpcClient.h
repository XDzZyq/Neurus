#pragma once

#include "command/CommandJson.h"
#include "command/CommandTypes.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace neurus::ipc
{

/// Connection settings for one CLI invocation.
struct IpcClientOptions
{
	std::uint16_t             port = 0;
	std::string               token;
	std::chrono::milliseconds timeout{10000};
};

/**
 * @brief Blocking loopback client: connect, authenticate, send one command.
 *
 * A fresh connection is used per call; there is no pooling or reconnect.
 */
class IpcClient
{
public:
	explicit IpcClient(IpcClientOptions options);

	/**
	 * @brief Sends one command after a hello handshake.
	 * @param request Command to execute.
	 * @param id Correlation id echoed by the server.
	 * @param error Receives transport or protocol failure detail.
	 * @return The server's result, or std::nullopt when the exchange failed.
	 */
	std::optional<command::CommandResult> Send(const command::CommandRequest& request,
	                                           std::uint64_t id,
	                                           command::CommandError& error) const;

private:
	IpcClientOptions m_options;
};

} // namespace neurus::ipc
