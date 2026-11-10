#pragma once

#include "cli/SubCommand.h"

#include <string>

namespace neurus::cli
{

/// `neurus list`: list live sessions from the local registry.
class ListCommand final : public SubCommand
{
public:
	void Register(CLI::App& parent, const std::string& globalFormat) override;
	bool Selected() const override;
	CliExit Execute() override;

private:
	std::string m_format;
};

} // namespace neurus::cli
