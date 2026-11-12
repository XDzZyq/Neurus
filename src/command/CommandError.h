#pragma once

#include <optional>
#include <string_view>

namespace neurus::command
{

/**
 * @brief Stable, machine-facing command error codes.
 *
 * The string form is part of the IPC protocol and must not change without a
 * protocol version bump.
 */
enum class CommandErrorCode
{
	None,
	InvalidArgument,
	SessionNotFound,
	SessionExists,
	AmbiguousSession,
	NoSessions,
	DirtyProject,
	SaveFailed,
	IoError,
	ScriptError,
	ObjectNotFound,
	AmbiguousObject,
	Unauthorized,
	ProtocolVersionMismatch,
	FrameInvalid,
	FrameTooLarge,
	ServerBusy,
	Timeout,
	AppUnreachable,
	AppExited,
	Internal
};

/**
 * @brief Returns the wire string for an error code.
 * @param code Error code to render.
 * @return The stable protocol string, or an empty view for CommandErrorCode::None.
 */
std::string_view ToString(CommandErrorCode code);

/**
 * @brief Parses a wire error string.
 * @param text Protocol string, for example "DIRTY_PROJECT".
 * @return The matching code, or std::nullopt when the string is unknown.
 */
std::optional<CommandErrorCode> ParseErrorCode(std::string_view text);

} // namespace neurus::command
