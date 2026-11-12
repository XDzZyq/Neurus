#include "command/CommandJson.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace neurus::command
{
namespace
{

void SetError(CommandError& error, CommandErrorCode code, std::string message)
{
	error.code = code;
	error.message = std::move(message);
}

bool ReadVersion(const Json& json, CommandError& error)
{
	if (!json.is_object())
	{
		SetError(error, CommandErrorCode::FrameInvalid, "Message is not a JSON object.");
		return false;
	}

	const auto versionIt = json.find("v");
	if (versionIt == json.end() || !versionIt->is_number_integer())
	{
		SetError(error, CommandErrorCode::InvalidArgument, "Missing protocol version 'v'.");
		return false;
	}
	if (versionIt->get<int>() != kProtocolVersion)
	{
		SetError(error, CommandErrorCode::ProtocolVersionMismatch,
		         "Unsupported protocol version " + versionIt->dump() + ".");
		return false;
	}
	return true;
}

bool ReadId(const Json& json, std::uint64_t& id, CommandError& error)
{
	if (!json.contains("id") || !json["id"].is_number_unsigned())
	{
		SetError(error, CommandErrorCode::InvalidArgument, "Missing unsigned request id.");
		return false;
	}
	id = json["id"].get<std::uint64_t>();
	return true;
}

} // namespace

std::string EncodeRequest(const WireRequest& request)
{
	Json json;
	json["v"] = kProtocolVersion;
	json["id"] = request.id;
	json["command"] = request.request.name;
	json["args"] = request.request.args.is_null() ? Json::object() : request.request.args;
	return json.dump();
}

std::optional<WireRequest> DecodeRequest(std::string_view body, CommandError& error)
{
	Json json;
	try
	{
		json = Json::parse(body);
	}
	catch (const nlohmann::json::exception& e)
	{
		SetError(error, CommandErrorCode::FrameInvalid, e.what());
		return std::nullopt;
	}

	WireRequest request;
	if (!ReadVersion(json, error) || !ReadId(json, request.id, error))
		return std::nullopt;

	const auto commandIt = json.find("command");
	if (commandIt == json.end() || !commandIt->is_string() || commandIt->get<std::string>().empty())
	{
		SetError(error, CommandErrorCode::InvalidArgument, "Missing command name.");
		return std::nullopt;
	}

	request.request.name = commandIt->get<std::string>();
	if (const auto argsIt = json.find("args"); argsIt != json.end())
	{
		if (argsIt->is_null())
			request.request.args = Json::object();
		else if (!argsIt->is_object())
		{
			SetError(error, CommandErrorCode::InvalidArgument, "Command arguments must be an object.");
			return std::nullopt;
		}
		else
			request.request.args = *argsIt;
	}
	return request;
}

std::string EncodeResponse(const WireResponse& response)
{
	Json json;
	json["v"] = kProtocolVersion;
	json["id"] = response.id;
	json["ok"] = response.result.ok;
	if (response.result.ok)
	{
		json["result"] = response.result.details;
	}
	else
	{
		json["error"] = {
			{"code", std::string(ToString(response.result.code))},
			{"message", response.result.message},
			{"details", response.result.details},
		};
	}
	return json.dump();
}

std::optional<WireResponse> DecodeResponse(std::string_view body, CommandError& error)
{
	Json json;
	try
	{
		json = Json::parse(body);
	}
	catch (const nlohmann::json::exception& e)
	{
		SetError(error, CommandErrorCode::FrameInvalid, e.what());
		return std::nullopt;
	}

	WireResponse response;
	if (!ReadVersion(json, error) || !ReadId(json, response.id, error))
		return std::nullopt;

	const auto okIt = json.find("ok");
	if (okIt == json.end() || !okIt->is_boolean())
	{
		SetError(error, CommandErrorCode::InvalidArgument, "Missing response status 'ok'.");
		return std::nullopt;
	}

	if (okIt->get<bool>())
	{
		const auto resultIt = json.find("result");
		response.result = CommandResult::Success(
			resultIt != json.end() && resultIt->is_object() ? *resultIt : Json::object());
		return response;
	}

	const auto errorIt = json.find("error");
	if (errorIt == json.end() || !errorIt->is_object())
	{
		SetError(error, CommandErrorCode::FrameInvalid, "Missing error object.");
		return std::nullopt;
	}

	const auto codeIt = errorIt->find("code");
	const auto code = codeIt != errorIt->end() && codeIt->is_string()
		? ParseErrorCode(codeIt->get<std::string>())
		: std::nullopt;
	response.result.ok = false;
	response.result.code = code.value_or(CommandErrorCode::Internal);
	const auto messageIt = errorIt->find("message");
	response.result.message = messageIt != errorIt->end() && messageIt->is_string()
		? messageIt->get<std::string>()
		: std::string{};
	const auto detailsIt = errorIt->find("details");
	response.result.details = detailsIt != errorIt->end() && detailsIt->is_object()
		? *detailsIt
		: Json::object();
	return response;
}

} // namespace neurus::command
