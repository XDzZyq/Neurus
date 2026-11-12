#pragma once

#include <QApplication>
#include <QTimer>

#include <vulkan/vulkan_raii.hpp>

#include <cstdint>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/VulkanContext.h"
#include "app/AppArgs.h"
#include "command/ISessionState.h"
#include "command/IScriptCommands.h"
#include "command/ISnapshotCommands.h"
#include "editor/Editor.h"
#include "render/DeferredRenderer.h"
#include "render/RenderThread.h"
#include "render/UploadManager.h"
#include "script/ScriptEngine.h"
#include "app/Preferences.h"
#include "ui/UIManager.h"
#include "platform/PlatformSurface.h"

namespace neurus {

class UIEvents;
class SessionServer;
class EditorCommandAdapter;

/**
 * @brief Application lifecycle manager - fully RAII.
 *
 * The constructor creates the Qt application; Run() initializes the GPU and
 * editor subsystems, wires signals and enters the event loop. Members are
 * declared in teardown order, so reverse-order destruction releases GPU
 * resources in the required sequence.
 */
class Application : public command::ISessionState,
                    public command::IScriptCommands,
                    public command::ISnapshotCommands
{
public:
	/**
	 * @brief Creates the QApplication and render timer.
	 * @param argc Argument count.
	 * @param argv Argument vector.
	 */
	Application(int argc, char* argv[]);

	/** @brief Auto-destroy in reverse declaration order (render thread first). */
	~Application();

	Application(const Application&) = delete;
	Application& operator=(const Application&) = delete;

	/**
	 * @brief Initialises Vulkan / Editor / render loop and runs the event loop.
	 * @return Application exit code (0 on success, -1 on fatal error).
	 */
	int Run();

	// --- ISessionState (read-only view for command handlers) ---
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

	// --- IScriptCommands (script.run) ---
	command::CommandResult Execute(const command::CommandRequest& request) override;

	// --- ISnapshotCommands (snapshot.capture) ---
	command::CommandResult Capture(const std::string& path, bool all) override;

private:
	// --- Initialisation helpers (called in order by Run()) ---
	bool InitVulkan();
	bool InitRenderer();
	void InitEditor();
	void InitScripting();
	void ResizeViewport(int width, int height);

	/**
	 * @brief Aligns the viewport's physical extent with the swapchain's.
	 * @param actual Extent the render thread reports for its swapchain.
	 *
	 * ResizeViewport() can only estimate the physical extent before the
	 * swapchain rebuilds; this corrects the estimate once the real one is known.
	 */
	void ReconcileRenderExtent(const vk::Extent2D& actual);

	/// Last swapchain extent ReconcileRenderExtent() acted on (main thread only).
	vk::Extent2D app_lastReportedExtent{};

	/**
	 * @brief Applies a finished pick readback as a selection, if one is ready.
	 *
	 * Called once per UI tick, before Edit(). Never blocks: an unfinished pick
	 * stays pending for the next tick, so a click is never lost to a busy render
	 * thread and the main thread never waits on one.
	 */
	void ResolvePendingPick();

	/// A click's readback still in flight on the render thread (main thread only).
	struct PendingPick
	{
		std::future<uint32_t> objectId;   ///< Answered by the render thread.
		int                   modifiers;  ///< Input modifiers held at the click.
	};

	/// At most one: a newer click supersedes an unanswered older one.
	std::optional<PendingPick> app_pendingPick;

	void WireSignals();
	void NewFrameSignals(UIEvents& uiEvents);
	void PanelSignals(UIEvents& uiEvents);
	void RecreateSignals(UIEvents& uiEvents);
	void ScreenShotSignals(UIEvents& uiEvents);

	// --- Project lifecycle (the Application owns the project path) ---
	bool OnProjectNew(std::string* error = nullptr);
	bool OnProjectOpen(const std::string& path, std::string* error = nullptr);
	bool OnProjectSave(const std::string& path = std::string(), std::string* error = nullptr);

	// --- Scripting (Script Console → ScriptEngine) ---
	void OnRunScript(const std::string& code);
	void OnRunFile(const std::string& path);

	/**
	 * @brief Applies the target-FPS preference to the render timer.
	 * @param fps Frames per second (0 = unlimited).
	 */
	void ApplyTargetFps(int fps);

	template<typename Panel, typename Event>
	void ConnectUIEvent(
		QObject* sender,
		void (Panel::*signal)(const Event&))
	{
		QObject::connect(
			static_cast<Panel*>(sender),
			signal,
			[editor = app_editor.get()](const Event& e)
			{
				editor->OnUIEvent(e);
			}
		);
	}

	// --- Qt infrastructure (destroyed after GPU stack) ---
	std::unique_ptr<QApplication>         app_qtApp;
	std::unique_ptr<QTimer>               app_renderTimer;

	// --- Project/document state (app-level; Project itself stays stateless) ---
	std::string app_projectPath;    ///< Current project file path ("" = unsaved).
	std::string app_uiLayout;       ///< Transient UI-state blob for (de)serialization.
	std::string app_savedUiState;   ///< UI blob as last saved/loaded (dirty baseline).
	bool        app_uiBaselinePending = true;  ///< Rebase the layout on the first tick.
	std::unique_ptr<Preferences> app_preferences;  ///< ~/.neurus/preferences.json (loaded before the window).

	// --- Command session state ---
	AppArgs      app_args;          ///< Parsed --session / --project.
	std::vector<std::string> app_rawArguments;  ///< argv captured before QApplication parses it.
	std::string  app_sessionName;   ///< Named session, empty for a normal launch.
	std::chrono::steady_clock::time_point app_startTime;  ///< Session uptime origin.

	// --- GPU / UI stack (destroyed in REVERSE declaration order) ---
	// The render thread goes first: its destructor joins the thread and leaves
	// the renderer idle, so nothing below may be torn down while it runs.
	// The Editor outlives the UI: tearing down a widget can still emit a signal
	// the Application forwards to the Editor (the shader editor's highlighter
	// reports a text change from its destructor), and the Editor never points
	// into the UI, so the UI must go first.
	std::unique_ptr<VulkanContext>        app_vkContext;       // 1st declared -> destroyed 10th (LAST)
	std::unique_ptr<Editor>               app_editor;          // 2nd declared -> destroyed 9th
	/// Command-side view of the editor: outlives the session server below.
	std::unique_ptr<EditorCommandAdapter> app_editorCommands;  // 3rd declared -> destroyed 8th
	// Declared AFTER app_editor so reverse-order destruction tears the engine
	// down BEFORE the Editor: the engine may hold a ScriptContext that borrows
	// the Editor's scene/config/resources, so releasing the engine first avoids
	// dangling borrows during Python finalization. It still outlives the frame
	// loop.
	std::unique_ptr<ScriptEngine>         app_script;          // 3rd declared -> destroyed 8th
	std::unique_ptr<SessionServer>        app_session;         // 4th declared -> destroyed 7th
	std::unique_ptr<UIManager>            app_mainWindow;      // 5th declared -> destroyed 6th
	std::unique_ptr<vk::raii::SurfaceKHR> app_surface;         // 6th declared -> destroyed 5th

	/**
	 * @brief Surfaces whose swapchain use could not be proven finished.
	 *
	 * A surface-change timeout or failure leaves it unknown which surface the
	 * live swapchain still references, so the new one is parked here and the
	 * old one stays in app_surface: both live until shutdown, long after the
	 * render thread joined and the renderer went idle.
	 */
	std::vector<std::unique_ptr<vk::raii::SurfaceKHR>> app_retiredSurfaces;  // 7th -> destroyed 4th

	std::unique_ptr<UploadManager>        app_uploadManager;   // 8th declared -> destroyed 3rd
	std::unique_ptr<DeferredRenderer>     app_renderer;        // 9th declared -> destroyed 2nd
	std::unique_ptr<RenderThread>         app_renderThread;    // 10th declared -> destroyed 1st (FIRST)
};

} // namespace neurus
