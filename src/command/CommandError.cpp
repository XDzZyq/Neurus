#include "command/CommandError.h"

namespace neurus::command
{

std::string_view ToString(CommandErrorCode code)
{
	switch (code)
	{
	case CommandErrorCode::None:
		return {};
	case CommandErrorCode::InvalidArgument:
		return "INVALID_ARGUMENT";
	case CommandErrorCode::SessionNotFound:
		return "SESSION_NOT_FOUND";
	case CommandErrorCode::SessionExists:
		return "SESSION_EXISTS";
	case CommandErrorCode::AmbiguousSession:
		return "AMBIGUOUS_SESSION";
	case CommandErrorCode::NoSessions:
		return "NO_SESSIONS";
	case CommandErrorCode::DirtyProject:
		return "DIRTY_PROJECT";
	case CommandErrorCode::SaveFailed:
		return "SAVE_FAILED";
	case CommandErrorCode::IoError:
		return "IO_ERROR";
	case CommandErrorCode::ObjectNotFound:
		return "OBJECT_NOT_FOUND";
	case CommandErrorCode::AmbiguousObject:
		return "AMBIGUOUS_OBJECT";
	case CommandErrorCode::Unauthorized:
		return "UNAUTHORIZED";
	case CommandErrorCode::ProtocolVersionMismatch:
		return "PROTOCOL_VERSION_MISMATCH";
	case CommandErrorCode::FrameInvalid:
		return "FRAME_INVALID";
	case CommandErrorCode::FrameTooLarge:
		return "FRAME_TOO_LARGE";
	case CommandErrorCode::ServerBusy:
		return "SERVER_BUSY";
	case CommandErrorCode::Timeout:
		return "TIMEOUT";
	case CommandErrorCode::AppUnreachable:
		return "APP_UNREACHABLE";
	case CommandErrorCode::AppExited:
		return "APP_EXITED";
	case CommandErrorCode::Internal:
		return "INTERNAL";
	}

	return "INTERNAL";
}

std::optional<CommandErrorCode> ParseErrorCode(std::string_view text)
{
	for (auto code : {CommandErrorCode::None, CommandErrorCode::InvalidArgument,
	                  CommandErrorCode::SessionNotFound, CommandErrorCode::SessionExists,
	                  CommandErrorCode::AmbiguousSession, CommandErrorCode::NoSessions,
	                  CommandErrorCode::DirtyProject, CommandErrorCode::SaveFailed,
	                  CommandErrorCode::IoError, CommandErrorCode::ObjectNotFound,
	                  CommandErrorCode::AmbiguousObject, CommandErrorCode::Unauthorized,
	                  CommandErrorCode::ProtocolVersionMismatch,
	                  CommandErrorCode::FrameInvalid, CommandErrorCode::FrameTooLarge,
	                  CommandErrorCode::ServerBusy, CommandErrorCode::Timeout,
	                  CommandErrorCode::AppUnreachable, CommandErrorCode::AppExited,
	                  CommandErrorCode::Internal})
	{
		if (ToString(code) == text)
			return code;
	}
	return std::nullopt;
}

} // namespace neurus::command
