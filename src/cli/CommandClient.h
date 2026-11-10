#pragma once

#include "cli/CliParse.h"
#include "command/CommandTypes.h"

#include <optional>
#include <string>

namespace neurus::cli
{

/**
 * @brief Resolves the target session, sends one command and reports failures.
 *
 * On success the caller owns the returned result and prints its payload. On
 * failure the error has already been printed in @p format and @p exitCode is
 * set, so the caller only returns it.
 *
 * @param session Explicit session name, or std::nullopt for the only session.
 * @param request Command to send.
 * @param format Output format for the error envelope.
 * @param exitCode Receives the process exit code.
 * @param timeoutSeconds Command timeout.
 * @return The successful result, or std::nullopt after reporting an error.
 */
std::optional<command::CommandResult> RunSessionCommand(
	const std::optional<std::string>& session,
	const command::CommandRequest& request,
	OutputFormat format,
	CliExit& exitCode,
	int timeoutSeconds = 10);

} // namespace neurus::cli
