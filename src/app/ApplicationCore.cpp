#include "app/ApplicationCore.h"

#include "app/Preferences.h"
#include "app/ProjectIo.h"
#include "app/ScriptCommands.h"

#include "core/Log.h"
#include "editor/Editor.h"
#include "script/ScriptConsole.h"
#include "script/ScriptEngine.h"

#include <exception>
#include <iomanip>
#include <sstream>
#include <utility>

namespace neurus
{
namespace
{

/**
 * @brief Converts a path to the UTF-8 string the editor and project layer take.
 *
 * path::string() is the ANSI code page on MSVC and throws beyond it.
 */
std::string Utf8Path(const std::filesystem::path& path)
{
	const std::u8string utf8 = path.u8string();
	return std::string(utf8.begin(), utf8.end());
}

} // namespace

ApplicationCore::ApplicationCore()
	: m_preferences(std::make_unique<Preferences>())
	, m_script(std::make_unique<ScriptEngine>())
{
}

ApplicationCore::~ApplicationCore() = default;

void ApplicationCore::LoadPreferences()
{
	const std::string path = Preferences::DefaultPath();
	const Preferences::LoadResult result = m_preferences->Load(path);
	if (result == Preferences::LoadResult::Corrupt)
	{
		NEURUS_ERR("[ApplicationCore] Preferences at " << path
		          << " are corrupt; moving them aside and using defaults");
		Preferences::Backup(path);
	}
}

bool ApplicationCore::InitializeScripting(Editor& editor, const std::vector<std::string>& preload)
{
	m_script->SetEventQueue(&editor.GetEventQueue());
	if (!m_script->Initialize())
	{
		NEURUS_ERR("[ApplicationCore] Script engine failed to initialize; "
		          "script.* commands will report it as unavailable");
		return false;
	}

	if (!preload.empty())
	{
		// Before the first frame: a heavy import is paid here, once, instead of
		// stalling the first script that needs it. A failure is reported and does
		// not abort startup. The line also lands in the console buffer, which is
		// what the UI panel and `script console` read.
		for (const ScriptPreloadResult& result : m_script->Preload(preload))
		{
			std::ostringstream line;
			if (!result.ok)
			{
				line << "preload " << result.package << " failed: " << result.error;
			}
			else
			{
				line << "preload " << result.package << ": " << std::fixed
				     << std::setprecision(3) << result.loadSeconds << "s ("
				     << (result.cached ? "cached" : "loaded") << ")";
			}

			const std::string text = line.str();
			m_script->Console().Append(
				result.ok ? ScriptLineKind::Output : ScriptLineKind::Error, text + "\n");
			if (result.ok)
			{
				NEURUS_LOG("[ApplicationCore] " << text);
			}
			else
			{
				NEURUS_ERR("[ApplicationCore] " << text);
			}
		}
	}
	return true;
}

bool ApplicationCore::CreateDefaultDocument(Editor& editor, const std::filesystem::path& objPath,
                                            std::string& error)
{
	try
	{
		editor.NewScene();
		editor.CreateDefaultScene(Utf8Path(objPath));
		editor.ClearDirty();
		editor.UploadSceneResources();
	}
	catch (const std::exception& e)
	{
		error = e.what();
		NEURUS_ERR("[ApplicationCore] Failed to create the default document: " << e.what());
		return false;
	}
	return true;
}

bool ApplicationCore::LoadProject(const std::filesystem::path& path, Editor& editor,
                                  std::string& uiLayout, std::string& error)
{
	if (!::neurus::LoadProject(path, editor, uiLayout, error))
		return false;

	m_projectPath = Utf8Path(path);
	return true;
}

bool ApplicationCore::SaveProject(const std::filesystem::path& path, Editor& editor,
                                  const std::string& uiLayout, std::string& error)
{
	if (!::neurus::SaveProject(path, editor, uiLayout, error))
		return false;

	m_projectPath = Utf8Path(path);
	return true;
}

command::CommandResult ApplicationCore::ExecuteScript(const command::CommandRequest& request,
                                                      Editor& editor)
{
	if (!m_script->IsReady())
	{
		return command::CommandResult::Failure(command::CommandErrorCode::Internal,
		                                      "Scripting is not initialized.");
	}
	return ExecuteScriptCommand(request, *m_script, editor, *m_preferences, ScriptProjectRoot());
}

std::string ApplicationCore::ScriptProjectRoot() const
{
	std::filesystem::path root;
	if (!m_projectPath.empty())
	{
		root = std::filesystem::path(m_projectPath).parent_path();
	}
	if (root.empty())
	{
		std::error_code error;
		root = std::filesystem::current_path(error);
		if (error)
		{
			NEURUS_ERR("[ApplicationCore] cannot resolve the working directory for the "
			           "script sandbox: " << error.message());
			return {};
		}
	}

	// Python receives UTF-8; do not rely on path::string() (ANSI on MSVC).
	const std::u8string utf8 = root.u8string();
	return std::string(utf8.begin(), utf8.end());
}

} // namespace neurus
