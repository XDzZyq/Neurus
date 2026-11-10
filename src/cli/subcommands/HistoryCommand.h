#pragma once

#include "cli/SubCommand.h"

#include <string>

namespace neurus::cli
{

/// `neurus history`: report or step the undo/redo history.
class HistoryCommand final : public SubCommand
{
public:
	void Register(CLI::App& parent, const GlobalOptions& globals) override;
	bool Selected() const override;
	CliExit Execute() override;

private:
	CliExit ExecuteAction(const char* command, const char* action);

	CLI::App* m_app = nullptr;
	CLI::App* m_showApp = nullptr;
	CLI::App* m_undoApp = nullptr;
	CLI::App* m_redoApp = nullptr;

	std::string m_session;
	std::string m_format;
	int         m_timeoutSeconds = 10;
};

} // namespace neurus::cli
