#pragma once

#include "cli/SubCommand.h"

#include <optional>
#include <string>

namespace neurus::cli
{

/// `neurus open`: launch a named background session and wait for readiness.
class OpenCommand final : public SubCommand
{
public:
	void Register(CLI::App& parent, const GlobalOptions& globals) override;
	bool Selected() const override;
	CliExit Execute() override;

private:
	std::optional<std::string> m_project;
	std::optional<std::string> m_name;
	std::optional<std::string> m_appPath;
	int                        m_timeoutSeconds = 30;
	bool                       m_noWait = false;
	bool                       m_noBind = false;
	std::string                m_format;
};

} // namespace neurus::cli
