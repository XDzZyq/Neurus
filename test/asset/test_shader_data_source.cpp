#include "asset/data/ShaderData.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace neurus {
namespace {

class ShaderDataSourceTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		m_path = std::filesystem::temp_directory_path()
		       / ("neurus-shader-source-" + std::to_string(stamp) + ".glsl");
	}

	void TearDown() override
	{
		std::error_code error;
		std::filesystem::remove(m_path, error);
		EXPECT_FALSE(error) << error.message();
	}

	std::filesystem::path m_path;
};

TEST_F(ShaderDataSourceTest, MissingFileReportsTheRequestedPath)
{
	std::string error;
	EXPECT_FALSE(ShaderData::ReadSource(m_path.string(), &error));
	EXPECT_NE(error.find("cannot open file"), std::string::npos);
	EXPECT_NE(error.find(m_path.filename().string()), std::string::npos);
}

TEST_F(ShaderDataSourceTest, EmptyFileIsReportedRatherThanParsed)
{
	{ std::ofstream file(m_path); }
	std::string error;
	EXPECT_FALSE(ShaderData::ReadSource(m_path.string(), &error));
	EXPECT_NE(error.find("file is empty"), std::string::npos);
}

TEST_F(ShaderDataSourceTest, SourceTextIsPreservedAndPreviousErrorCleared)
{
	const std::string text = "#version 450\nvoid main() {}\n";
	{ std::ofstream file(m_path); file << text; }
	std::string error = "previous failure";
	const auto source = ShaderData::ReadSource(m_path.string(), &error);
	ASSERT_TRUE(source);
	EXPECT_EQ(*source, text);
	EXPECT_TRUE(error.empty());
}

} // namespace
} // namespace neurus
