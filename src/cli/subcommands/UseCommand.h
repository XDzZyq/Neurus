#pragma once

#include "cli/SubCommand.h"

#include <optional>
#include <string>

namespace neurus::cli
{

/// `neurus use`: bind a default session to this terminal.
class UseCommand final : public SubCommand
{
public:
	void Register(CLI::App& parent, const GlobalOptions& globals) override;
	bool Selected() const override;
	CliExit Execute() override;

private:
	CLI::App*                  m_app = nullptr;
	std::optional<std::string> m_name;
	bool                       m_clear = false;
	std::string                m_format;
};

} // namespace neurus::cli
