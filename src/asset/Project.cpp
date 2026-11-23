/**
 * @file Project.cpp
 * @brief Registration-based serializer implementation.
 */

#include "asset/Project.hpp"

#include <fstream>
#include <stdexcept>

#include <cereal/archives/json.hpp>
#include <nlohmann/json.hpp>

#include "core/Log.hpp"

namespace neurus::project
{

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

void Project::Save(const std::string& path) const
{
	std::ofstream os(path);
	if (!os.is_open())
	{
		throw std::runtime_error("Failed to create project file: " + path);
	}

	{
		cereal::JSONOutputArchive archive(os);
		archive(cereal::make_nvp("project", *this));
	}

	NEURUS_LOG("[Project] Saved to " << path);
}

void Project::Load(const std::string& path)
{
	std::ifstream is(path);
	if (!is.is_open())
	{
		throw std::runtime_error("Failed to open project file: " + path);
	}

	const nlohmann::ordered_json document = nlohmann::ordered_json::parse(is);
	const auto& project = document.at("project");
	if (!project.is_object())
		throw std::runtime_error("Project root must be an object");
	// Components keep their own recovery policy, but never share parser state.
	// A malformed graph/config must not make later UI or history data disappear.
	for (auto& component : m_components)
	{
		nlohmann::ordered_json isolated = nlohmann::ordered_json::object();
		if (const auto value = project.find(component->Key()); value != project.end())
			isolated[component->Key()] = *value;
		component->LoadJson(isolated.dump());
	}

	NEURUS_LOG("[Project] Loaded from " << path);
}

} // namespace neurus::project
