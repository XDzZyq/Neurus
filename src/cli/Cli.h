#pragma once

namespace neurus::cli
{

/**
 * @brief Parses the command line and runs the requested command.
 * @return Process exit code.
 */
int RunCli(int argc, char* argv[]);

} // namespace neurus::cli
