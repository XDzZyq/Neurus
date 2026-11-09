#pragma once

#include "cli/CliParse.h"

namespace neurus::cli
{

/// Runs `neurus list`.
CliExit RunList(const ListOptions& options);

} // namespace neurus::cli
