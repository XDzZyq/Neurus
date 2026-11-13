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
		error = "Project file not found: " + path.string();
		NEURUS_ERR("[ProjectIo] " << error);
		return false;
	}

	try
	{
		editor.BeginLoad();
		project::Project proj;
		RegisterProjectComponents(proj, editor, uiLayout);
		proj.Load(path);
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
		proj.Save(path);
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
