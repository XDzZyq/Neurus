#include "app/AppArgs.h"

namespace neurus
{
namespace
{

bool TakeValue(const std::vector<std::string>& arguments, std::size_t& index,
               const char* name, std::optional<std::string>& value, std::string& error)
{
	const std::string& argument = arguments[index];
	const std::string option = name;
	if (argument == option)
	{
		if (index + 1 >= arguments.size())
		{
			error = std::string("Missing value for ") + name + ".";
			return true;
		}
		value = arguments[++index];
		return true;
	}

	const std::string prefix = option + "=";
	if (argument.rfind(prefix, 0) == 0)
	{
		value = argument.substr(prefix.size());
		return true;
	}
	return false;
}

} // namespace

AppArgs AppArgs::Parse(const std::vector<std::string>& arguments)
{
	AppArgs result;
	for (std::size_t index = 1; index < arguments.size(); ++index)
	{
		if (TakeValue(arguments, index, "--session", result.session, result.error))
		{
			if (!result.error.empty())
				break;
			continue;
		}
		if (TakeValue(arguments, index, "--project", result.project, result.error))
		{
			if (!result.error.empty())
				break;
			continue;
		}
		// Unknown arguments belong to Qt or to a future option; ignore them.
	}
	return result;
}

AppArgs AppArgs::Parse(int argc, char* argv[])
{
	std::vector<std::string> arguments;
	arguments.reserve(static_cast<std::size_t>(argc));
	for (int i = 0; i < argc; ++i)
		arguments.emplace_back(argv[i] ? argv[i] : "");
	return Parse(arguments);
}

} // namespace neurus
