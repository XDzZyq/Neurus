#include "cli/SessionClient.h"

#include "cli/TerminalBinding.h"
#include "ipc/IpcClient.h"

#include <chrono>
#include <utility>

namespace neurus::cli
{

std::vector<ipc::SessionRecord> LiveSessions()
{
	ipc::SessionRegistry registry(ipc::ResolveSessionsDirectory());
	return registry.ListLive();
}

std::optional<std::string> ResolveSessionNameForCommand(
	const std::optional<std::string>& explicitName,
	const std::vector<ipc::SessionRecord>& live)
{
	if (explicitName && !explicitName->empty())
		return explicitName;
	if (const auto environment = EnvironmentSession())
		return environment;

	const auto bound = ReadTerminalBinding();
	if (!bound)
		return std::nullopt;
	for (const auto& record : live)
	{
		if (record.session == *bound)
			return bound;
	}

	// The bound session is gone: clear the stale binding and fall back to the
	// normal "only live session" rule.
	std::string error;
	(void)ClearTerminalBinding(error);
	return std::nullopt;
}

std::optional<command::CommandResult> SendCommand(const ipc::SessionRecord& record,
                                                  const command::CommandRequest& request,
                                                  std::uint64_t id,
                                                  command::CommandError& error,
                                                  int timeoutSeconds)
{
	ipc::IpcClient client(ipc::IpcClientOptions{
		record.port, record.token, std::chrono::seconds(timeoutSeconds)});
	return client.Send(request, id, error);
}

} // namespace neurus::cli
