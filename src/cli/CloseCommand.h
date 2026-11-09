#pragma once

#include "cli/CliParse.h"

namespace neurus::cli
{

/// Runs `neurus close`.
CliExit RunClose(const CloseOptions& options);

} // namespace neurus::cli
