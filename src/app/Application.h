#pragma once

#include <QApplication>
#include <QTimer>

#include <vulkan/vulkan_raii.hpp>

#include <memory>
#include <vector>

#include "app/VulkanContext.h"
#include "editor/Editor.h"
#include "render/DeferredRenderer.h"
#include "render/RenderThread.h"
#include "render/UploadManager.h"
#include "app/Preferences.h"
#include "ui/UIManager.h"
#include "platform/PlatformSurface.h"

namespace neurus {

class UIEvents;

/**
 * @brief Application lifecycle manager - fully RAII.
 *
 * The constructor creates the Qt application; Run() initializes the GPU and
 * editor subsystems, wires signals and enters the event loop. Members are
 * declared in teardown order, so reverse-order destruction releases GPU
 * resources in the required sequence.
 */
class Application
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

private:
	// --- Initialisation helpers (called in order by Run()) ---
	bool InitVulkan();
	bool InitRenderer();
	void InitEditor();
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

	void WireSignals();
	void NewFrameSignals(UIEvents& uiEvents);
	void PanelSignals(UIEvents& uiEvents);
	void RecreateSignals(UIEvents& uiEvents);
	void ScreenShotSignals(UIEvents& uiEvents);

	// --- Project lifecycle (the Application owns the project path) ---
	void OnProjectNew();
	void OnProjectOpen(const std::string& path);
	void OnProjectSave(const std::string& path = std::string());

	/** @brief Aggregated document dirtiness: editor scene/config OR UI-state drift. */
	bool IsDirty() const;

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

	// --- Platform abstraction (destroyed after everything else) ---
	std::unique_ptr<PlatformSurface>      app_platform;

	// --- Qt infrastructure (destroyed after GPU stack) ---
	std::unique_ptr<QApplication>         app_qtApp;
	std::unique_ptr<QTimer>               app_renderTimer;

	// --- Project/document state (app-level; Project itself stays stateless) ---
	std::string app_projectPath;    ///< Current project file path ("" = unsaved).
	std::string app_uiLayout;       ///< Transient UI-state blob for (de)serialization.
	std::string app_savedUiState;   ///< UI blob as last saved/loaded (dirty baseline).
	std::unique_ptr<Preferences> app_preferences;  ///< ~/.neurus/preferences.json (loaded before the window).

	// --- GPU / UI stack (destroyed in REVERSE declaration order) ---
	// The render thread goes first: its destructor joins the thread and leaves
	// the renderer idle, so nothing below may be torn down while it runs.
	// The Editor outlives the UI: tearing down a widget can still emit a signal
	// the Application forwards to the Editor (the shader editor's highlighter
	// reports a text change from its destructor), and the Editor never points
	// into the UI, so the UI must go first.
	std::unique_ptr<VulkanContext>        app_vkContext;       // 1st declared -> destroyed 8th (LAST)
	std::unique_ptr<Editor>               app_editor;          // 2nd declared -> destroyed 7th
	std::unique_ptr<UIManager>            app_mainWindow;      // 3rd declared -> destroyed 6th
	std::unique_ptr<vk::raii::SurfaceKHR> app_surface;         // 4th declared -> destroyed 5th

	/**
	 * @brief Surfaces whose swapchain use could not be proven finished.
	 *
	 * A surface-change timeout or failure leaves it unknown which surface the
	 * live swapchain still references, so the new one is parked here and the
	 * old one stays in app_surface: both live until shutdown, long after the
	 * render thread joined and the renderer went idle.
	 */
	std::vector<std::unique_ptr<vk::raii::SurfaceKHR>> app_retiredSurfaces;  // 5th -> destroyed 4th

	std::unique_ptr<UploadManager>        app_uploadManager;   // 6th declared -> destroyed 3rd
	std::unique_ptr<DeferredRenderer>     app_renderer;        // 7th declared -> destroyed 2nd
	std::unique_ptr<RenderThread>         app_renderThread;    // 8th declared -> destroyed 1st (FIRST)
};

} // namespace neurus
