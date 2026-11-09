#include "cli/Output.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <cstddef>

namespace neurus::cli
{
namespace
{

command::Json SessionJson(const ipc::SessionRecord& record, std::optional<bool> dirty)
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

} // namespace

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
		const command::Json json{{"ok", true}, {"session", SessionJson(record, dirty)}};
		std::cout << json.dump(2) << '\n';
		return;
	}

	std::cout << "session " << record.session << " pid " << record.pid
	          << " endpoint 127.0.0.1:" << record.port << '\n';
	std::cout << "project " << record.project << " dirty " << (dirty ? "yes" : "no") << '\n';
}

void PrintStarting(const std::string& session, int pid, OutputFormat format)
{
	if (format == OutputFormat::Json)
	{
		const command::Json json{
			{"ok", true},
			{"session", {{"name", session}, {"pid", pid}, {"starting", true}}},
		};
		std::cout << json.dump(2) << '\n';
		return;
	}
	std::cout << "session " << session << " pid " << pid << " (starting)\n";
}

void PrintSessions(const std::vector<ipc::SessionRecord>& records, OutputFormat format)
{
	if (format == OutputFormat::Json)
	{
		command::Json sessions = command::Json::array();
		for (const auto& record : records)
			sessions.push_back(SessionJson(record, std::nullopt));
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

void PrintCloseResults(const std::vector<std::pair<std::string, command::CommandResult>>& results,
                       OutputFormat format)
{
	if (format == OutputFormat::Json)
	{
		command::Json entries = command::Json::array();
		bool allOk = true;
		for (const auto& [session, result] : results)
		{
			command::Json entry{{"session", session}, {"ok", result.ok}};
			if (result.ok)
				entry["result"] = result.details;
			else
			{
				allOk = false;
				entry["error"] = {{"code", std::string(command::ToString(result.code))},
				                  {"message", result.message},
				                  {"details", result.details}};
			}
			entries.push_back(std::move(entry));
		}
		const command::Json json{{"ok", allOk}, {"results", entries}};
		std::cout << json.dump(2) << '\n';
		return;
	}

	for (const auto& [session, result] : results)
	{
		if (result.ok)
			std::cout << "closed " << session << '\n';
		else
			std::cout << session << ": " << command::ToString(result.code) << ": "
			          << result.message << '\n';
	}
}

std::string ReadLogTail(const std::filesystem::path& path, std::size_t maxLines)
{
	std::ifstream in(path, std::ios::binary);
	if (!in.is_open())
		return {};

	std::vector<std::string> lines;
	std::string line;
	while (std::getline(in, line))
	{
		lines.push_back(std::move(line));
		if (lines.size() > maxLines * 2)
			lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(maxLines));
	}

	std::ostringstream out;
	const std::size_t first = lines.size() > maxLines ? lines.size() - maxLines : 0;
	for (std::size_t i = first; i < lines.size(); ++i)
		out << lines[i] << '\n';
	return out.str();
}

} // namespace neurus::cli
