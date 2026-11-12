#include "cli/Output.h"

#include <iostream>

namespace neurus::cli
{

command::Json SessionRecordJson(const ipc::SessionRecord& record, std::optional<bool> dirty)
{
	command::Json session{
		{"name", record.session},
		{"pid", record.pid},
		{"endpoint", "127.0.0.1:" + std::to_string(record.port)},
		{"project", record.project},
		{"app_version", record.appVersion},
	};
	if (dirty)
		session["dirty"] = *dirty;
	return session;
}

void PrintError(OutputFormat format, command::CommandErrorCode code,
                const std::string& message, const command::Json& details)
{
	if (format == OutputFormat::Json)
	{
		const command::Json json{
			{"ok", false},
			{"error", {{"code", std::string(command::ToString(code))},
			           {"message", message},
			           {"details", details}}},
		};
		std::cout << json.dump(2) << '\n';
		return;
	}

	std::cout << (command::ToString(code).empty() ? "error" : command::ToString(code))
	          << ": " << message << '\n';
}

void PrintSession(const ipc::SessionRecord& record, bool dirty, OutputFormat format)
{
	if (format == OutputFormat::Json)
	{
		const command::Json json{{"ok", true}, {"session", SessionRecordJson(record, dirty)}};
		std::cout << json.dump(2) << '\n';
		return;
	}

	std::cout << "session " << record.session << " pid " << record.pid
	          << " endpoint 127.0.0.1:" << record.port << '\n';
	std::cout << "project " << record.project << " dirty " << (dirty ? "yes" : "no") << '\n';
}

} // namespace neurus::cli
