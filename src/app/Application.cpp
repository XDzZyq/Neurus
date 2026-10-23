/**
 * @file Application.cpp
 * @brief Application lifecycle implementation.
 *
 * Run() initializes Vulkan, the renderer, the upload manager, the editor and
 * the main window, loads or creates the project, then enters the Qt event
 * loop. Teardown is handled by C++ member destruction in reverse declaration
 * order.
 */

#include "app/Application.h"

#include "VulkanContext.h"
#include "core/Log.h"
#include "editor/Editor.h"
#include "editor/events/SceneEvents.h"
#include "editor/events/ShaderEvents.h"
#include "editor/events/UIEvents.h"
#include "editor/events/ConfigEvents.h"
#include "editor/events/InputEvents.h"
#include "asset/Project.h"
#include "asset/components/SceneComponent.h"
#include "asset/components/ResourceComponent.h"
#include "asset/components/ConfigComponent.h"
#include "asset/components/UIComponent.h"
#include "asset/components/EditorComponent.h"
#include "asset/components/HistoryComponent.h"
#include "scene/Scene.h"
#include "render/DeferredRenderer.h"
#include "render/RenderCache.h"
#include "render/RenderFrameSnapshot.h"
#include "render/Texture.h"
#include "render/shaders/ShaderLibrary.h"
#include "ui/UIManager.h"
#include "ui/UIContext.h"
#include "ui/utils/I18n.h"
#include "app/Preferences.h"
#include "ui/panels/Outliner.h"
#include "ui/panels/PropertyPanel.h"
#include "ui/panels/RenderConfigPanel.h"
#include "ui/panels/ShaderEditorPanel.h"
#include "ui/panels/Viewport.h"

#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QTimer>

#include <chrono>
#include <cmath>
#include <future>

#include <iostream>
#include <memory>
#include <utility>

namespace {

/// Starter mesh of the default scene, shared by the first-launch fallback and
/// File > New so the two cannot drift. Relative, so project files stay portable.
constexpr const char* kDefaultSceneObj = "res/obj/sphere.obj";

/// How long a pick readback may take before the click is abandoned and the
/// current selection kept. One frame plus the readback is a few milliseconds;
/// 250 ms still covers a debug build with validation enabled, without letting a
/// hung GPU freeze the UI thread indefinitely.
constexpr auto kPickTimeout = std::chrono::milliseconds(250);

/// How long a surface change may take before the new surface is parked. The
/// answer is only "my swapchain released it", so on timeout both surfaces must
/// stay alive: guessing which one the live swapchain uses would destroy a
/// surface out from under it.
constexpr auto kSurfaceChangeTimeout = std::chrono::seconds(2);

/**
 * @brief Resolves a resource path relative to the executable directory.
 *
 * The res/ folder is copied alongside the executable at build time.
 * @param relativePath Path relative to res/ (e.g., "obj/sphere.obj").
 * @return Absolute path to the resource.
 */
static QString resolveResourcePath(const char* relativePath)
{
	return QCoreApplication::applicationDirPath() + "/res/" + relativePath;
}

// Registers the live components on every call. Order matters: the resource
// pool is registered first, before the scene resolves references against it.
static void BuildProject(neurus::project::Project& proj,
                         neurus::Editor& editor,
                         std::string& uiLayout)
{
	// Resource pool first: the Scene's ID references resolve against it.
	proj.Register<neurus::project::ResourceComponent>(editor.GetResourceManager());
	proj.Register<neurus::project::SceneComponent>(editor.GetScene(),
	                                                editor.GetResourceManager());
	proj.Register<neurus::project::ConfigComponent>(editor.GetRenderConfig());
	proj.Register<neurus::project::UIComponent>(uiLayout);
	// History, then editor view state: both are appended-last nodes, so legacy
	// files that lack them load cleanly (each Load() falls back).
	proj.Register<neurus::project::HistoryComponent>(editor.GetOperations());
	proj.Register<neurus::project::EditorComponent>(editor.GetViewport());
}

} // anonymous namespace

