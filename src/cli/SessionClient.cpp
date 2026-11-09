#include "cli/SessionClient.h"

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
