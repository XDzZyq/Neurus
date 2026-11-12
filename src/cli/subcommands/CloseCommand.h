#pragma once

#include "cli/SubCommand.h"

#include <optional>
#include <string>

namespace neurus::cli
{

/// `neurus close`: close one named session, the only live session, or all.
class CloseCommand final : public SubCommand
{
public:
	void Register(CLI::App& parent, const GlobalOptions& globals) override;
	bool Selected() const override;
	CliExit Execute() override;

private:
	std::optional<std::string> m_name;
	std::string                m_session;
	bool                       m_all = false;
	bool                       m_save = false;
	bool                       m_force = false;
	int                        m_timeoutSeconds = 10;
	std::string                m_format;
};

} // namespace neurus::cli
