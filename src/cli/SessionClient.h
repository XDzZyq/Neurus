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
