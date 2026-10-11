#include "Icons.h"
#include "scene/ObjectID.h"

namespace neurus
{

// --- Static members --------------------------------------------------------

std::unordered_map<std::string, QString> Icons::s_paths;
std::unordered_map<std::string, QIcon>   Icons::s_cache;
bool                                     Icons::s_initialized = false;

// --- Initialize (idempotent) -----------------------------------------------

void Icons::Initialize()
{
	if (s_initialized) return;
	s_initialized = true;

	// --- Scene icons ---
	RegisterIcon("scene:camera",      ":/icons/scene/camera.svg");
	RegisterIcon("scene:environment", ":/icons/scene/environment.svg");
	RegisterIcon("scene:light",       ":/icons/scene/light.svg");
	RegisterIcon("scene:mesh",        ":/icons/scene/mesh.svg");

	// --- Editor icons ---
	RegisterIcon("editor:movie",             ":/icons/editor/movie.svg");
	RegisterIcon("editor:movie_off",         ":/icons/editor/movie_off.svg");
	RegisterIcon("editor:preview_invisible", ":/icons/editor/preview_invisible.svg");
	RegisterIcon("editor:preview_visible",   ":/icons/editor/preview_visible.svg");
	RegisterIcon("editor:render_invisible",  ":/icons/editor/render_invisible.svg");
	RegisterIcon("editor:render_visible",    ":/icons/editor/render_visible.svg");
}

// --- ObjectIcon — GOType → QIcon ------------------------------------------

QIcon Icons::ObjectIcon(int goType)
{
	switch (ObjectID::GOType(goType))
	{
	case ObjectID::GOType::GO_CAM:    return GetIcon("scene:camera");
	case ObjectID::GOType::GO_LIGHT:  return GetIcon("scene:light");
	case ObjectID::GOType::GO_MESH:   return GetIcon("scene:mesh");
	default: return GetIcon("scene:mesh");
	}
}

// --- RegisterIcon ----------------------------------------------------------

void Icons::RegisterIcon(const std::string& name, const QString& resourcePath)
{
	s_paths[name] = resourcePath;
}

// --- GetIcon ---------------------------------------------------------------

const QIcon& Icons::GetIcon(const std::string& name)
{
	auto cacheIt = s_cache.find(name);
	if (cacheIt != s_cache.end())
	{
		return cacheIt->second;
	}

	auto pathIt = s_paths.find(name);
	if (pathIt == s_paths.end())
	{
		// Unknown icon name: return a shared empty icon, do not cache.
		static const QIcon s_emptyIcon;
		return s_emptyIcon;
	}

	QIcon icon(pathIt->second);
	auto [insertedIt, _] = s_cache.emplace(name, std::move(icon));
	return insertedIt->second;
}

const QIcon &Icons::GetIconPair(const std::string &name_on, const std::string &name_off)
{
	auto cacheIt = s_cache.find(name_on + name_off);
	if (cacheIt != s_cache.end())
	{
		return cacheIt->second;
	}

	auto path_on = s_paths.find(name_on);
	auto path_off = s_paths.find(name_off);


	QIcon icon;

	icon.addFile(
		path_on->second,
		QSize(),
		QIcon::Normal,
		QIcon::On
	);

	icon.addFile(
		path_off->second,
		QSize(),
		QIcon::Normal,
		QIcon::Off
	);

	auto [insertedIt, _] = s_cache.emplace(name_on + name_off, std::move(icon));
	return insertedIt->second;
}

} // namespace neurus
