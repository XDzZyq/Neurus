#pragma once

#include "cli/CliParse.h"

#include <CLI/CLI.hpp>

#include <string>

namespace neurus::cli
{

/// Adds the shared `--format human|json` option to one command.
void AddFormatOption(CLI::App& app, std::string& storage);

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
	 * @param globalFormat Value of the global `--format`, if given.
	 */
	virtual void Register(CLI::App& parent, const std::string& globalFormat) = 0;

	/// @return True when this subcommand was selected on the command line.
	virtual bool Selected() const = 0;

	/// @brief Executes the command after parsing.
	virtual CliExit Execute() = 0;

protected:
	/// Resolves the local/global format option into one output format.
	OutputFormat Format(const std::string& localFormat) const;

	CLI::App*          m_app = nullptr;
	const std::string* m_globalFormat = nullptr;
};

} // namespace neurus::cli
