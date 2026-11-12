#include "cli/Cli.h"

#include "cli/SubCommand.h"
#include "cli/subcommands/CloseCommand.h"
#include "cli/subcommands/HistoryCommand.h"
#include "cli/subcommands/ListCommand.h"
#include "cli/subcommands/ObjectCommand.h"
#include "cli/subcommands/OpenCommand.h"
#include "cli/subcommands/ProjectCommand.h"
#include "cli/subcommands/RenderCommand.h"
#include "cli/subcommands/SceneCommand.h"
#include "cli/subcommands/ScriptCommand.h"
#include "cli/subcommands/SnapshotCommand.h"
#include "cli/subcommands/StatusCommand.h"
#include "cli/subcommands/UseCommand.h"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace neurus::cli
{

int RunCli(const std::vector<std::string>& arguments)
{
	CLI::App app{"Neurus command-line session control"};
	app.name(arguments.empty() ? "neurus" : arguments.front());
	app.set_version_flag("--version", "0.1.0");
	std::string globalFormat;
	AddFormatOption(app, globalFormat);
	std::string globalSession;
	AddSessionOption(app, globalSession);
	app.require_subcommand(1);

	std::vector<std::unique_ptr<SubCommand>> commands;
	commands.push_back(std::make_unique<OpenCommand>());
	commands.push_back(std::make_unique<CloseCommand>());
	commands.push_back(std::make_unique<StatusCommand>());
	commands.push_back(std::make_unique<ListCommand>());
	commands.push_back(std::make_unique<ProjectCommand>());
	commands.push_back(std::make_unique<SceneCommand>());
	commands.push_back(std::make_unique<ObjectCommand>());
	commands.push_back(std::make_unique<RenderCommand>());
	commands.push_back(std::make_unique<HistoryCommand>());
	commands.push_back(std::make_unique<ScriptCommand>());
	commands.push_back(std::make_unique<SnapshotCommand>());
	commands.push_back(std::make_unique<UseCommand>());
	const GlobalOptions globals{globalFormat, globalSession};
	try
	{
		for (auto& command : commands)
			command->Register(app, globals);
	}
	catch (const std::exception& error)
	{
		std::cerr << "neurus: " << error.what() << '\n';
		return 1;
	}

	try
	{
		// CLI11's vector form expects the arguments reversed and without the
		// program name; the argv form builds exactly this shape internally.
		std::vector<std::string> parseArgs(arguments.begin() + (arguments.empty() ? 0 : 1),
		                                   arguments.end());
		std::reverse(parseArgs.begin(), parseArgs.end());
		app.parse(std::move(parseArgs));
	}
	catch (const CLI::ParseError& error)
	{
		// app.exit() prints the message (or help) and returns CLI11's own code;
		// map every failure onto the documented usage exit code.
		const int code = app.exit(error);
		return code == 0 ? static_cast<int>(CliExit::Ok) : static_cast<int>(CliExit::Usage);
	}

	try
	{
		for (auto& command : commands)
		{
			if (command->Selected())
				return static_cast<int>(command->Execute());
		}
	}
	catch (const std::exception& error)
	{
		std::cerr << "neurus: " << error.what() << '\n';
		return 1;
	}
	catch (...)
	{
		std::cerr << "neurus: unknown failure\n";
		return 1;
	}
	return static_cast<int>(CliExit::Usage);
}

} // namespace neurus::cli
