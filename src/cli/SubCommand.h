#pragma once

#include "cli/CliParse.h"

#include <CLI/CLI.hpp>

#include <string>

namespace neurus::cli
{

/// Root-level options a subcommand may fall back to.
struct GlobalOptions
{
	const std::string& format;   ///< `--format` given before the subcommand.
	const std::string& session;  ///< `--session` given before the subcommand.
};

/// Adds the shared `--format human|json` option to one command.
void AddFormatOption(CLI::App& app, std::string& storage);

/// Adds the shared `--session NAME` target option to one command.
void AddSessionOption(CLI::App& app, std::string& storage);

/**
 * @brief Base class for one `neurus` subcommand.
 *
 * Each command owns its CLI11 options and registers itself on a parent app,
 * mirroring the Editor controller pattern. Cli.cpp only builds the command
 * list, registers it, and dispatches the selected command.
 */
class SubCommand
{
public:
	virtual ~SubCommand() = default;

	/**
	 * @brief Creates the CLI11 subcommand and binds its options.
	 * @param parent Parent CLI::App.
	 * @param globals Root-level option values.
	 */
	virtual void Register(CLI::App& parent, const GlobalOptions& globals) = 0;

	/// @return True when this subcommand was selected on the command line.
	virtual bool Selected() const = 0;

	/// @brief Executes the command after parsing.
	virtual CliExit Execute() = 0;

protected:
	/// Resolves the local/global format option into one output format.
	OutputFormat Format(const std::string& localFormat) const;

	/**
	 * @brief Resolves the local/global `--session` option.
	 * @param localSession Value of the command-local option.
	 * @return The explicit name, or std::nullopt to use the only live session.
	 */
	std::optional<std::string> SessionName(const std::string& localSession) const;

	/// Stores the root-level fallbacks; called by each Register().
	void SetGlobals(const GlobalOptions& globals);

	CLI::App*          m_app = nullptr;
	const std::string* m_globalFormat = nullptr;
	const std::string* m_globalSession = nullptr;
};

} // namespace neurus::cli
