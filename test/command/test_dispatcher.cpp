/**
 * @file test_dispatcher.cpp
 * @brief Tests for the bounded main-thread command dispatcher.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <thread>

#include "command/CommandDispatcher.hpp"
#include "command/Commands.hpp"

using namespace neurus::command;

namespace
{

class RecordingExecutor final : public ICommandExecutor
{
public:
	CommandResult Execute(const CommandRequest&) override
	{
		++calls;
		lastThread = std::this_thread::get_id();
		return CommandResult::Success({{"ok", true}});
	}

	std::atomic<int> calls{0};
	std::thread::id lastThread;
};

class ThrowingExecutor final : public ICommandExecutor
{
public:
	CommandResult Execute(const CommandRequest&) override
	{
		throw std::runtime_error("boom");
	}
};

} // namespace

TEST(CommandDispatcher, PostsFromWorkerAndRunsOnlyInDrain)
{
	CommandDispatcher dispatcher;
	RecordingExecutor executor;
	const auto mainThread = std::this_thread::get_id();

	bool completed = false;
	std::thread worker([&]
	{
		ASSERT_TRUE(dispatcher.Post(1, {kSessionStatus, {}},
		                            [&](std::uint64_t id, const CommandResult& result)
		                            {
			                            EXPECT_EQ(id, 1u);
			                            EXPECT_TRUE(result.ok);
			                            EXPECT_EQ(result.details["ok"], true);
			                            completed = true;
		                            }));
	});
	worker.join();

	EXPECT_EQ(executor.calls.load(), 0);
	EXPECT_EQ(dispatcher.Drain(executor), 1u);
	EXPECT_EQ(executor.calls.load(), 1);
	EXPECT_EQ(executor.lastThread, mainThread);
	EXPECT_TRUE(completed);
}

TEST(CommandDispatcher, FullQueueRejectsNewWorkUntilDrainedInBoundedBatches)
{
	CommandDispatcher dispatcher;
	for (std::size_t i = 0; i < kCommandQueueCapacity; ++i)
	{
		ASSERT_TRUE(dispatcher.Post(i, {kSessionStatus, {}},
		                            [](std::uint64_t, const CommandResult&) {}));
	}
	EXPECT_FALSE(dispatcher.Post(999, {kSessionStatus, {}},
	                             [](std::uint64_t, const CommandResult&) {}));
	RecordingExecutor executor;
	ASSERT_EQ(dispatcher.Drain(executor), kCommandDrainBatch);
	constexpr std::size_t refill = 5;
	for (std::size_t i = 0; i < refill; ++i)
	{
		ASSERT_TRUE(dispatcher.Post(kCommandQueueCapacity + i, {kSessionStatus, {}},
		                            [](std::uint64_t, const CommandResult&) {}));
	}
	for (std::size_t pending = kCommandQueueCapacity - kCommandDrainBatch + refill; pending > 0;)
	{
		const auto expected = std::min(pending, kCommandDrainBatch);
		ASSERT_EQ(dispatcher.Drain(executor), expected) << "pending before drain: " << pending;
		pending -= expected;
		EXPECT_EQ(dispatcher.PendingCount(), pending);
	}
}

TEST(CommandDispatcher, ExecutorExceptionBecomesInternalFailure)
{
	CommandDispatcher dispatcher;
	ThrowingExecutor executor;
	CommandResult seen = CommandResult::Success();

	ASSERT_TRUE(dispatcher.Post(1, {kSessionStatus, {}},
	                            [&](std::uint64_t, const CommandResult& result) { seen = result; }));
	dispatcher.Drain(executor);

	EXPECT_FALSE(seen.ok);
	EXPECT_EQ(seen.code, CommandErrorCode::Internal);
}
