/**
 * @file test_dispatcher.cpp
 * @brief Tests for the bounded main-thread command dispatcher.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "command/CommandDispatcher.h"
#include "command/Commands.h"

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

	std::thread worker([&]
	{
		ASSERT_TRUE(dispatcher.Post(1, {kSessionStatus, {}},
		                            [](std::uint64_t, const CommandResult&) {}));
	});
	worker.join();

	EXPECT_EQ(executor.calls.load(), 0);
	EXPECT_EQ(dispatcher.Drain(executor), 1u);
	EXPECT_EQ(executor.calls.load(), 1);
	EXPECT_EQ(executor.lastThread, mainThread);
}

TEST(CommandDispatcher, DrainIsBoundedToBatchSize)
{
	CommandDispatcher dispatcher;
	RecordingExecutor executor;
	for (std::uint64_t i = 0; i < kCommandDrainBatch + 5; ++i)
	{
		ASSERT_TRUE(dispatcher.Post(i, {kSessionStatus, {}},
		                            [](std::uint64_t, const CommandResult&) {}));
	}

	EXPECT_EQ(dispatcher.Drain(executor), kCommandDrainBatch);
	EXPECT_EQ(dispatcher.PendingCount(), 5u);
	EXPECT_EQ(dispatcher.Drain(executor), 5u);
	EXPECT_EQ(dispatcher.PendingCount(), 0u);
}

TEST(CommandDispatcher, FullQueueRejects)
{
	CommandDispatcher dispatcher;
	for (std::size_t i = 0; i < kCommandQueueCapacity; ++i)
	{
		ASSERT_TRUE(dispatcher.Post(i, {kSessionStatus, {}},
		                            [](std::uint64_t, const CommandResult&) {}));
	}
	EXPECT_FALSE(dispatcher.Post(999, {kSessionStatus, {}},
	                             [](std::uint64_t, const CommandResult&) {}));
}

TEST(CommandDispatcher, CompletionReceivesIdAndResult)
{
	CommandDispatcher dispatcher;
	RecordingExecutor executor;
	std::uint64_t seenId = 0;
	CommandResult seen = CommandResult::Failure(CommandErrorCode::Internal, "unset");

	ASSERT_TRUE(dispatcher.Post(42, {kSessionStatus, {}},
	                            [&](std::uint64_t id, const CommandResult& result)
	                            {
		                            seenId = id;
		                            seen = result;
	                            }));
	dispatcher.Drain(executor);

	EXPECT_EQ(seenId, 42u);
	EXPECT_TRUE(seen.ok);
	EXPECT_EQ(seen.details["ok"], true);
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
