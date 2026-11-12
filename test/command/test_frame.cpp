/**
 * @file test_frame.cpp
 * @brief Length-prefixed framing tests.
 */

#include <gtest/gtest.h>

#include <string>

#include "ipc/Frame.h"

using namespace neurus::ipc;

TEST(Frame, EncodesBigEndianLength)
{
	const auto frame = EncodeFrame("abc");
	ASSERT_EQ(frame.size(), 7u);
	EXPECT_EQ(static_cast<unsigned char>(frame[0]), 0u);
	EXPECT_EQ(static_cast<unsigned char>(frame[1]), 0u);
	EXPECT_EQ(static_cast<unsigned char>(frame[2]), 0u);
	EXPECT_EQ(static_cast<unsigned char>(frame[3]), 3u);
	EXPECT_EQ(frame.substr(4), "abc");
}

TEST(Frame, ReassemblesByteByByte)
{
	FrameDecoder decoder(1024);
	const auto frame = EncodeFrame("hello");
	std::string body;
	for (std::size_t i = 0; i < frame.size(); ++i)
	{
		decoder.Append(std::string_view(frame).substr(i, 1));
		const auto status = decoder.Next(body);
		if (i + 1 < frame.size())
			EXPECT_EQ(status, FrameStatus::NeedMore);
		else
			EXPECT_EQ(status, FrameStatus::Ok);
	}

	EXPECT_EQ(body, "hello");
	EXPECT_EQ(decoder.Buffered(), 0u);
}

TEST(Frame, TwoCoalescedFrames)
{
	FrameDecoder decoder(1024);
	decoder.Append(EncodeFrame("one") + EncodeFrame("two"));

	std::string body;
	ASSERT_EQ(decoder.Next(body), FrameStatus::Ok);
	EXPECT_EQ(body, "one");
	ASSERT_EQ(decoder.Next(body), FrameStatus::Ok);
	EXPECT_EQ(body, "two");
	EXPECT_EQ(decoder.Next(body), FrameStatus::NeedMore);
}

TEST(Frame, OversizeBodyIsTooLarge)
{
	FrameDecoder decoder(16);
	decoder.Append(EncodeFrame(std::string(17, 'x')));
	std::string body;
	EXPECT_EQ(decoder.Next(body), FrameStatus::TooLarge);
	EXPECT_EQ(decoder.Buffered(), 0u);
}

TEST(Frame, ZeroLengthIsInvalid)
{
	FrameDecoder decoder(16);
	decoder.Append(std::string("\0\0\0\0", 4));
	std::string body;
	EXPECT_EQ(decoder.Next(body), FrameStatus::Invalid);
}
