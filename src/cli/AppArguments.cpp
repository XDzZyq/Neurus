#include "cli/AppArguments.h"

namespace neurus::cli
{

std::vector<std::string> BuildAppArguments(
	const std::string& session,
	const std::optional<std::string>& project,
	const std::vector<std::string>& preload)
{
	std::vector<std::string> arguments{"--session", session};
	if (project)
	{
		arguments.push_back("--project");
		arguments.push_back(*project);
	}
	for (const std::string& package : preload)
	{
		arguments.push_back("--preload");
		arguments.push_back(package);
	}
	return arguments;
}

} // namespace neurus::cli
