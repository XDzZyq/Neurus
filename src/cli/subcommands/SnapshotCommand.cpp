#include "cli/subcommands/SnapshotCommand.h"

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

void PrintSnapshot(const command::Json& snapshot, OutputFormat format)
{
	if (format == OutputFormat::Json)
	{
		const command::Json json{{"ok", true}, {"snapshot", snapshot}};
		std::cout << json.dump(2) << '\n';
		return;
	}

	const bool all = snapshot.value("all", false);
	const std::string path = snapshot.value("path", std::string{});
	if (!all)
	{
		std::cout << "saved " << path << '\n';
		return;
	}

	std::cout << "saved " << snapshot.value("count", 0) << " file(s) under " << path << '\n';
	for (const auto& file : snapshot["files"])
		std::cout << "  " << file.get<std::string>() << '\n';
}

} // namespace

void SnapshotCommand::Register(CLI::App& parent, const GlobalOptions& globals)
{
	SetGlobals(globals);
	m_app = parent.add_subcommand("snapshot", "Capture the viewport or render attachments.");
	m_app->require_subcommand(1);

	m_captureApp = m_app->add_subcommand("capture", "Write a snapshot to disk.");
	m_captureApp->add_option("--path", m_path,
	                         "PNG file for the viewport, or directory for --all.");
	m_captureApp->add_flag("--all", m_all, "Capture every attachment, not just the viewport.");
	AddSessionOption(*m_captureApp, m_session);
	AddFormatOption(*m_captureApp, m_format);
	m_captureApp->add_option("--timeout", m_timeoutSeconds, "Command timeout in seconds.")
		->check(CLI::PositiveNumber);
}

bool SnapshotCommand::Selected() const
{
	return m_app != nullptr && m_app->parsed();
}

CliExit SnapshotCommand::Execute()
{
	if (m_captureApp == nullptr || !m_captureApp->parsed())
	{
		PrintError(OutputFormat::Human, command::CommandErrorCode::InvalidArgument,
		           "A snapshot action is required.");
		return CliExit::Usage;
	}

	const OutputFormat format = Format(m_format);
	if (m_path.empty())
	{
		PrintError(format, command::CommandErrorCode::InvalidArgument,
		           "--path is required.");
		return CliExit::Usage;
	}

	std::error_code pathError;
	const auto absolute = std::filesystem::absolute(m_path, pathError);
	const std::string path = pathError ? m_path : absolute.string();

	CliExit exitCode = CliExit::Ok;
	const auto result = RunSessionCommand(SessionName(m_session),
	                                      {command::kSnapshotCapture,
	                                       {{"path", path}, {"all", m_all}}},
	                                      format, exitCode, m_timeoutSeconds);
	if (!result)
		return exitCode;

	PrintSnapshot(result->details, format);
	return CliExit::Ok;
}

} // namespace neurus::cli
