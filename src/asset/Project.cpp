/**
 * @file Project.cpp
 * @brief Registration-based serializer implementation.
 */

#include "asset/Project.h"

#include <fstream>
#include <stdexcept>

#include <cereal/archives/json.hpp>

#include "core/Log.h"

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

	{
		cereal::JSONInputArchive archive(is);
		archive(cereal::make_nvp("project", *this));
	}

	NEURUS_LOG("[Project] Loaded from " << path);
}

} // namespace neurus::project
