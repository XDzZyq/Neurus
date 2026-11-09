#pragma once

#include "cli/CliParse.h"

namespace neurus::cli
{

/// Runs `neurus status`.
CliExit RunStatus(const StatusOptions& options);

} // namespace neurus::cli
