#pragma once

#include "cli/SubCommand.h"

#include <string>

namespace neurus::cli
{

/// `neurus snapshot`: capture the viewport or all render attachments.
class SnapshotCommand final : public SubCommand
{
public:
	void Register(CLI::App& parent, const GlobalOptions& globals) override;
	bool Selected() const override;
	CliExit Execute() override;

private:
	CLI::App* m_app = nullptr;
	CLI::App* m_captureApp = nullptr;

	std::string m_path;
	bool        m_all = false;
	std::string m_session;
	std::string m_format;
	int         m_timeoutSeconds = 90;
};

} // namespace neurus::cli
