#include "cli/subcommands/ListCommand.h"

#include "cli/Output.h"
#include "cli/SessionClient.h"

namespace neurus::cli
{

void ListCommand::Register(CLI::App& parent, const std::string& globalFormat)
{
	m_globalFormat = &globalFormat;
	m_app = parent.add_subcommand("list", "List live sessions in the local registry.");
	AddFormatOption(*m_app, m_format);
}

bool ListCommand::Selected() const
{
	return m_app != nullptr && m_app->parsed();
}

CliExit ListCommand::Execute()
{
	ListOptions options;
	options.format = Format(m_format);
	PrintSessions(LiveSessions(), options.format);
	return CliExit::Ok;
}

} // namespace neurus::cli
