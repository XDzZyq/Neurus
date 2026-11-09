#include "cli/Cli.h"

#include "cli/CliParse.h"
#include "cli/CloseCommand.h"
#include "cli/ListCommand.h"
#include "cli/OpenCommand.h"
#include "cli/Output.h"
#include "cli/StatusCommand.h"

#include <CLI/CLI.hpp>

#include <optional>
#include <string>

namespace neurus::cli
{
namespace
{

OutputFormat ParseFormat(const std::string& globalFormat, const std::string& localFormat)
{
	const std::string& value = globalFormat.empty() ? localFormat : globalFormat;
	return value == "json" ? OutputFormat::Json : OutputFormat::Human;
}

void AddFormatOption(CLI::App* app, std::string& storage)
{
	app->add_option("--format", storage, "Output format: human or json")
		->check(CLI::IsMember({"human", "json"}));
}

} // namespace

int RunCli(int argc, char* argv[])
{
	CLI::App app{"Neurus command-line session control"};
	app.set_version_flag("--version", "0.1.0");
	std::string globalFormat;
	AddFormatOption(&app, globalFormat);
	app.require_subcommand(1);

	// --- open ---
	auto* open = app.add_subcommand("open", "Launch a named background Neurus session.");
	std::optional<std::string> openProject;
	std::optional<std::string> openName;
	std::optional<std::string> openApp;
	int openTimeout = 30;
	bool openNoWait = false;
	std::string openFormat;
	open->add_option("project", openProject, "Project file to open.");
	open->add_option("--name", openName, "Session name; allocated when omitted.");
	open->add_option("--app", openApp, "Path to the Neurus executable.");
	open->add_option("--timeout", openTimeout, "Readiness timeout in seconds.")
		->check(CLI::PositiveNumber);
	open->add_flag("--no-wait", openNoWait, "Do not wait for readiness.");
	AddFormatOption(open, openFormat);

	// --- close ---
	auto* close = app.add_subcommand("close", "Close a running Neurus session.");
	std::optional<std::string> closeName;
	bool closeAll = false;
	bool closeSave = false;
	bool closeForce = false;
	int closeTimeout = 10;
	std::string closeFormat;
	close->add_option("name", closeName, "Session name.");
	close->add_flag("--all", closeAll, "Close every live session.");
	close->add_flag("--save", closeSave, "Save the project before closing.");
	close->add_flag("--force", closeForce, "Discard unsaved changes.");
	close->add_option("--timeout", closeTimeout, "Command timeout in seconds.")
		->check(CLI::PositiveNumber);
	AddFormatOption(close, closeFormat);

	// --- status ---
	auto* status = app.add_subcommand("status", "Report a running session.");
	std::optional<std::string> statusName;
	int statusTimeout = 10;
	std::string statusFormat;
	status->add_option("name", statusName, "Session name.");
	status->add_option("--timeout", statusTimeout, "Command timeout in seconds.")
		->check(CLI::PositiveNumber);
	AddFormatOption(status, statusFormat);

	// --- list ---
	auto* list = app.add_subcommand("list", "List live sessions in the local registry.");
	std::string listFormat;
	AddFormatOption(list, listFormat);

	try
	{
		app.parse(argc, argv);
	}
	catch (const CLI::ParseError& error)
	{
		return app.exit(error);
	}

	if (open->parsed())
	{
		OpenOptions options;
		options.project = openProject;
		options.name = openName;
		if (openApp)
			options.app = std::filesystem::path(*openApp);
		options.timeoutSeconds = openTimeout;
		options.noWait = openNoWait;
		options.format = ParseFormat(globalFormat, openFormat);
		return static_cast<int>(RunOpen(options));
	}
	if (close->parsed())
	{
		CloseOptions options;
		options.name = closeName;
		options.all = closeAll;
		options.save = closeSave;
		options.force = closeForce;
		options.timeoutSeconds = closeTimeout;
		options.format = ParseFormat(globalFormat, closeFormat);
		return static_cast<int>(RunClose(options));
	}
	if (status->parsed())
	{
		StatusOptions options;
		options.name = statusName;
		options.timeoutSeconds = statusTimeout;
		options.format = ParseFormat(globalFormat, statusFormat);
		return static_cast<int>(RunStatus(options));
	}
	if (list->parsed())
	{
		ListOptions options;
		options.format = ParseFormat(globalFormat, listFormat);
		return static_cast<int>(RunList(options));
	}

	return static_cast<int>(CliExit::Usage);
}

} // namespace neurus::cli
