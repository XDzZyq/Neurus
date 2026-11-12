#include "cli/AppArguments.h"

namespace neurus::cli
{

std::vector<std::string> BuildAppArguments(const std::string& session,
                                          const std::optional<std::string>& project)
{
	std::vector<std::string> arguments{"--session", session};
	if (project)
	{
		arguments.push_back("--project");
		arguments.push_back(*project);
	}
	return arguments;
}

} // namespace neurus::cli
