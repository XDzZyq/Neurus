#pragma once

#include "cli/CliParse.h"

namespace neurus::cli
{

/// Runs `neurus open`.
CliExit RunOpen(const OpenOptions& options);

} // namespace neurus::cli
