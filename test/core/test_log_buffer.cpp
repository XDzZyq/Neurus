#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>

#include "core/Log.hpp"

namespace neurus
{

class LogBufferTest : public ::testing::Test
{
protected:
	void SetUp() override { LogBuffer::instance().Clear(); }
	void TearDown() override { LogBuffer::instance().Clear(); }
};

TEST_F(LogBufferTest, AtReturnsEntriesInOrder)
{
	auto& buf = LogBuffer::instance();
	buf.Append(LogEntry{LogLevel::Info, {}, "f", 1, "first", 0});
	buf.Append(LogEntry{LogLevel::Error, {}, "f", 2, "second", 0});
	EXPECT_EQ(buf.At(0).message, "first");
	EXPECT_EQ(buf.At(1).message, "second");
	EXPECT_EQ(std::string(buf.At(0).func), "f");
	EXPECT_EQ(buf.At(1).line, 2);
	EXPECT_EQ(buf.InfoCount(), 1u);
	EXPECT_EQ(buf.ErrorCount(), 1u);
	EXPECT_EQ(buf.At(0).seq, 1u);
	EXPECT_EQ(buf.At(1).seq, 2u);
	EXPECT_EQ(buf.HeadSeq(), buf.At(1).seq);
	buf.Clear();
	EXPECT_EQ(buf.Size(), 0u);
	EXPECT_EQ(buf.InfoCount(), 0u);
	EXPECT_EQ(buf.ErrorCount(), 0u);
	EXPECT_EQ(buf.HeadSeq(), 0u);
}

TEST_F(LogBufferTest, RingDropsOldestWhenFull)
{
	auto& buf = LogBuffer::instance();
	for (std::size_t i = 0; i < kLogCapacity + 5; ++i)
		buf.Append(LogEntry{LogLevel::Info, {}, "f", static_cast<int>(i), "m", 0});
	EXPECT_EQ(buf.Size(), kLogCapacity);
	EXPECT_EQ(buf.At(0).line, 5); // oldest 5 dropped
	EXPECT_EQ(buf.InfoCount(), kLogCapacity);
	buf.Append(LogEntry{LogLevel::Error, {}, "f", 0, "error", 0});
	EXPECT_EQ(buf.Size(), kLogCapacity);
	EXPECT_EQ(buf.InfoCount(), kLogCapacity - 1);
	EXPECT_EQ(buf.ErrorCount(), 1u);
}

TEST_F(LogBufferTest, ThreadSafeAppend)
{
	auto& buf = LogBuffer::instance();
	constexpr int kThreads = 2;
	constexpr int kPerThread = 1000;
	std::vector<std::thread> threads;
	for (int t = 0; t < kThreads; ++t)
		threads.emplace_back([&buf]() {
			for (int i = 0; i < kPerThread; ++i)
				buf.Append(LogEntry{LogLevel::Info, {}, "f", i, "m", 0});
		});
	for (auto& th : threads)
		th.join();
	EXPECT_EQ(buf.Size(), static_cast<std::size_t>(kThreads * kPerThread));
	EXPECT_EQ(buf.InfoCount(), static_cast<std::size_t>(kThreads * kPerThread));
	EXPECT_EQ(buf.HeadSeq(), static_cast<uint64_t>(kThreads * kPerThread));
}

} // namespace neurus
