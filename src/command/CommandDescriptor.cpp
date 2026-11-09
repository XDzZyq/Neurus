#include "command/CommandDescriptor.h"

#include <utility>

namespace neurus::command
{

CommandCatalog::CommandCatalog(std::vector<CommandDescriptor> descriptors)
	: m_descriptors(std::move(descriptors))
{
}

const CommandDescriptor* CommandCatalog::Find(std::string_view name) const
{
	for (const auto& descriptor : m_descriptors)
	{
		if (descriptor.name == name)
			return &descriptor;
	}
	return nullptr;
}

const std::vector<CommandDescriptor>& CommandCatalog::All() const
{
	return m_descriptors;
}

CommandCatalog MakeSessionCatalog()
{
	return CommandCatalog({
		CommandDescriptor{"session.status", "Report the running session state.", false, false, {}},
		CommandDescriptor{"session.close", "Close the running session.", true, true, {}},
	});
}

} // namespace neurus::command
