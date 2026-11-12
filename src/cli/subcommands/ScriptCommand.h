#pragma once

#include "cli/SubCommand.h"

#include <string>

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
	CLI::App* m_app = nullptr;
	CLI::App* m_runApp = nullptr;
	CLI::App* m_infoApp = nullptr;
	CLI::App* m_evalApp = nullptr;
	CLI::App* m_consoleApp = nullptr;
	CLI::App* m_clearApp = nullptr;

	std::string m_code;
	std::string m_file;
	std::string m_evalCode;
	int         m_tail = 50;
	std::string m_session;
	std::string m_format;
	int         m_timeoutSeconds = 60;
};

} // namespace neurus::cli
