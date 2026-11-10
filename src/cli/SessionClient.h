#pragma once

#include "command/CommandJson.h"
#include "ipc/SessionRegistry.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace neurus::cli
{

/// @return Every registry entry whose process is still alive.
std::vector<ipc::SessionRecord> LiveSessions();

/**
 * @brief Resolves the effective session name for a command.
 *
 * Order: explicit name, `NEURUS_SESSION`, the terminal binding, then none (the
 * registry resolver picks the only live session). A binding whose session is no
 * longer live is cleared so a stale terminal does not block later commands.
 */
std::optional<std::string> ResolveSessionNameForCommand(
	const std::optional<std::string>& explicitName,
	const std::vector<ipc::SessionRecord>& live);

/**
 * @brief Sends one command to one session.
 * @param record Target session.
 * @param request Command to send.
 * @param id Correlation id.
 * @param error Receives a transport/protocol failure.
 * @param timeoutSeconds Command timeout.
 */
std::optional<command::CommandResult> SendCommand(const ipc::SessionRecord& record,
                                                  const command::CommandRequest& request,
                                                  std::uint64_t id,
                                                  command::CommandError& error,
                                                  int timeoutSeconds);

} // namespace neurus::cli
