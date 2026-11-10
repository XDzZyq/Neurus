#pragma once

#include "cli/SubCommand.h"

namespace neurus::cli
{

/**
 * @brief `neurus project` - inspect, create, open and save the document.
 */
class ProjectCommand final : public SubCommand
{
public:
	void Register(CLI::App& parent, const GlobalOptions& globals) override;
	bool Selected() const override;
	CliExit Execute() override;

private:
	struct CommonOptions
	{
		std::string session;
		std::string format;
		int         timeoutSeconds = 10;
	};

	enum class Action
	{
		None,
		Info,
		New,
		Open,
		Save
	};

	Action SelectedAction() const;

	CLI::App* m_app = nullptr;
	CLI::App* m_infoApp = nullptr;
	CLI::App* m_newApp = nullptr;
	CLI::App* m_openApp = nullptr;
	CLI::App* m_saveApp = nullptr;

	CommonOptions m_info;
	CommonOptions m_new;
	CommonOptions m_open;
	CommonOptions m_save;

	bool        m_newForce = false;
	std::string m_openPath;
	bool        m_openForce = false;
	std::string m_savePath;
};

} // namespace neurus::cli
