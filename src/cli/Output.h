#pragma once

#include "cli/CliParse.h"
#include "ipc/SessionRegistry.h"

#include <string>
#include <utility>
#include <vector>

namespace neurus::cli
{

/// Prints a one-line error in the selected format.
void PrintError(OutputFormat format, command::CommandErrorCode code,
                const std::string& message,
                const command::Json& details = command::Json::object());

/// Prints the sessions produced by `open` or `status`.
void PrintSession(const ipc::SessionRecord& record, bool dirty, OutputFormat format);

/// Prints the launch notice for `open --no-wait`.
void PrintStarting(const std::string& session, int pid, OutputFormat format);

/// Prints a table for `list`.
void PrintSessions(const std::vector<ipc::SessionRecord>& records, OutputFormat format);

/// Prints per-session close outcomes.
void PrintCloseResults(const std::vector<std::pair<std::string, command::CommandResult>>& results,
                       OutputFormat format);

/// Reads the last lines of a log file for an early-exit diagnostic.
std::string ReadLogTail(const std::filesystem::path& path, std::size_t maxLines);

} // namespace neurus::cli
