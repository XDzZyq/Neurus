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

CommandCatalog MakeCommandCatalog()
{
	return CommandCatalog({
		CommandDescriptor{"session.status", "Report the running session state.", false, false, {}},
		CommandDescriptor{"session.close", "Close the running session.", true, true, {}},
		CommandDescriptor{"project.info", "Report the current project document.", false, false, {}},
		CommandDescriptor{"project.new", "Replace the document with a new scene.", true, false, {}},
		CommandDescriptor{"project.open", "Load a project file.", true, false, {"path"}},
		CommandDescriptor{"project.save", "Save the project to its path or a new path.", true, false, {}},
		CommandDescriptor{"scene.list", "List scene objects, optionally by type.", false, false, {}},
		CommandDescriptor{"object.get", "Report one scene object.", false, false, {}},
		CommandDescriptor{"object.set", "Change properties of one scene object.", true, false, {}},
		CommandDescriptor{"object.load", "Load a mesh file into the scene.", true, false, {"path"}},
		CommandDescriptor{"object.delete", "Remove one or more scene objects.", true, false, {}},
		CommandDescriptor{"render.get", "Report the render configuration.", false, false, {}},
		CommandDescriptor{"render.set", "Change render configuration values.", true, false, {}},
		CommandDescriptor{"history.get", "Report the undo/redo history.", false, false, {}},
		CommandDescriptor{"history.undo", "Undo the most recent edit.", true, false, {}},
		CommandDescriptor{"history.redo", "Redo the most recently undone edit.", true, false, {}},
	});
}

} // namespace neurus::command
