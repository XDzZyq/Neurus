#pragma once

#include "cli/CliParse.h"
#include "ipc/SessionRegistry.h"

#include <optional>
#include <string>

namespace neurus::cli
{

/// Builds the JSON object describing one session.
command::Json SessionRecordJson(const ipc::SessionRecord& record,
                                std::optional<bool> dirty = std::nullopt);

/// Prints a one-line error in the selected format.
void PrintError(OutputFormat format, command::CommandErrorCode code,
                const std::string& message,
                const command::Json& details = command::Json::object());

/// Prints the session produced by `open` or `status`.
void PrintSession(const ipc::SessionRecord& record, bool dirty, OutputFormat format);

} // namespace neurus::cli
