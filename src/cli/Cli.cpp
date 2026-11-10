#include "cli/Cli.h"

#include "cli/SubCommand.h"
#include "cli/subcommands/CloseCommand.h"
#include "cli/subcommands/ListCommand.h"
#include "cli/subcommands/OpenCommand.h"
#include "cli/subcommands/StatusCommand.h"

#include <CLI/CLI.hpp>

#include <memory>
#include <string>
#include <vector>

namespace neurus::cli
{

int RunCli(int argc, char* argv[])
{
	CLI::App app{"Neurus command-line session control"};
	app.set_version_flag("--version", "0.1.0");
	std::string globalFormat;
	AddFormatOption(app, globalFormat);
	app.require_subcommand(1);

	std::vector<std::unique_ptr<SubCommand>> commands;
	commands.push_back(std::make_unique<OpenCommand>());
	commands.push_back(std::make_unique<CloseCommand>());
	commands.push_back(std::make_unique<StatusCommand>());
	commands.push_back(std::make_unique<ListCommand>());
	for (auto& command : commands)
		command->Register(app, globalFormat);

	try
	{
		app.parse(argc, argv);
	}
	catch (const CLI::ParseError& error)
	{
		return app.exit(error);
	}

	for (auto& command : commands)
	{
		if (command->Selected())
			return static_cast<int>(command->Execute());
	}
	return static_cast<int>(CliExit::Usage);
}

} // namespace neurus::cli