namespace neurus {

// =========================================================================
// Constructor / Destructor
// =========================================================================

Application::Application(int argc, char* argv[])
{
	// --- Qt Application ---
	app_qtApp = std::make_unique<QApplication>(argc, argv);
	app_renderTimer = std::make_unique<QTimer>();
	app_renderTimer->setInterval(16);  // ~60 FPS
	app_qtApp->setApplicationName("Neurus");
	app_qtApp->setApplicationVersion("0.1.0");
}

Application::~Application()
{
}

// =========================================================================
// Run() – orchestration
// =========================================================================

int Application::Run()
{
	// --- Preferences: the Application is their sole manager. Loaded BEFORE the
	// window is built so the UI starts in the saved language; the file is
	// created on first run. ---
	app_preferences = std::make_unique<neurus::Preferences>();
	const std::string prefsPath = neurus::Preferences::DefaultPath();

	// "auto" stays stored verbatim: nothing here rewrites the preference with
	// a concrete code, which would erase the "follow the system" choice.
	switch (app_preferences->Load(prefsPath))
	{
	case neurus::Preferences::LoadResult::Ok:
		break;
	case neurus::Preferences::LoadResult::Missing:
		app_preferences->Save(prefsPath);  // First run: create the file.
		break;
	case neurus::Preferences::LoadResult::Corrupt:
		// Defaults are in effect, but the file is NOT overwritten here: move it
		// aside first so the aboutToQuit save below cannot clobber the evidence.
		NEURUS_ERR("[Application] " << prefsPath << " is unreadable; starting "
		           "with default preferences.");
		neurus::Preferences::Backup(prefsPath);
		break;
	}

	neurus::I18n::instance().setLanguage(
		QString::fromStdString(app_preferences->language));
	ApplyTargetFps(app_preferences->targetFps);

	// Persist preferences once more on clean shutdown (safety net; changes
	// are also saved eagerly on every edit).
	QObject::connect(app_qtApp.get(), &QCoreApplication::aboutToQuit,
	                 [this]() { app_preferences->Save(neurus::Preferences::DefaultPath()); });

	if (!InitVulkan())
	{
		return -1;
	}

	if (!InitRenderer())
	{
		return -1;
	}

	// Everything from here on reaches the renderer through the thread. It is not
	// started yet, and the two setup calls below stay direct: at this point no
	// other thread touches the renderer.
	app_renderThread = std::make_unique<neurus::RenderThread>(*app_renderer);

	// --- Create UploadManager on the upload queue (compute + transfer: it also runs IBL) ---
	app_uploadManager = std::make_unique<neurus::UploadManager>(
		app_vkContext->device(),
		app_vkContext->physicalDevice(),
		app_vkContext->transferQueue(),
		app_vkContext->transferQueueFamily());

	// --- Wire LightingCache to RenderCache via transfer queue ---
	// Pre-Start, main thread only: the render thread is not running yet, so
	// these setup calls need no control message.
	app_renderer->GetRenderCache().SetLightingCache(
		app_uploadManager->CreateLightingCache(
			app_vkContext->device(),
			app_vkContext->physicalDevice()));

	// The renderer executes every Editor-posted GPU request itself and needs the
	// upload service to do it. Non-owning: the renderer is destroyed before the
	// UploadManager (see Application.h teardown order), so the pointer stays
	// valid for the renderer's whole lifetime.
	app_renderer->SetUploadManager(app_uploadManager.get());

	// Now renderer + uploadManager exist - safe to create Editor
	InitEditor();

	const auto projectPath = resolveResourcePath("shadow.neurus.json").toStdString();
	const std::string objPath = kDefaultSceneObj;

	try
	{
		// Open the existing project (scene, config and UI layout all live in
		// it). Inlined rather than OnProjectOpen so a missing file throws
		// through to the default-project fallback below.
		app_editor->BeginLoad();
		neurus::project::Project proj;
		BuildProject(proj, *app_editor, app_uiLayout);
		proj.Load(projectPath);
		app_editor->FinishLoad();
		app_mainWindow->ApplyLayout(app_uiLayout);
		app_projectPath = projectPath;
		app_savedUiState = app_uiLayout;
		NEURUS_LOG("[Application] Loaded project: " << projectPath);
	}
	catch (const std::exception& e)
	{
		NEURUS_LOG("[Application] Project file not found, creating default: " << e.what());
		// Same two steps as File > New: an empty document, then the default
		// content. The upload waits until after show(), below.
		app_editor->NewScene();
		app_editor->CreateDefaultScene(objPath);
		// Save for future runs (captures the current default UI layout too).
		// This also rebases the dirty baseline.
		OnProjectSave(projectPath);
	}

	app_mainWindow->show();
	ResizeViewport(app_mainWindow->getViewportWidth(), app_mainWindow->getViewportHeight());

	// Upload scene GPU resources AFTER the window is shown and the surface is
	// ready: doing it earlier causes "Surface lost during recreation" on the
	// first frame because the swapchain is not fully ready.
	app_editor->UploadSceneResources();

	// Requests and resizes posted before this point are queued and serviced by
	// the thread's first iteration, so nothing posted above is lost.
	app_renderThread->Start();

	WireSignals();

	NEURUS_LOG("[Application] Entering event loop");
	app_renderTimer->start();
	int result = app_qtApp->exec();

	// Explicit: join here, while every member the thread touches still exists.
	// The destructor's Stop() then has nothing left to do.
	app_renderThread->Stop();
	return result;
}

// =========================================================================
// InitVulkan – two-phase Vulkan initialisation (Instance → Window → Surface → Device)
// =========================================================================

bool Application::InitVulkan()
{
	auto& uiEvents = neurus::UIEvents::instance();

	try
	{
		app_platform = CreatePlatformSurface();
		auto vkInstance = neurus::VulkanContext::CreateInstance(*app_platform);
		app_vkContext = std::make_unique<neurus::VulkanContext>(std::move(vkInstance));

		// The window is seeded with plain preference values (language / FPS /
		// file path); the UI layer never touches this type.
		app_mainWindow = std::make_unique<neurus::UIManager>(
			QString::fromStdString(app_preferences->language),
			app_preferences->targetFps,
			QString::fromStdString(neurus::Preferences::DefaultPath()));
		app_mainWindow->show();  // Must show before surface creation on macOS

		NativeWindowHandle hwnd = app_mainWindow->getViewportHwnd();
		app_surface = std::make_unique<vk::raii::SurfaceKHR>(
			app_platform->createSurface(app_vkContext->instance(), hwnd));

		app_vkContext->InitDevice();

		uiEvents.setGpuName(QString::fromStdString(app_vkContext->gpuName()));
	}
	catch (const std::exception& e)
	{
		NEURUS_ERR("Vulkan initialization failed: " << e.what());
		return false;
	}

	try
	{
		app_vkContext->InitQueue(*app_surface);
	}
	catch (const std::exception& e)
	{
		NEURUS_ERR("Queue initialization failed: " << e.what());
		return false;
	}

	return true;
}

// =========================================================================
// InitRenderer – deferred renderer + light upload
// =========================================================================

bool Application::InitRenderer()
{
	try
	{
		app_renderer = std::make_unique<neurus::DeferredRenderer>(
			app_vkContext->device(),
			app_vkContext->physicalDevice(),
			app_vkContext->graphicsQueue(),
			app_vkContext->graphicsQueueFamily(),
			*app_surface,
			app_mainWindow->getViewportWidth(),
			app_mainWindow->getViewportHeight()
		);
	}
	catch (const std::exception& e)
	{
		NEURUS_ERR("DeferredRenderer initialization failed: " << e.what());
		return false;
	}

	return true;
}

void Application::ResizeViewport(int width, int height)
{
	// AppKit must see the resize on the main thread; the swapchain itself is
	// rebuilt on the render thread.
	app_platform->onResize(
		app_mainWindow->getViewportHwnd(),
		static_cast<uint32_t>(width), static_cast<uint32_t>(height));
	app_renderThread->PostResize(
		static_cast<uint32_t>(width), static_cast<uint32_t>(height));

	// The swapchain extent is no longer known synchronously, so the physical
	// extent is estimated the way the platform layer sizes the Metal drawable:
	// logical size times the widget's device pixel ratio. ReconcileRenderExtent()
	// replaces the estimate once the swapchain reports its own extent.
	float ratio = 1.0f;
	if (auto* viewport = app_mainWindow->GetPanel<neurus::Viewport>())
	{
		ratio = static_cast<float>(viewport->devicePixelRatioF());
	}

	const auto toPhysical = [ratio](int logical) {
		const int physical = static_cast<int>(std::lround(static_cast<double>(logical) * ratio));
		return static_cast<uint32_t>(physical > 0 ? physical : 1);
	};

	app_editor->GetViewport().HandleResize(
		glm::uvec2{static_cast<uint32_t>(width), static_cast<uint32_t>(height)},
		glm::uvec2{toPhysical(width), toPhysical(height)});
}

void Application::ReconcileRenderExtent(const vk::Extent2D& actual)
{
	if (!app_editor || actual.width == 0 || actual.height == 0)
	{
		return;
	}

	// Act only when the thread reports a NEW extent. During a drag the reported
	// one lags the latest estimate by a resize or two; re-applying that stale
	// value every tick would overwrite each fresh estimate and make the DPI ratio
	// flicker. A new report means the thread just applied a resize, and the last
	// one after the drag settles is the truth.
	if (actual == app_lastReportedExtent)
	{
		return;
	}
	app_lastReportedExtent = actual;

	neurus::EditorViewport& viewport = app_editor->GetViewport();

	// A zero logical size means the panel is not laid out yet: nothing to match.
	const glm::uvec2 logical = viewport.Size();
	if (logical.x == 0 || logical.y == 0)
	{
		return;
	}

	// The swapchain can clamp or round what the estimate assumed; its own
	// extent is the truth, and the viewport derives its DPI ratio from it.
	const glm::uvec2 render = viewport.RenderExtent();
	if (render.x == actual.width && render.y == actual.height)
	{
		return;
	}

	viewport.HandleResize(logical, glm::uvec2{actual.width, actual.height});
}

// =========================================================================
// InitEditor – editor with project ownership transfer
// =========================================================================

void Application::InitEditor()
{
	// Every Editor request now goes through the thread's Post, which also wakes
	// the thread: a request posted while no frame is coming is still serviced.
	app_editor = std::make_unique<neurus::Editor>(app_renderThread.get());
	app_editor->Initialize();
	NEURUS_LOG("[Application] Editor initialized, IBL handled by Editor");
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

void Application::OnProjectNew()
{
	// File > New is two editor steps: reset the document to empty, then fill
	// it with the same default content a first launch gets. Keeping them apart
	// is what lets NewScene() mean "empty" while the menu still hands the user
	// something to look at.
	try
	{
		app_editor->NewScene();
		app_editor->CreateDefaultScene(kDefaultSceneObj);
		// That content IS the new document's baseline, not an unsaved edit.
		app_editor->ClearDirty();
		// Safe here: the window is already shown.
		app_editor->UploadSceneResources();
	}
	catch (const std::exception& e)
	{
		NEURUS_ERR("Failed to create new project: " << e.what());
	}

	app_projectPath.clear();
	// Layout is intentionally left untouched on New; rebase the dirty
	// baseline so a fresh project isn't reported as having unsaved changes.
	app_savedUiState = app_mainWindow->ExportLayout();
}

void Application::OnProjectOpen(const std::string& path)
{
	try
	{
		app_editor->BeginLoad();
		neurus::project::Project proj;
		BuildProject(proj, *app_editor, app_uiLayout);
		proj.Load(path);
		app_editor->FinishLoad();
		app_mainWindow->ApplyLayout(app_uiLayout);
		app_projectPath = path;
		app_savedUiState = app_uiLayout;
	}
	catch (const std::exception& e)
	{
		NEURUS_ERR("[Application] Failed to open project: " << e.what());
	}
}

void Application::OnProjectSave(const std::string& path)
{
	const std::string target = path.empty() ? app_projectPath : path;
	if (target.empty())
	{
		NEURUS_ERR("[Application] Save requested but no project path is set.");
		return;
	}

	try
	{
		app_uiLayout = app_mainWindow->ExportLayout();
		neurus::project::Project proj;
		BuildProject(proj, *app_editor, app_uiLayout);
		proj.Save(target);
		app_projectPath = target;
		app_savedUiState = app_uiLayout;
		app_editor->ClearDirty();
	}
	catch (const std::exception& e)
	{
		NEURUS_ERR("[Application] Failed to save project: " << e.what());
	}
}

bool Application::IsDirty() const
{
	return app_editor->IsDirty() || (app_mainWindow->ExportLayout() != app_savedUiState);
}

// =========================================================================
// NewFrameSignals – render request + timer-driven loop
// =========================================================================

void Application::NewFrameSignals(neurus::UIEvents& uiEvents)
{
	// --- Render request (manual frame trigger); the timer below drives it ---
	QObject::connect(&uiEvents, &neurus::UIEvents::newFrame,
	                 [this]() {
	                     if (!app_renderThread || !app_editor)
	                     {
	                         return;
	                     }

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
	                     app_mainWindow->Refresh(ctx);
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

		// Left-click: pixel-perfect object selection via the ID buffer.
		QObject::connect(viewport, &neurus::Viewport::mousePressed,
		                 [this, viewport](const neurus::MousePressEvent& e) {
		                     if (e.button != Input::MouseButton::Left)
		                     {
		                         return;
		                     }

		                     // A left click that confirms a modal transform
		                     // gesture must not re-select what is under the
		                     // cursor, nor wait for this path's readback: the main
		                     // thread no longer idles the queue, it waits on a
		                     // render-thread readback. Safe at frame granularity:
		                     // gizmo state only ever changes inside Edit().
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

		                     auto objectId = app_renderThread->PostPick(u, v);
		                     if (objectId.wait_for(kPickTimeout) != std::future_status::ready)
		                     {
		                         NEURUS_ERR("[Application] Pick readback timed out; "
		                                    "keeping the current selection.");
		                         return;
		                     }

		                     uint32_t objectID = 0;
		                     try
		                     {
		                         objectID = objectId.get();
		                     }
		                     catch (const std::exception& ex)
		                     {
		                         // Broken promise (thread stopped) or a readback
		                         // error inside the renderer.
		                         NEURUS_ERR("[Application] Pick readback failed: " << ex.what());
		                         return;
		                     }

		                     // objectID 0 means background (clears the selection).
		                     ObjectSelected selEvent {
		                         static_cast<int>(objectID),
		                         static_cast<int>(e.modifiers)
		                     };
		                     app_editor->OnUIEvent(selEvent);
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
	                             app_platform->createSurface(
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
	// Screenshot requests (F12 / menu action)
	QObject::connect(&uiEvents, &neurus::UIEvents::screenshotRequested,
	                 [this]() {
	                     if (app_renderThread)
	                     {
	                         app_renderThread->PostScreenshot(false);
	                     }
	                 });

	// Attachment dump requests (Ctrl+F12)
	QObject::connect(&uiEvents, &neurus::UIEvents::screenshotAllRequested,
	                 [this]() {
	                     if (app_renderThread)
	                     {
	                         app_renderThread->PostScreenshot(true);
	                     }
	                 });
}
} // namespace neurus
