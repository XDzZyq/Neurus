#pragma once

#include "cli/SubCommand.h"

namespace neurus::cli
{

/// `neurus scene`: inspect the scene document.
class SceneCommand final : public SubCommand
{
public:
	void Register(CLI::App& parent, const GlobalOptions& globals) override;
	bool Selected() const override;
	CliExit Execute() override;

private:
	CLI::App* m_app = nullptr;
	CLI::App* m_listApp = nullptr;

	std::string m_type;
	std::string m_session;
	std::string m_format;
	int         m_timeoutSeconds = 10;
};

} // namespace neurus::cli
