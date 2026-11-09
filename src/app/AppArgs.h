#pragma once

#include <QStringList>

#include <optional>
#include <string>

namespace neurus
{

/**
 * @brief Command-line arguments understood by the Neurus application.
 *
 * `--session` makes this instance a background command server; `--project`
 * overrides the project opened at startup. Unknown arguments are left to Qt.
 */
struct AppArgs
{
	std::optional<std::string> session;  ///< Named command session.
	std::optional<std::string> project;  ///< Project file to open.
	std::string                error;    ///< Parse error, empty when valid.

	/**
	 * @brief Parses arguments as returned by QCoreApplication::arguments().
	 * @param arguments Argument list, including the program name.
	 */
	static AppArgs Parse(const QStringList& arguments);

	/**
	 * @brief Parses a raw argv array.
	 * @param argc Argument count.
	 * @param argv Argument vector.
	 */
	static AppArgs Parse(int argc, char* argv[]);
};

} // namespace neurus
