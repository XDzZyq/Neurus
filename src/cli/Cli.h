#pragma once

#include <string>
#include <vector>

namespace neurus::cli
{

/**
 * @brief Parses the command line and runs the requested command.
 * @param arguments Process arguments including argv[0], in normal order.
 * @return Process exit code.
 */
int RunCli(const std::vector<std::string>& arguments);

} // namespace neurus::cli
