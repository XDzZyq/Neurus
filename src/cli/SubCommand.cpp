#include "cli/SubCommand.h"

#include <optional>

namespace neurus::cli
{

void AddFormatOption(CLI::App& app, std::string& storage)
{
	app.add_option("--format", storage, "Output format: human or json")
		->check(CLI::IsMember({"human", "json"}));
}

void AddSessionOption(CLI::App& app, std::string& storage)
{
	app.add_option("-s,--session", storage, "Session name; defaults to the only live session.");
}

OutputFormat SubCommand::Format(const std::string& localFormat) const
{
	const std::string& value = (m_globalFormat != nullptr && !m_globalFormat->empty())
		? *m_globalFormat
		: localFormat;
	return value == "json" ? OutputFormat::Json : OutputFormat::Human;
}

std::optional<std::string> SubCommand::SessionName(const std::string& localSession) const
{
	if (!localSession.empty())
		return localSession;
	if (m_globalSession != nullptr && !m_globalSession->empty())
		return *m_globalSession;
	return std::nullopt;
}

void SubCommand::SetGlobals(const GlobalOptions& globals)
{
	m_globalFormat = &globals.format;
	m_globalSession = &globals.session;
}

} // namespace neurus::cli
