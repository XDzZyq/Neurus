#include "cli/ListCommand.h"

#include "cli/Output.h"
#include "cli/SessionClient.h"

namespace neurus::cli
{

CliExit RunList(const ListOptions& options)
{
	PrintSessions(LiveSessions(), options.format);
	return CliExit::Ok;
}

} // namespace neurus::cli
