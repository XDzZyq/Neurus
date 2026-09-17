#pragma once

#include <QApplication>
#include <QTimer>

#include <vulkan/vulkan_raii.hpp>

#include <memory>

#include "app/VulkanContext.h"
#include "editor/Editor.h"
#include "render/DeferredRenderer.h"
#include "render/Screenshot.h"
#include "render/UploadManager.h"
#include "app/Preferences.h"
#include "ui/UIManager.h"
#include "platform/PlatformSurface.h"

namespace neurus {

class UIEvents;

/**
 * @brief Application lifecycle manager – fully RAII.
 *
 * Constructor creates the Qt application.  Run() initialises all GPU /
 * Editor subsystems, wires signals, and enters the event loop.  The
 * destructor relies on C++ reverse-order member destruction to tear
 * down GPU resources in the required order:
 *   renderer → editor → screenshot → surface → mainWindow → vkContext → platform
 *
 * Usage:
 * @code
 *   neurus::Application app(argc, argv);
 *   return app.Run();
 * @endcode
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

	/** @brief Auto-destroy in reverse declaration order (renderer first). */
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
	void WireSignals();
	void NewFrameSignals(UIEvents& uiEvents);
	void PanelSignals(UIEvents& uiEvents);
	void RecreateSignals(UIEvents& uiEvents);
	void ScreenShotSignals(UIEvents& uiEvents);

	// --- Project lifecycle (app-level persistence coordination) ---
	// Application owns the project path and orchestrates save/open across
	// Editor (scene/config) and UIManager (UI-state blob).
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

	// --- GPU / UI stack (destroyed in REVERSE order: renderer first, vkContext last) ---
	// Screenshot holds refs to RenderCache (owned by DeferredRenderer) — must be destroyed BEFORE renderer.
	std::unique_ptr<VulkanContext>        app_vkContext;       // 1st declared → destroyed 6th (LAST)
	std::unique_ptr<UIManager>            app_mainWindow;      // 2nd declared → destroyed 5th
	std::unique_ptr<vk::raii::SurfaceKHR> app_surface;         // 3rd declared → destroyed 4th
	std::unique_ptr<Screenshot>           app_screenshot;      // 4th declared → destroyed 3rd
	std::unique_ptr<Editor>               app_editor;          // 5th declared → destroyed 2nd
	std::unique_ptr<UploadManager>        app_uploadManager;   // 6th declared → destroyed 1.5th
	std::unique_ptr<DeferredRenderer>     app_renderer;        // 7th declared → destroyed 1st (FIRST)
};

} // namespace neurus
