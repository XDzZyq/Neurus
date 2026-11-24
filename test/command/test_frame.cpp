/**
 * @file test_frame.cpp
 * @brief Length-prefixed framing tests.
 */

#include <gtest/gtest.h>

#include <string>

#include "ipc/Frame.hpp"

using namespace neurus::ipc;

TEST(Frame, DecodesAStreamAcrossFragmentedAndCoalescedWrites)
{
	FrameDecoder decoder(1024);
	const auto first = EncodeFrame("hello");
	ASSERT_EQ(first.substr(0, 4), std::string("\0\0\0\5", 4));
	EXPECT_EQ(first.substr(4), "hello");

	std::string body;
	for (std::size_t i = 0; i + 1 < first.size(); ++i)
	{
		SCOPED_TRACE(i);
		decoder.Append(std::string_view(first).substr(i, 1));
		EXPECT_EQ(decoder.Next(body), FrameStatus::NeedMore);
	}

	decoder.Append(first.substr(first.size() - 1) + EncodeFrame("world"));
	ASSERT_EQ(decoder.Next(body), FrameStatus::Ok);
	EXPECT_EQ(body, "hello");
	ASSERT_EQ(decoder.Next(body), FrameStatus::Ok);
	EXPECT_EQ(body, "world");
	EXPECT_EQ(decoder.Next(body), FrameStatus::NeedMore);
	EXPECT_EQ(decoder.Buffered(), 0u);
}

TEST(Frame, RejectsInvalidFrameLengthsAndDiscardsBufferedData)
{
	struct InvalidFrame
	{
		std::string bytes;
		FrameStatus status;
	};
	const InvalidFrame cases[] = {
		{std::string(4, '\0'), FrameStatus::Invalid},
		{EncodeFrame(std::string(17, 'x')), FrameStatus::TooLarge}
	};
	for (const auto& frame : cases)
	{
		SCOPED_TRACE(frame.bytes.size());
		FrameDecoder decoder(16);
		decoder.Append(frame.bytes);
		std::string body;
		EXPECT_EQ(decoder.Next(body), frame.status);
		EXPECT_EQ(decoder.Buffered(), 0u);
	}
}
