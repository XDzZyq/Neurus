#include "cli/subcommands/ProjectCommand.h"

#include "cli/CommandClient.h"
#include "cli/Output.h"
#include "command/Commands.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace neurus::cli
{
namespace
{

void AddCommonOptions(CLI::App& app, std::string& session, std::string& format,
                      int& timeoutSeconds)
{
	AddSessionOption(app, session);
	AddFormatOption(app, format);
	app.add_option("--timeout", timeoutSeconds, "Command timeout in seconds.")
		->check(CLI::PositiveNumber);
}

std::string AbsolutePath(const std::string& path)
{
	if (path.empty())
		return {};
	std::error_code error;
	const auto absolute = std::filesystem::absolute(path, error);
	return error ? path : absolute.string();
}

void PrintProject(const std::string& action, const command::Json& project,
                  OutputFormat format)
{
	if (format == OutputFormat::Json)
	{
		const command::Json json{{"ok", true}, {"action", action}, {"project", project}};
		std::cout << json.dump(2) << '\n';
		return;
	}

	const std::string path = project.value("path", std::string{});
	const int objects = project.value("objects", 0);
	const bool dirty = project.value("dirty", false);
	if (action == "save")
		std::cout << "saved " << path << '\n';
	else if (action == "open")
		std::cout << "opened " << path << " objects " << objects << '\n';
	else if (action == "new")
		std::cout << "new project objects " << objects << '\n';
	else
	{
		std::cout << "project " << (path.empty() ? "(unsaved)" : path) << '\n';
		std::cout << "dirty " << (dirty ? "yes" : "no") << " objects " << objects << '\n';
	}
}

} // namespace

void ProjectCommand::Register(CLI::App& parent, const GlobalOptions& globals)
{
	SetGlobals(globals);
	m_app = parent.add_subcommand("project", "Inspect, create, open and save the project.");
	m_app->require_subcommand(1);

	m_infoApp = m_app->add_subcommand("info", "Report the current project document.");
	AddCommonOptions(*m_infoApp, m_info.session, m_info.format, m_info.timeoutSeconds);

	m_newApp = m_app->add_subcommand("new", "Replace the document with a new scene.");
	AddCommonOptions(*m_newApp, m_new.session, m_new.format, m_new.timeoutSeconds);
	m_newApp->add_flag("--force", m_newForce, "Discard unsaved changes.");

	m_openApp = m_app->add_subcommand("open", "Load a project file.");
	AddCommonOptions(*m_openApp, m_open.session, m_open.format, m_open.timeoutSeconds);
	m_openApp->add_option("--path", m_openPath, "Project file to open.");
	m_openApp->add_flag("--force", m_openForce, "Discard unsaved changes.");

	m_saveApp = m_app->add_subcommand("save", "Save the project.");
	AddCommonOptions(*m_saveApp, m_save.session, m_save.format, m_save.timeoutSeconds);
	m_saveApp->add_option("--path", m_savePath, "Save to this path instead of the current one.");
}

bool ProjectCommand::Selected() const
{
	return m_app != nullptr && m_app->parsed();
}

ProjectCommand::Action ProjectCommand::SelectedAction() const
{
	if (m_infoApp != nullptr && m_infoApp->parsed())
		return Action::Info;
	if (m_newApp != nullptr && m_newApp->parsed())
		return Action::New;
	if (m_openApp != nullptr && m_openApp->parsed())
		return Action::Open;
	if (m_saveApp != nullptr && m_saveApp->parsed())
		return Action::Save;
	return Action::None;
}

CliExit ProjectCommand::Execute()
{
	const Action action = SelectedAction();
	if (action == Action::None)
	{
		PrintError(OutputFormat::Human, command::CommandErrorCode::InvalidArgument,
		           "A project action is required.");
		return CliExit::Usage;
	}

	const CommonOptions* common = nullptr;
	command::CommandRequest request;
	switch (action)
	{
	case Action::Info:
		common = &m_info;
		request = {command::kProjectInfo, {}};
		break;
	case Action::New:
		common = &m_new;
		request = {command::kProjectNew, {{"force", m_newForce}}};
		break;
	case Action::Open:
		common = &m_open;
		if (m_openPath.empty())
		{
			PrintError(Format(common->format), command::CommandErrorCode::InvalidArgument,
			           "Pass --path with the project file to open.");
			return CliExit::Usage;
		}
		request = {command::kProjectOpen,
		           {{"path", AbsolutePath(m_openPath)}, {"force", m_openForce}}};
		break;
	case Action::Save:
		common = &m_save;
		request = {command::kProjectSave, {}};
		if (!m_savePath.empty())
			request.args["path"] = AbsolutePath(m_savePath);
		break;
	case Action::None:
		break;
	}

	const OutputFormat format = Format(common->format);
	CliExit exitCode = CliExit::Ok;
	const auto result = RunSessionCommand(SessionName(common->session), request, format,
	                                      exitCode, common->timeoutSeconds);
	if (!result)
		return exitCode;

	const char* name = action == Action::Info ? "info"
		: action == Action::New ? "new"
		: action == Action::Open ? "open"
		: "save";
	PrintProject(name, result->details, format);
	return CliExit::Ok;
}

} // namespace neurus::cli
