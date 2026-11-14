#pragma once

#include "command/CommandTypes.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace neurus
{

class Editor;
class Preferences;
class ScriptEngine;

/**
 * @brief Behaviour both composition roots share.
 *
 * The Qt application and the headless renderer own the same three things beyond
 * their GPU stack: the preferences file, the embedded script engine and the open
 * project (with its session name). This class owns them and the operations that
 * act on them, so the CLI sees one implementation on every platform:
 *
 *   - application: ApplicationCore + QApplication + UIManager + surface + Qt signals
 *   - headless:    ApplicationCore + offscreen target + the session loop
 *
 * It is composition, not a base class: each root owns one instance and keeps its
 * own Editor, renderer and event loop. Nothing here touches Qt, the UI or Vulkan.
 */
class ApplicationCore
{
public:
	ApplicationCore();
	~ApplicationCore();

	ApplicationCore(const ApplicationCore&) = delete;
	ApplicationCore& operator=(const ApplicationCore&) = delete;

	/**
	 * @brief Loads the persisted preferences, moving a corrupt file aside.
	 *
	 * A missing file is not an error; the defaults apply.
	 */
	void LoadPreferences();

	Preferences& Prefs() { return *m_preferences; }
	const Preferences& Prefs() const { return *m_preferences; }

	ScriptEngine& Script() { return *m_script; }
	const ScriptEngine& Script() const { return *m_script; }

	/**
	 * @brief Wires the engine to @p editor and starts the interpreter.
	 *
	 * Scripted mutations go through the editor's own event queue, so a script
	 * write is applied by the same controller as the equivalent panel edit.
	 *
	 * @param preload Packages imported before the first frame.
	 * @return False when the interpreter is not ready; @p preload is then skipped.
	 */
	bool InitializeScripting(Editor& editor, const std::vector<std::string>& preload);

	/**
	 * @brief Empties @p editor and loads the default content into it.
	 * @param objPath Mesh the default scene is built around.
	 */
	bool CreateDefaultDocument(Editor& editor, const std::filesystem::path& objPath,
	                           std::string& error);

	/**
	 * @brief Empties @p editor and loads a project file into it.
	 *
	 * A failure leaves the editor empty (BeginLoad already dropped the previous
	 * document), so the caller decides whether to restore default content.
	 * On success the project path becomes the script sandbox root.
	 */
	bool LoadProject(const std::filesystem::path& path, Editor& editor,
	                 std::string& uiLayout, std::string& error);

	/**
	 * @brief Saves @p editor to @p path and clears the dirty flag.
	 * @param uiLayout Opaque UI layout blob; empty for a headless caller.
	 */
	bool SaveProject(const std::filesystem::path& path, Editor& editor,
	                 const std::string& uiLayout, std::string& error);

	/**
	 * @brief Runs one `script.*` command against @p editor.
	 *
	 * External commands always execute under the host sandbox policy; the CLI
	 * cannot choose trusted mode.
	 */
	command::CommandResult ExecuteScript(const command::CommandRequest& request, Editor& editor);

	const std::string& ProjectPath() const { return m_projectPath; }
	void SetProjectPath(std::string path) { m_projectPath = std::move(path); }

	const std::string& SessionName() const { return m_sessionName; }
	void SetSessionName(std::string name) { m_sessionName = std::move(name); }

private:
	/// Directory the script sandbox roots default to: the project directory.
	std::string ScriptProjectRoot() const;

	std::unique_ptr<Preferences> m_preferences;
	std::unique_ptr<ScriptEngine> m_script;
	std::string m_projectPath;
	std::string m_sessionName;
};

} // namespace neurus
