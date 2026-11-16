#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace neurus::command
{

/**
 * @brief Declares one command for help text, allow-listing and validation.
 */
struct CommandDescriptor
{
	std::string              name;         ///< Dotted command name.
	std::string              summary;      ///< One-line description.
	bool                     mutating = false;   ///< True when the command changes document state.
	bool                     sensitive = false;  ///< True for close and script execution.
	std::vector<std::string> requiredArgs;       ///< Argument keys that must be present.
};

/**
 * @brief Immutable set of command descriptors shared by the CLI and server.
 */
class CommandCatalog
{
public:
	CommandCatalog() = default;

	/**
	 * @brief Builds a catalog from descriptors.
	 * @param descriptors Descriptors to own; duplicate names are kept as given.
	 */
	explicit CommandCatalog(std::vector<CommandDescriptor> descriptors);

	/**
	 * @brief Finds a descriptor by exact name.
	 * @param name Command name to look up.
	 * @return The descriptor, or nullptr when the name is unknown.
	 */
	const CommandDescriptor* Find(std::string_view name) const;

	/// @return Every descriptor in registration order.
	const std::vector<CommandDescriptor>& All() const;

private:
	std::vector<CommandDescriptor> m_descriptors;
};

/**
 * @brief Builds the catalog of commands this application implements.
 * @return Descriptors for the session, project, scene, object, render and
 *         history families.
 */
CommandCatalog MakeCommandCatalog();

} // namespace neurus::command
