#pragma once

#include "command/CommandTypes.h"
#include "command/ICommandExecutor.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>

namespace neurus::command
{

/// Maximum number of requests waiting for the main thread.
inline constexpr std::size_t kCommandQueueCapacity = 256;

/// Maximum number of requests executed in one frame tick.
inline constexpr std::size_t kCommandDrainBatch = 16;

/// Receives a result on the thread that called Drain().
using CommandCompletion = std::function<void(std::uint64_t id, const CommandResult& result)>;

/**
 * @brief Thread-safe bridge between IPC threads and the application main thread.
 *
 * The IPC layer calls Post(); the application calls Drain() once per frame tick.
 * Execute() is invoked only from Drain(), so editor state is never touched from
 * an I/O thread.
 */
class CommandDispatcher
{
public:
	CommandDispatcher() = default;
	CommandDispatcher(const CommandDispatcher&) = delete;
	CommandDispatcher& operator=(const CommandDispatcher&) = delete;

	/**
	 * @brief Queues one request.
	 * @param id Correlation id echoed to the caller.
	 * @param request Command to execute later.
	 * @param completion Invoked on the draining thread with the result.
	 * @return False when the queue is full; the caller should answer SERVER_BUSY.
	 */
	bool Post(std::uint64_t id, CommandRequest request, CommandCompletion completion);

	/**
	 * @brief Executes at most kCommandDrainBatch queued requests.
	 * @param executor Executor called on the calling thread.
	 * @return Number of requests executed.
	 */
	std::size_t Drain(ICommandExecutor& executor);

	/// @return Number of queued requests.
	std::size_t PendingCount() const;

private:
	struct PendingCommand
	{
		std::uint64_t      id;
		CommandRequest     request;
		CommandCompletion  completion;
	};

	mutable std::mutex          m_mutex;
	std::deque<PendingCommand>  m_pending;
};

} // namespace neurus::command
