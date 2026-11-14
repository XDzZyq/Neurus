#include "app/ProjectIo.h"

#include "core/Log.h"
#include "editor/Editor.h"

#include "asset/Project.h"
#include "asset/components/ConfigComponent.h"
#include "asset/components/EditorComponent.h"
#include "asset/components/HistoryComponent.h"
#include "asset/components/ResourceComponent.h"
#include "asset/components/SceneComponent.h"
#include "asset/components/UIComponent.h"

#include <exception>

namespace neurus
{
namespace
{

/**
 * @brief Converts a path to the UTF-8 string Project::Load/Save take.
 *
 * std::filesystem::path converts implicitly to std::string only where its native
 * string type is char; on MSVC it is std::wstring, so an implicit conversion does
 * not compile (and path::string() is the ANSI code page, which throws beyond it).
 */
std::string Utf8Path(const std::filesystem::path& path)
{
	const std::u8string utf8 = path.u8string();
	return std::string(utf8.begin(), utf8.end());
}

} // namespace

void RegisterProjectComponents(project::Project& proj, Editor& editor, std::string& uiLayout)
{
	// Resource pool first: the Scene's ID references resolve against it.
	proj.Register<project::ResourceComponent>(editor.GetResourceManager());
	proj.Register<project::SceneComponent>(editor.GetScene(), editor.GetResourceManager());
	proj.Register<project::ConfigComponent>(editor.GetRenderConfig());
	proj.Register<project::UIComponent>(uiLayout);
	// History, then editor view state: both are appended-last nodes, so legacy
	// files that lack them load cleanly (each Load() falls back).
	proj.Register<project::HistoryComponent>(editor.GetOperations());
	proj.Register<project::EditorComponent>(editor.GetViewport());
}

bool LoadProject(const std::filesystem::path& path, Editor& editor,
                 std::string& uiLayout, std::string& error)
{
	std::error_code fileError;
	if (!std::filesystem::is_regular_file(path, fileError))
	{
		error = "Project file not found: " + Utf8Path(path);
		NEURUS_ERR("[ProjectIo] " << error);
		return false;
	}

	try
	{
		editor.BeginLoad();
		project::Project proj;
		RegisterProjectComponents(proj, editor, uiLayout);
		proj.Load(Utf8Path(path));
		editor.FinishLoad();
	}
	catch (const std::exception& e)
	{
		error = e.what();
		NEURUS_ERR("[ProjectIo] Failed to open project: " << e.what());
		return false;
	}
	return true;
}

bool SaveProject(const std::filesystem::path& path, Editor& editor,
                 const std::string& uiLayout, std::string& error)
{
	if (path.empty())
	{
		error = "No project path is set.";
		NEURUS_ERR("[ProjectIo] " << error);
		return false;
	}

	try
	{
		std::string layout = uiLayout;
		project::Project proj;
		RegisterProjectComponents(proj, editor, layout);
		proj.Save(Utf8Path(path));
		editor.ClearDirty();
	}
	catch (const std::exception& e)
	{
		error = e.what();
		NEURUS_ERR("[ProjectIo] Failed to save project: " << e.what());
		return false;
	}
	return true;
}

} // namespace neurus
