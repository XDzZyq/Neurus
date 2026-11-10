#include "cli/subcommands/ListCommand.h"

#include "cli/Output.h"
#include "cli/SessionClient.h"

#include <iostream>
#include <vector>

namespace neurus::cli
{
namespace
{

void PrintSessions(const std::vector<ipc::SessionRecord>& records, OutputFormat format)
{
	if (format == OutputFormat::Json)
	{
		command::Json sessions = command::Json::array();
		for (const auto& record : records)
			sessions.push_back(SessionRecordJson(record));
		const command::Json json{{"ok", true}, {"sessions", sessions}};
		std::cout << json.dump(2) << '\n';
		return;
	}

	std::cout << "session\tpid\tport\tproject\n";
	for (const auto& record : records)
	{
		std::cout << record.session << '\t' << record.pid << '\t' << record.port << '\t'
		          << record.project << '\n';
	}
}

} // namespace

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
