#pragma once

#include "AppArgs.h"

#include "command/IScriptCommands.h"
#include "command/ISessionState.h"
#include "command/ISnapshotCommands.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace neurus
{

class ApplicationCore;
class DeferredRenderer;
class Editor;
class EditorCommandAdapter;
class RenderThread;
class SessionServer;
class UploadManager;
class VulkanContext;

/**
 * @brief Qt-free application: a windowless renderer driven by the command CLI.
 *
 * Owns the same object graph the Qt application does, minus the UI, the window
 * surface and the swapchain: an ApplicationCore (preferences, script engine,
 * open project), a headless Vulkan context, the deferred renderer on an
 * offscreen target, the editor, the render thread and the session server.
 * It exposes no window and renders only when a command asks for an image, but
 * it keeps publishing frames so a capture always sees converged output.
 *
 * Command surface: `session.*`, `project.*`, `scene.*`, `object.*`, `render.*`,
 * `history.*`, `script.*` and `snapshot.*` behave exactly as in the GUI.
 */
class HeadlessApplication final : public command::ISessionState,
                                  public command::IScriptCommands,
                                  public command::ISnapshotCommands
{
public:
	/**
	 * @param arguments Raw command line, including the program name.
	 */
	explicit HeadlessApplication(std::vector<std::string> arguments);
	~HeadlessApplication() override;

	HeadlessApplication(const HeadlessApplication&) = delete;
	HeadlessApplication& operator=(const HeadlessApplication&) = delete;

	/** @brief Runs until the session closes, a signal arrives, or startup fails. */
	int Run();

	// --- command::ISessionState ---
	int Pid() const override;
	std::string SessionName() const override;
	std::string ProjectPath() const override;
	bool IsDirty() const override;
	bool Save(std::string& error) override;
	bool SaveAs(const std::string& path, std::string& error) override;
	bool NewProject(bool force, std::string& error) override;
	bool OpenProject(const std::string& path, bool force, std::string& error) override;
	int SceneObjectCount() const override;
	std::int64_t UptimeSeconds() const override;
	std::string AppVersion() const override;

	// --- command::ISnapshotCommands ---
	command::CommandResult Capture(const std::string& path, bool all) override;

	// --- command::IScriptCommands ---
	command::CommandResult Execute(const command::CommandRequest& request) override;

private:
	AppArgs                     m_args;
	std::chrono::steady_clock::time_point m_startTime;

	// Declared so destruction runs in dependency order (reverse of this list):
	// the render thread joins before the renderer, the renderer before the
	// upload manager; the core (script engine) outlives the editor it writes to,
	// and the Vulkan context goes last.
	std::unique_ptr<ApplicationCore>      m_core;
	std::unique_ptr<VulkanContext>        m_vkContext;
	std::unique_ptr<Editor>               m_editor;
	std::unique_ptr<EditorCommandAdapter> m_commands;
	std::unique_ptr<SessionServer>        m_session;
	std::unique_ptr<UploadManager>        m_uploader;
	std::unique_ptr<DeferredRenderer>     m_renderer;
	std::unique_ptr<RenderThread>         m_renderThread;
};

} // namespace neurus
