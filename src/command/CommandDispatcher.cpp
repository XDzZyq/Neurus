#include "command/CommandDispatcher.h"

#include <exception>
#include <utility>
#include <vector>

namespace neurus::command
{

bool CommandDispatcher::Post(std::uint64_t id, CommandRequest request, CommandCompletion completion)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_pending.size() >= kCommandQueueCapacity)
		return false;

	m_pending.push_back(PendingCommand{id, std::move(request), std::move(completion)});
	return true;
}

std::size_t CommandDispatcher::Drain(ICommandExecutor& executor)
{
	std::vector<PendingCommand> batch;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		const std::size_t count = m_pending.size() < kCommandDrainBatch
			? m_pending.size()
			: kCommandDrainBatch;
		batch.reserve(count);
		for (std::size_t i = 0; i < count; ++i)
		{
			batch.push_back(std::move(m_pending.front()));
			m_pending.pop_front();
		}
	}

	for (auto& pending : batch)
	{
		CommandResult result;
		try
		{
			result = executor.Execute(pending.request);
		}
		catch (const std::exception& e)
		{
			result = CommandResult::Failure(CommandErrorCode::Internal, e.what());
		}
		catch (...)
		{
			result = CommandResult::Failure(CommandErrorCode::Internal, "Unknown command failure.");
		}

		if (pending.completion)
			pending.completion(pending.id, result);
	}
	return batch.size();
}

std::size_t CommandDispatcher::PendingCount() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_pending.size();
}

} // namespace neurus::command
