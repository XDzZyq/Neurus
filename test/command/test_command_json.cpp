/**
 * @file test_command_json.cpp
 * @brief Wire codec tests for the command protocol.
 */

#include <gtest/gtest.h>

#include "command/CommandJson.hpp"
#include "command/Commands.hpp"

using namespace neurus::command;

TEST(CommandJson, RequestRoundTripsWithIdAndVersion)
{
	auto text = EncodeRequest({7, {kSessionClose, {{"save", true}, {"force", false}}}});
	auto parsed = nlohmann::json::parse(text);
	EXPECT_EQ(parsed["v"], kProtocolVersion);
	EXPECT_EQ(parsed["id"], 7);
	EXPECT_EQ(parsed["command"], "session.close");
	EXPECT_EQ(parsed["args"]["save"], true);

	CommandError error{};
	parsed["future"] = true;
	auto decoded = DecodeRequest(parsed.dump(), error);
	ASSERT_TRUE(decoded.has_value()) << error.message;
	EXPECT_EQ(decoded->id, 7u);
	EXPECT_EQ(decoded->request.name, "session.close");
	EXPECT_TRUE(decoded->request.args["save"].get<bool>());
}

TEST(CommandJson, InvalidRequestReportsTheProtocolError)
{
	struct InvalidRequest
	{
		const char* text;
		CommandErrorCode expected;
	};
	const InvalidRequest cases[] = {
		{R"({"v":1,"id":1,"args":{}})", CommandErrorCode::InvalidArgument},
		{R"({"v":99,"id":1,"command":"session.status"})", CommandErrorCode::ProtocolVersionMismatch},
		{"{not json", CommandErrorCode::FrameInvalid}
	};
	for (const auto& request : cases)
	{
		SCOPED_TRACE(request.text);
		CommandError error{};
		EXPECT_FALSE(DecodeRequest(request.text, error).has_value());
		EXPECT_EQ(error.code, request.expected);
	}
}

TEST(CommandJson, ResponsesRoundTripTheirIdsAndPayloads)
{
	const CommandResult results[] = {
		CommandResult::Success({{"session", "build-1"}}),
		CommandResult::Failure(CommandErrorCode::DirtyProject, "dirty")
	};
	for (const auto& result : results)
	{
		SCOPED_TRACE(result.ok ? "success" : "failure");
		const auto text = EncodeResponse({3, result});
		const auto wire = nlohmann::json::parse(text);
		EXPECT_EQ(wire["ok"], result.ok);
		if (!result.ok)
			EXPECT_EQ(wire["error"]["code"], "DIRTY_PROJECT");

		CommandError error{};
		const auto decoded = DecodeResponse(text, error);
		ASSERT_TRUE(decoded.has_value()) << error.message;
		EXPECT_EQ(decoded->id, 3u);
		EXPECT_EQ(decoded->result.ok, result.ok);
		EXPECT_EQ(decoded->result.code, result.code);
		EXPECT_EQ(decoded->result.message, result.message);
		EXPECT_EQ(decoded->result.details, result.details);
	}
}
