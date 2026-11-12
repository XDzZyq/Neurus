#pragma once

#include "cli/SubCommand.h"
#include "command/CommandTypes.h"

#include <string>
#include <vector>

namespace neurus::cli
{

/// `neurus script`: run Python in the application.
class ScriptCommand final : public SubCommand
{
public:
	void Register(CLI::App& parent, const GlobalOptions& globals) override;
	bool Selected() const override;
	CliExit Execute() override;

private:
	/// Adds the sandbox/trusted and policy-override flags to a run-like parser.
	void AddSandboxOptions(CLI::App& app);

	/// Encodes the sandbox choice and its overrides into a command's args.
	void AddSandboxArgs(command::Json& args) const;

	CLI::App* m_app = nullptr;
	CLI::App* m_runApp = nullptr;
	CLI::App* m_infoApp = nullptr;
	CLI::App* m_evalApp = nullptr;
	CLI::App* m_consoleApp = nullptr;
	CLI::App* m_clearApp = nullptr;
	CLI::App* m_modeApp = nullptr;

	std::string m_code;
	std::string m_file;
	std::string m_evalCode;
	std::string m_modeName;
	int         m_tail = 50;
	std::string m_session;
	std::string m_format;
	int         m_timeoutSeconds = 60;

	// Sandbox options. `--trusted` is the only way to run unrestricted; a run
	// is sandboxed when neither flag is given.
	bool m_sandbox = false;
	bool m_trustedMode = false;
	std::vector<std::string> m_allowRead;
	std::vector<std::string> m_allowWrite;
	std::vector<std::string> m_blockedModules;
	bool   m_allowNetwork = false;
	bool   m_allowSubprocess = false;
	bool   m_allowCtypes = false;
	double m_timeLimitSeconds = -1.0;  ///< Negative = not given.
	int    m_memoryLimitMb = -1;       ///< Negative = not given.
};

} // namespace neurus::cli
