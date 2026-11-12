/**
 * @file ApplicationSignals.cpp
 * @brief Qt signal/slot wiring and the frame tick.
 *
 * Kept out of Application.cpp so lifecycle/initialisation and command
 * interfaces stay readable. Every connection here forwards UI events into the
 * editor or the application; the frame tick drains commands, scripts and queued
 * UI events in the one order the renderer depends on.
 */

#include "app/Application.h"

#include "app/Preferences.h"
#include "app/session/SessionServer.h"
#include "core/Log.h"
#include "editor/events/ConfigEvents.h"
#include "editor/events/InputEvents.h"
#include "editor/events/SceneEvents.h"
#include "editor/events/ShaderEvents.h"
#include "editor/events/UIEvents.h"
#include "ui/UIManager.h"
#include "ui/UIContext.h"
#include "ui/panels/Outliner.h"
#include "ui/panels/PropertyPanel.h"
#include "ui/panels/RenderConfigPanel.h"
#include "ui/panels/ScriptConsolePanel.h"
#include "ui/panels/ShaderEditorPanel.h"
#include "ui/panels/Viewport.h"
#include "ui/utils/I18n.h"

#include <QCoreApplication>
#include <QDir>
#include <QObject>
#include <QString>
#include <QTimer>

#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <utility>

namespace {

/// How long a surface change may take before the new surface is parked. The
/// answer is only "my swapchain released it", so on timeout both surfaces must
/// stay alive: guessing which one the live swapchain uses would destroy a
/// surface out from under it.
constexpr auto kSurfaceChangeTimeout = std::chrono::seconds(2);

/**
 * @brief Narrows @p path to UTF-8 for a log line, without throwing.
 *
 * std::filesystem::path::string() yields the *native* narrow encoding, which MSVC
 * maps to the ANSI code page: it throws std::system_error for any character
 * outside it, and a log line is no place to lose a screenshot to an install path.
 * u8string() cannot fail that way, and on POSIX the native narrow encoding is
 * already UTF-8, so the bytes are exactly what string() produced.
 *
 * Only referenced from a NEURUS_LOG call, which compiles out in Release - hence
 * the attribute.
 */
[[maybe_unused]] static std::string utf8PathForLog(const std::filesystem::path& path)
{
	const std::u8string utf8 = path.u8string();
	return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

} // namespace

namespace neurus {

void Application::OnRunScript(const std::string& code)
{
	// Deferred: the engine queues it and the frame loop runs it. The run
	// dispatches every event synchronously as it executes, so no later drain is
	// involved - the mutation is applied before the next Python statement.
	app_script->Submit(code);
}

void Application::OnRunFile(const std::string& path)
{
	app_script->SubmitFile(path);
}

// =========================================================================
// WireSignals – Qt signal/slot connections
// =========================================================================

void Application::WireSignals()
{
	auto& uiEvents = neurus::UIEvents::instance();
	NewFrameSignals(uiEvents);
	PanelSignals(uiEvents);
	RecreateSignals(uiEvents);
	ScreenShotSignals(uiEvents);

	// --- Preferences changes: apply, persist, and retranslate via I18n. ---
	QObject::connect(&uiEvents, &neurus::UIEvents::languageChangeRequested,
	                 [this](const QString& language) {
	                     neurus::I18n::instance().setLanguage(language);
	                     app_preferences->language = language.toStdString();
	                     app_preferences->Save(neurus::Preferences::DefaultPath());
	                 });
	QObject::connect(&uiEvents, &neurus::UIEvents::targetFpsChangeRequested,
	                 [this](int fps) {
	                     app_preferences->targetFps = fps;
	                     app_preferences->Save(neurus::Preferences::DefaultPath());
	                     ApplyTargetFps(fps);
	                 });
}

void Application::ApplyTargetFps(int fps)
{
	// 0 = unlimited: let the timer fire as fast as the event loop allows.
	app_renderTimer->setInterval(fps > 0 ? 1000 / fps : 0);
}

// =========================================================================
// Project lifecycle – app-level persistence coordination
// =========================================================================

namespace
{

/// A tick that outruns a frame's budget at 60 FPS stalls the UI thread; logging
/// it separates a hitch caused by main-thread work from one caused by waiting
/// on the render thread.
constexpr auto kSlowTickMs = std::chrono::milliseconds(20);

} // anonymous namespace

// =========================================================================
// NewFrameSignals – render request + timer-driven loop
// =========================================================================

void Application::NewFrameSignals(neurus::UIEvents& uiEvents)
{
	// --- Render request (manual frame trigger); the timer below drives it ---
	QObject::connect(&uiEvents, &neurus::UIEvents::newFrame,
	                 [this]() {
	                     // The null check below does no work, so it is the only
	                     // exit that skips the measurement without hiding cost.
	                     const auto tickStart = std::chrono::steady_clock::now();

	                     if (!app_renderThread || !app_editor || !app_script)
	                     {
	                         return;
	                     }

	                     // Commands run first: their handlers dispatch events
	                     // synchronously, so mutations are visible to this tick.
	                     // The first tick rebases the UI dirty baseline on the
	                     // layout Qt actually produced; show() and the first
	                     // layout pass can still move a restored dock layout.
	                     if (app_uiBaselinePending)
	                     {
		                     app_savedUiState = app_mainWindow->ExportLayout();
		                     app_uiBaselinePending = false;
	                     }
	                     if (app_session)
	                     {
		                     app_session->DrainOnMainThread();
		                     if (app_session->CloseRequested())
		                     {
			                     app_session->WaitForWrites(std::chrono::seconds(2));
			                     app_qtApp->quit();
			                     return;
		                     }
	                     }

	                     // Run any queued scripts first: a run dispatches its
	                     // events synchronously, so its mutations are already
	                     // applied when the queued UI events drain below.
	                     // The context is generated fresh for this batch.
	                     neurus::ScriptContext scriptCtx;
	                     app_editor->UpdateScriptContext(scriptCtx);
	                     app_script->ExecutePending(scriptCtx);

	                     // A finished pick enters as a selection event, so this
	                     // tick's Edit() handles it like any other UI event.
	                     ResolvePendingPick();

	                     // Process all enqueued events (camera, gizmo, ...). Every
	                     // Post of this tick happens inside Edit(), before the
	                     // Publish below: the ordering contract in
	                     // RenderRequestSink.h.
	                     app_editor->Edit();

	                     // The frame is built per tick and moved into the
	                     // mailbox, so the render thread reads it while the main
	                     // thread refills a fresh one.
	                     neurus::RenderFrameSnapshot snapshot;
	                     app_editor->BuildSnapshot(snapshot);
	                     app_renderThread->Publish(std::move(snapshot));

	                     const neurus::RendererStatus status = app_renderThread->Status();
	                     ReconcileRenderExtent(status.extent);

	                     const neurus::HistoryView history = app_editor->GetHistory();

	                     neurus::UIContext ctx;
	                     app_editor->UpdateUIContext(ctx);
	                     // The profile belongs to the last frame the thread drew,
	                     // typically one tick behind: fine for a stats panel.
	                     ctx.profile = &status.profile;
	                     ctx.log = &neurus::LogBuffer::instance();
	                     ctx.history = &history;
	                     ctx.scriptConsole = &app_script->Console();
	                     app_mainWindow->Refresh(ctx);

	                     const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
	                         std::chrono::steady_clock::now() - tickStart);
	                     if (elapsed > kSlowTickMs)
	                     {
	                         NEURUS_LOG("[Application] slow tick " << elapsed.count() << " ms");
	                     }
	                 });

