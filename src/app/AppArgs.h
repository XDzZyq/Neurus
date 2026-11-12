#pragma once

#include <optional>
#include <string>
#include <vector>

namespace neurus
{

/**
 * @brief Command-line arguments understood by the Neurus application.
 *
 * `--session` makes this instance a background command server; `--project`
 * overrides the project opened at startup; `--preload` imports a Python
 * package before the first frame and may be repeated. Unknown arguments are
 * left to Qt.
 */
struct AppArgs
{
	std::optional<std::string> session;  ///< Named command session.
	std::optional<std::string> project;  ///< Project file to open.
	std::vector<std::string>   preload;  ///< Packages imported at startup.
	std::string                error;    ///< Parse error, empty when valid.

	/**
	 * @brief Parses an argument list.
	 * @param arguments Argument list, including the program name at index 0.
	 */
	static AppArgs Parse(const std::vector<std::string>& arguments);

	/**
	 * @brief Parses a raw argv array.
	 * @param argc Argument count.
	 * @param argv Argument vector.
	 */
	static AppArgs Parse(int argc, char* argv[]);
};

} // namespace neurus
