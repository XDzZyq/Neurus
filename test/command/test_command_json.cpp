/**
 * @file test_command_json.cpp
 * @brief Wire codec tests for the command protocol.
 */

#include <gtest/gtest.h>

#include "command/CommandJson.h"
#include "command/Commands.h"

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
	auto decoded = DecodeRequest(text, error);
	ASSERT_TRUE(decoded.has_value()) << error.message;
	EXPECT_EQ(decoded->id, 7u);
	EXPECT_EQ(decoded->request.name, "session.close");
	EXPECT_TRUE(decoded->request.args["save"].get<bool>());
}

TEST(CommandJson, MissingCommandIsInvalidArgument)
{
	CommandError error{};
	EXPECT_FALSE(DecodeRequest(R"({"v":1,"id":1,"args":{}})", error).has_value());
	EXPECT_EQ(error.code, CommandErrorCode::InvalidArgument);
}

TEST(CommandJson, WrongVersionIsAMismatch)
{
	CommandError error{};
	EXPECT_FALSE(DecodeRequest(R"({"v":99,"id":1,"command":"session.status"})", error).has_value());
	EXPECT_EQ(error.code, CommandErrorCode::ProtocolVersionMismatch);
}

TEST(CommandJson, MalformedBodyIsFrameInvalid)
{
	CommandError error{};
	EXPECT_FALSE(DecodeRequest("{not json", error).has_value());
	EXPECT_EQ(error.code, CommandErrorCode::FrameInvalid);
}

TEST(CommandJson, UnknownFieldsAreIgnored)
{
	nlohmann::json json = {{"v", kProtocolVersion}, {"id", 2},
	                       {"command", "session.status"},
	                       {"args", nlohmann::json::object()}, {"future", true}};
	CommandError error{};
	EXPECT_TRUE(DecodeRequest(json.dump(), error).has_value());
}

TEST(CommandJson, ErrorCodeStringRoundTrips)
{
	auto text = EncodeResponse({3, CommandResult::Failure(CommandErrorCode::DirtyProject, "dirty")});
	auto parsed = nlohmann::json::parse(text);
	EXPECT_EQ(parsed["ok"], false);
	EXPECT_EQ(parsed["error"]["code"], "DIRTY_PROJECT");

	CommandError error{};
	auto decoded = DecodeResponse(text, error);
	ASSERT_TRUE(decoded.has_value()) << error.message;
	EXPECT_EQ(decoded->id, 3u);
	EXPECT_FALSE(decoded->result.ok);
	EXPECT_EQ(decoded->result.code, CommandErrorCode::DirtyProject);
	EXPECT_EQ(decoded->result.message, "dirty");
}

TEST(CommandJson, SuccessResponseCarriesResultObject)
{
	auto text = EncodeResponse({4, CommandResult::Success({{"session", "build-1"}})});
	CommandError error{};
	auto decoded = DecodeResponse(text, error);
	ASSERT_TRUE(decoded.has_value()) << error.message;
	EXPECT_TRUE(decoded->result.ok);
	EXPECT_EQ(decoded->result.details["session"], "build-1");
}
