#include "cli/SubCommand.h"

namespace neurus::cli
{

void AddFormatOption(CLI::App& app, std::string& storage)
{
	app.add_option("--format", storage, "Output format: human or json")
		->check(CLI::IsMember({"human", "json"}));
}

OutputFormat SubCommand::Format(const std::string& localFormat) const
{
	const std::string& value = (m_globalFormat != nullptr && !m_globalFormat->empty())
		? *m_globalFormat
		: localFormat;
	return value == "json" ? OutputFormat::Json : OutputFormat::Human;
}

} // namespace neurus::cli
