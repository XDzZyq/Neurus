#pragma once

#include "command/CommandTypes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace neurus::command
{

/// Wire protocol major version. Additive fields are allowed within one major.
inline constexpr int kProtocolVersion = 1;

/// Transport-level failure detail produced while decoding a message.
struct CommandError
{
	CommandErrorCode code = CommandErrorCode::Internal;
	std::string      message;
};

/// A request as it appears on the wire, correlated by id.
struct WireRequest
{
	std::uint64_t  id = 0;
	CommandRequest request;
};

/// A response as it appears on the wire, echoing the request id.
struct WireResponse
{
	std::uint64_t id = 0;
	CommandResult result;
};

/**
 * @brief Encodes a request into its versioned JSON body.
 * @param request Request and correlation id.
 * @return UTF-8 JSON, without framing.
 */
std::string EncodeRequest(const WireRequest& request);

/**
 * @brief Decodes a request body.
 * @param body UTF-8 JSON body, without framing.
 * @param error Receives the failure detail when decoding fails.
 * @return The request, or std::nullopt with @p error set.
 */
std::optional<WireRequest> DecodeRequest(std::string_view body, CommandError& error);

/**
 * @brief Encodes a response into its versioned JSON body.
 * @param response Result and the id being answered.
 * @return UTF-8 JSON, without framing.
 */
std::string EncodeResponse(const WireResponse& response);

/**
 * @brief Decodes a response body.
 * @param body UTF-8 JSON body, without framing.
 * @param error Receives the failure detail when decoding fails.
 * @return The response, or std::nullopt with @p error set.
 */
std::optional<WireResponse> DecodeResponse(std::string_view body, CommandError& error);

} // namespace neurus::command
