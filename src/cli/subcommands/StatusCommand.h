#pragma once

#include "cli/SubCommand.h"

#include <optional>
#include <string>

namespace neurus::cli
{

/// `neurus status`: report one running session.
class StatusCommand final : public SubCommand
{
public:
	void Register(CLI::App& parent, const std::string& globalFormat) override;
	bool Selected() const override;
	CliExit Execute() override;

private:
	std::optional<std::string> m_name;
	int                        m_timeoutSeconds = 10;
	std::string                m_format;
};

} // namespace neurus::cli
