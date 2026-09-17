#pragma once

#include <QObject>
#include <QString>

#include "editor/events/ProjectEvents.h"
#include "editor/events/AssetEvents.h"
#include "editor/events/OperationEvents.h"
#include "editor/events/SceneEvents.h"

namespace neurus {

/**
 * @brief Singleton Qt signal bus for UI Layer ↔ Editor Layer communication.
 *
 * Owns signals related to window lifecycle, rendering triggers, and Vulkan device
 * status. These signals are Qt-dependent by design - the UI layer (QWindow,
 * QTimer, Qt event loop) emits them, and Editor/Renderer slots consume them.
 *
 * For Editor ↔ Renderer typed events (no Qt dependency), see EventBus.h.
 *
 * Usage:
 *   auto& ui = UIEvents::instance();
 *   QObject::connect(&ui, &UIEvents::newFrame, &myRenderer, &Renderer::DrawFrame);
 */
class UIEvents : public QObject
{
	Q_OBJECT

public:
	/**
	 * @brief Returns the singleton UIEvents instance.
	 * @note Thread-safe after first call (QML requires main-thread usage for MVP).
	 */
	static UIEvents& instance();

	// Prevent copies
	UIEvents(const UIEvents&) = delete;
	UIEvents& operator=(const UIEvents&) = delete;

	// Q_PROPERTY for QML access
	Q_PROPERTY(QString gpuName READ gpuName WRITE setGpuName NOTIFY gpuNameChanged)

	QString gpuName() const;
	void setGpuName(const QString& name);

signals:
	/** @brief Emitted each frame to trigger rendering. Connected to Renderer::DrawFrame(). */
	void newFrame();

	/** @brief Emitted when the application window is resized.
	 *  @param width New window width in pixels.
	 *  @param height New window height in pixels. */
	void windowResized(int width, int height);

	/** @brief Emitted when the Vulkan device is lost. Application should attempt cleanup. */
	void deviceLost();

	/** @brief Emitted by Vulkan validation layers with diagnostic messages.
	 *  @param severity "error", "warning", "info", or "verbose".
	 *  @param message Human-readable validation message. */
	void validationMessage(QString severity, QString message);

	/** @brief Emitted when GPU name changes (e.g., after device selection). */
	void gpuNameChanged();

	// --- UI-driven editor signals ---

	/** @brief Emitted when the viewport widget is recreated with a new native HWND.
	 *  @param newHwnd The new native window handle for Vulkan surface creation. */
	void uiRecreated(quintptr newHwnd);

	// --- Screenshot signals ---

	/** @brief Emitted when a screenshot is requested (F12 or menu).
	 *  Capture the current swapchain image to a PNG file. */
	void screenshotRequested();

	/** @brief Emitted when a full attachment dump is requested (Ctrl+F12).
	 *  Capture all G-Buffer attachments to individual PNG files. */
	void screenshotAllRequested();

	// --- Project file signals ---

	/** @brief Emitted when a new project is requested (Ctrl+N). */
	void projectNewRequested(const ProjectNewEvent& e);

	/** @brief Emitted when an existing project file should be opened (Ctrl+O). */
	void projectOpenRequested(const ProjectOpenEvent& e);

	/** @brief Emitted when the current project should be saved (Ctrl+S). */
	void projectSaveRequested(const ProjectSaveEvent& e);

	/** @brief Emitted when the current project should be saved to a new path (Ctrl+Shift+S). */
	void projectSaveAsRequested(const ProjectSaveAsEvent& e);

	// --- Mesh import signals ---

	/** @brief Emitted when a mesh file is selected for import (Edit → Add → Mesh...). */
	void meshImportRequested(const MeshImportEvent& e);

	/** @brief Emitted when a new camera should be added to the scene (Edit → Add → Camera). */
	void cameraAddRequested(const CameraAddEvent& e);

	/** @brief Emitted when a new light should be added to the scene (Edit → Add → Light). */
	void lightAddRequested(const LightAddEvent& e);

	/** @brief Emitted when a new sun light should be added to the scene (Edit → Add → Sun Light). */
	void sunLightAddRequested(const SunLightAddEvent& e);

	/** @brief Emitted when a new spot light should be added to the scene (Edit → Add → Spot Light). */
	void spotLightAddRequested(const SpotLightAddEvent& e);

	// --- Undo/redo signals ---

	/** @brief Emitted when the user requests undo (Edit → Undo / Ctrl+Z). */
	void undoRequested(const UndoRequested& e);

	/** @brief Emitted when the user requests redo (Edit → Redo / Ctrl+Shift+Z). */
	void redoRequested(const RedoRequested& e);

	// --- Preferences signals ---

	/** @brief Emitted when the user picks a new UI language in Preferences.
	 *  @param language Language code, e.g. "en", "zh_CN". */
	void languageChangeRequested(const QString& language);

	/** @brief Emitted when the user picks a new render-loop target FPS.
	 *  @param fps Frames per second (0 = unlimited). */
	void targetFpsChangeRequested(int fps);

public:
	/**
	 * @brief Convenience method to emit screenshotRequested from any layer.
	 * @note Prefer this over direct signal emission from outside the class.
	 */
	void requestScreenshot() { emit screenshotRequested(); }

	/**
	 * @brief Convenience method to emit screenshotAllRequested from any layer.
	 */
	void requestScreenshotAll() { emit screenshotAllRequested(); }

	// --- Project file convenience methods ---

	void requestProjectNew() { emit projectNewRequested(ProjectNewEvent{}); }
	void requestProjectOpen(const QString& path) { emit projectOpenRequested(ProjectOpenEvent{path.toStdString()}); }
	void requestProjectSave() { emit projectSaveRequested(ProjectSaveEvent{}); }
	void requestProjectSaveAs(const QString& path) { emit projectSaveAsRequested(ProjectSaveAsEvent{path.toStdString()}); }

	// --- Mesh import convenience method ---

	void requestMeshImport(const QString& path) { emit meshImportRequested(MeshImportEvent{path.toStdString()}); }

	void requestCameraAdd() { emit cameraAddRequested(CameraAddEvent{}); }
	void requestLightAdd() { emit lightAddRequested(LightAddEvent{}); }
	void requestSunLightAdd() { emit sunLightAddRequested(SunLightAddEvent{}); }
	void requestSpotLightAdd() { emit spotLightAddRequested(SpotLightAddEvent{}); }

	void requestUndo() { emit undoRequested(UndoRequested{}); }
	void requestRedo() { emit redoRequested(RedoRequested{}); }

	void requestLanguageChange(const QString& language) { emit languageChangeRequested(language); }
	void requestTargetFps(int fps) { emit targetFpsChangeRequested(fps); }

	void requestUIRecreation(quintptr newHwnd) { emit uiRecreated(newHwnd); }

private:
	UIEvents() = default;

	QString evt_gpuName;
};

} // namespace neurus