	// StartRenderLoop - timer-driven ~60 FPS render loop
	QObject::connect(app_renderTimer.get(), &QTimer::timeout, [&uiEvents]() {
		emit uiEvents.newFrame();
	});
}

// =========================================================================
// PanelSignals – Outliner selection + Viewport resize
// =========================================================================

void Application::PanelSignals(neurus::UIEvents& uiEvents)
{
	// --- UIEvents → Editor (via ConnectUIEvent → OnUIEvent → EventQueue) ---
	// --- Project file signals → Application (app-level persistence) ---
	QObject::connect(&uiEvents, &neurus::UIEvents::projectNewRequested,
	                 [this](const neurus::ProjectNewEvent&) { OnProjectNew(); });
	QObject::connect(&uiEvents, &neurus::UIEvents::projectOpenRequested,
	                 [this](const neurus::ProjectOpenEvent& e) { OnProjectOpen(e.path); });
	QObject::connect(&uiEvents, &neurus::UIEvents::projectSaveRequested,
	                 [this](const neurus::ProjectSaveEvent&) { OnProjectSave(); });
	QObject::connect(&uiEvents, &neurus::UIEvents::projectSaveAsRequested,
	                 [this](const neurus::ProjectSaveAsEvent& e) { OnProjectSave(e.path); });
	ConnectUIEvent(&uiEvents, &neurus::UIEvents::meshImportRequested);
	ConnectUIEvent(&uiEvents, &neurus::UIEvents::cameraAddRequested);
	ConnectUIEvent(&uiEvents, &neurus::UIEvents::lightAddRequested);
	ConnectUIEvent(&uiEvents, &neurus::UIEvents::sunLightAddRequested);
	ConnectUIEvent(&uiEvents, &neurus::UIEvents::spotLightAddRequested);

	// --- Undo/redo (Edit menu + shortcuts) → Editor ---
	ConnectUIEvent(&uiEvents, &neurus::UIEvents::undoRequested);
	ConnectUIEvent(&uiEvents, &neurus::UIEvents::redoRequested);

	// --- Script Console → Application (runs Python through the engine) ---
	// The console panel emits its own signals; the UIEvents requests let other
	// UI (e.g. the Tools menu) trigger a run without holding the panel.
	if (auto* console = app_mainWindow->GetPanel<neurus::ScriptConsolePanel>())
	{
		QObject::connect(console, &neurus::ScriptConsolePanel::runSnippetRequested,
		                 [this](const QString& code) { OnRunScript(code.toStdString()); });
		QObject::connect(console, &neurus::ScriptConsolePanel::runFileRequested,
		                 [this](const QString& path) { OnRunFile(path.toStdString()); });
	}
	QObject::connect(&uiEvents, &neurus::UIEvents::runScriptRequested,
	                 [this](const QString& code) { OnRunScript(code.toStdString()); });
	QObject::connect(&uiEvents, &neurus::UIEvents::runScriptFileRequested,
	                 [this](const QString& path) { OnRunFile(path.toStdString()); });

	// --- Outliner selection → Editor (via ConnectUIEvent → EventQueue) ---
	if (auto* outliner = app_mainWindow->GetPanel<neurus::Outliner>())
	{
		ConnectUIEvent(outliner, &neurus::Outliner::objectClicked);
		ConnectUIEvent(outliner, &neurus::Outliner::visibilityChanged);
		ConnectUIEvent(outliner, &neurus::Outliner::deleteRequested);
	}

	// --- Viewport signals: resize + camera control + pixel selection ---
	if (auto* viewport = app_mainWindow->GetPanel<neurus::Viewport>())
	{
		QObject::connect(viewport, &neurus::Viewport::resized,
		                 [this](int width, int height) {
		                     ResizeViewport(width, height);
		                 });

		// Forward mouse movement to Editor for camera orbit/pan/dolly
		ConnectUIEvent(viewport, &neurus::Viewport::mouseMoved);

		// Forward mouse scroll to Editor for camera zoom
		ConnectUIEvent(viewport, &neurus::Viewport::mouseScrolled);

		// Forward mouse press/release so the Editor can bound the camera drag
		// gesture (middle-button orbit/pan/dolly -> one undo entry). The
		// selection lambda below also listens to mousePressed (Left button).
		ConnectUIEvent(viewport, &neurus::Viewport::mousePressed);
		ConnectUIEvent(viewport, &neurus::Viewport::mouseReleased);

		// Forward the Delete key (remove all selected objects).
		ConnectUIEvent(viewport, &neurus::Viewport::deleteRequested);

		// Forward modal transform keystrokes (G/R/S, X/Y/Z, Return, Escape)
		// and the focus-loss abort.
		ConnectUIEvent(viewport, &neurus::Viewport::keyPressed);
		ConnectUIEvent(viewport, &neurus::Viewport::focusLost);

		// A Qt repaint of the native viewport (focus change, window uncovered)
		// does not redraw the presented image. Queue one overlay redraw so a
		// converged renderer presents again instead of leaving the client area
		// black.
		QObject::connect(viewport, &neurus::Viewport::exposed,
		                 [editor = app_editor.get()]() {
		                     editor->OnUIEvent(neurus::ViewportExposed{});
		                 });

		// Left-click: pixel-perfect object selection via the ID buffer.
		QObject::connect(viewport, &neurus::Viewport::mousePressed,
		                 [this, viewport](const neurus::MousePressEvent& e) {
		                     if (e.button != Input::MouseButton::Left)
		                     {
		                         return;
		                     }

		                     // A left click that confirms a modal transform
		                     // gesture must not re-select what is under the
		                     // cursor. Safe at frame granularity: gizmo state only
		                     // ever changes inside Edit().
		                     if (app_editor->GetViewport().IsGizmoActive())
		                     {
		                         return;
		                     }

		                     if (!app_renderThread)
		                     {
		                         return;
		                     }

		                     const int widgetW = viewport->width();
		                     const int widgetH = viewport->height();
		                     if (widgetW <= 0 || widgetH <= 0)
		                     {
		                         return;
		                     }

		                     // Normalised so the render thread maps the click onto
		                     // whatever extent its swapchain has.
		                     const float u = e.position.x / static_cast<float>(widgetW);
		                     const float v = e.position.y / static_cast<float>(widgetH);

		                     // Asynchronous: the render thread answers after its
		                     // current frame's fence, which during accumulation can
		                     // be a whole frame away (hundreds of ms in a debug
		                     // build). ResolvePendingPick() applies the answer on a
		                     // later tick; a newer click replaces this one.
		                     app_pendingPick = PendingPick{app_renderThread->PostPick(u, v),
		                                                   static_cast<int>(e.modifiers)};
		                 });
	}

	// RenderConfig changes -> Editor (via ConnectUIEvent).
	if (auto* cfgPanel = app_mainWindow->GetPanel<neurus::RenderConfigPanel>())
	{
		ConnectUIEvent(cfgPanel, &neurus::RenderConfigPanel::configValueChanged);
		ConnectUIEvent(cfgPanel, &neurus::RenderConfigPanel::editBegin);
		ConnectUIEvent(cfgPanel, &neurus::RenderConfigPanel::editEnd);
	}

	// --- Shader Editor signals → Editor ---
	if (auto* shaderPanel = app_mainWindow->GetPanel<neurus::ShaderEditorPanel>())
	{
		ConnectUIEvent(shaderPanel, &neurus::ShaderEditorPanel::compileRequested);
		ConnectUIEvent(shaderPanel, &neurus::ShaderEditorPanel::createShaderRequested);
		ConnectUIEvent(shaderPanel, &neurus::ShaderEditorPanel::codeEdited);
		ConnectUIEvent(shaderPanel, &neurus::ShaderEditorPanel::structEdited);
		ConnectUIEvent(shaderPanel, &neurus::ShaderEditorPanel::fieldAdded);
		ConnectUIEvent(shaderPanel, &neurus::ShaderEditorPanel::editBegin);
		ConnectUIEvent(shaderPanel, &neurus::ShaderEditorPanel::editEnd);
	}

	// Transform / property changes -> Editor
	if (auto* propPanel = app_mainWindow->GetPanel<neurus::PropertyPanel>())
	{
		ConnectUIEvent(propPanel, &neurus::PropertyPanel::positionChanged);
		ConnectUIEvent(propPanel, &neurus::PropertyPanel::rotationChanged);
		ConnectUIEvent(propPanel, &neurus::PropertyPanel::scaleChanged);

		// Camera properties
		ConnectUIEvent(propPanel, &neurus::PropertyPanel::cameraTargetChanged);
		ConnectUIEvent(propPanel, &neurus::PropertyPanel::cameraFovChanged);
		ConnectUIEvent(propPanel, &neurus::PropertyPanel::activeCameraChanged);

		// Mesh properties
		ConnectUIEvent(propPanel, &neurus::PropertyPanel::meshShadowChanged);
		ConnectUIEvent(propPanel, &neurus::PropertyPanel::meshMaterialChanged);

		// Light properties
		ConnectUIEvent(propPanel, &neurus::PropertyPanel::lightPowerChanged);
		ConnectUIEvent(propPanel, &neurus::PropertyPanel::lightRadiusChanged);
		ConnectUIEvent(propPanel, &neurus::PropertyPanel::lightShadowChanged);
		ConnectUIEvent(propPanel, &neurus::PropertyPanel::lightCutoffChanged);
		ConnectUIEvent(propPanel, &neurus::PropertyPanel::lightOuterCutoffChanged);

		// Environment properties
		ConnectUIEvent(propPanel, &neurus::PropertyPanel::envIntensityChanged);
		ConnectUIEvent(propPanel, &neurus::PropertyPanel::envRotationChanged);

		// Debug object properties (DebugLine / DebugPoints / DebugMesh)
		ConnectUIEvent(propPanel, &neurus::PropertyPanel::debugColorChanged);
		ConnectUIEvent(propPanel, &neurus::PropertyPanel::debugOpacityChanged);
		ConnectUIEvent(propPanel, &neurus::PropertyPanel::debugXRayChanged);
		ConnectUIEvent(propPanel, &neurus::PropertyPanel::debugLineWidthChanged);
		ConnectUIEvent(propPanel, &neurus::PropertyPanel::debugStippleChanged);
		ConnectUIEvent(propPanel, &neurus::PropertyPanel::debugPointTypeChanged);
		ConnectUIEvent(propPanel, &neurus::PropertyPanel::debugPointScaleChanged);
		ConnectUIEvent(propPanel, &neurus::PropertyPanel::debugProjectionModeChanged);
		ConnectUIEvent(propPanel, &neurus::PropertyPanel::debugPositionsChanged);
	}
}

// =========================================================================
// RecreateSignals – Viewport recreation (new window handle → new surface → new swapchain)
// =========================================================================

void Application::RecreateSignals(neurus::UIEvents& uiEvents)
{
	// New native window handle -> new surface -> new swapchain; rebind signals.
	QObject::connect(&uiEvents, &neurus::UIEvents::uiRecreated,
	                 [this](quintptr newHwnd) {
	                     // Reconnect panel signals to the new widget.
	                     PanelSignals(neurus::UIEvents::instance());

	                     if (!app_vkContext || !app_renderThread)
	                     {
	                         return;
	                     }

	                     // The surface is created on the main thread (it needs the
	                     // native window); the swapchain that uses it is rebuilt on
	                     // the render thread.
	                     std::unique_ptr<vk::raii::SurfaceKHR> new_surface;
	                     try
	                     {
	                         new_surface = std::make_unique<vk::raii::SurfaceKHR>(
	                             neurus::platform::CreateSurface(
	                                 app_vkContext->instance(),
	                                 reinterpret_cast<NativeWindowHandle>(newHwnd)));
	                     }
	                     catch (const std::exception& e)
	                     {
	                         NEURUS_ERR("Viewport recreation failed: " << e.what());
	                         return;
	                     }

	                     auto done = app_renderThread->PostSurfaceChange(*new_surface);
	                     if (done.wait_for(kSurfaceChangeTimeout) != std::future_status::ready)
	                     {
	                         // Neither surface can be proven retired: the thread may
	                         // still be drawing with one of them. Park the new one
	                         // and leave app_surface alone; both live until shutdown,
	                         // after the thread has been joined.
	                         NEURUS_ERR("[Application] Surface change timed out; parking "
	                                    "the new surface until shutdown.");
	                         app_retiredSurfaces.push_back(std::move(new_surface));
	                         return;
	                     }

	                     try
	                     {
	                         done.get();
	                     }
	                     catch (const std::exception& e)
	                     {
	                         // The swapchain state is unknown after a failure, so the
	                         // surface it might reference is never destroyed early.
	                         NEURUS_ERR("[Application] Surface change failed: " << e.what());
	                         app_retiredSurfaces.push_back(std::move(new_surface));
	                         return;
	                     }

	                     // The swapchain dropped its reference before done was
	                     // satisfied: only now is the old surface safe to destroy.
	                     app_surface = std::move(new_surface);
	                     NEURUS_LOG("[Application] Viewport recreated - new surface + swapchain");
	                 });
}

// =========================================================================
// ScreenShotSignals – screenshot + attachment dump requests
// =========================================================================

void Application::ScreenShotSignals(neurus::UIEvents& uiEvents)
{
	// Screenshots always land next to the executable, never in the process
	// working directory: a macOS launch or a shortcut sets a CWD the user does
	// not control, while the executable directory is one they can find. Only
	// this layer may ask Qt where Neurus lives; the renderer stays Qt-free by
	// receiving the resolved directory as a value with each request.
	const std::filesystem::path screenshotDir(
		QDir(QCoreApplication::applicationDirPath())
			.filePath("screenshots")
			.toStdU16String());
	// Logged through utf8PathForLog rather than path::string(): on MSVC the latter
	// throws std::system_error for a character outside the ANSI code page, and this
	// line runs outside any try block.
	NEURUS_LOG("[Application] screenshots are written under " << utf8PathForLog(screenshotDir));

	// Screenshot requests (F12 / menu action)
	QObject::connect(&uiEvents, &neurus::UIEvents::screenshotRequested,
	                 [this, screenshotDir]() {
	                     if (app_renderThread)
	                     {
	                         app_renderThread->PostScreenshot(false, screenshotDir);
	                     }
	                 });

	// Attachment dump requests (Ctrl+F12)
	QObject::connect(&uiEvents, &neurus::UIEvents::screenshotAllRequested,
	                 [this, screenshotDir]() {
	                     if (app_renderThread)
	                     {
	                         app_renderThread->PostScreenshot(true, screenshotDir);
	                     }
	                 });
}
} // namespace neurus
