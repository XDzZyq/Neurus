#pragma once

#include "command/CommandError.h"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

namespace neurus::command
{

/// JSON value type used by the wire protocol. It never appears in scene or
/// editor headers, so the command layer stays independent of them.
using Json = nlohmann::json;

/**
 * @brief One command invocation: a registered name plus validated arguments.
 */
struct CommandRequest
{
	std::string name;              ///< Dotted command name, for example "session.status".
	Json        args = Json::object();  ///< Command-specific argument object.
};

/**
 * @brief Structured outcome of one command.
 *
 * A response with @ref ok set means the handler and every controller it
 * triggered have finished; the command layer has no accepted-but-pending state.
 */
struct CommandResult
{
	bool              ok = false;                            ///< True only on success.
	CommandErrorCode  code = CommandErrorCode::None;         ///< Stable error code.
	std::string       message;                               ///< Human-readable English detail.
	Json              details = Json::object();              ///< Result payload or error details.

	/**
	 * @brief Builds a successful result.
	 * @param resultDetails Optional structured payload.
	 */
	static CommandResult Success(Json resultDetails = Json::object())
	{
		CommandResult result;
		result.ok = true;
		result.details = std::move(resultDetails);
		return result;
	}

	/**
	 * @brief Builds a failed result.
	 * @param errorCode Stable error code.
	 * @param errorMessage Human-readable English detail.
	 * @param errorDetails Optional structured error details.
	 */
	static CommandResult Failure(CommandErrorCode errorCode, std::string errorMessage,
	                             Json errorDetails = Json::object())
	{
		CommandResult result;
		result.ok = false;
		result.code = errorCode;
		result.message = std::move(errorMessage);
		result.details = std::move(errorDetails);
		return result;
	}
};

} // namespace neurus::command
