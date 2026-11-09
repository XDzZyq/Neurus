/**
 * @file test_dependencies.cpp
 * @brief Compile and link smoke test for the header-only command dependencies.
 */

#include <gtest/gtest.h>

#include <asio.hpp>
#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

TEST(CommandDependencies, HeadersCompileAndLink)
{
	asio::io_context io;
	CLI::App app;
	nlohmann::json json = {{"ok", true}};

	EXPECT_EQ(json["ok"], true);
	EXPECT_FALSE(io.stopped());
	EXPECT_EQ(app.get_name(), "");
}
