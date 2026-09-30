/**
 * @file Application.cpp
 * @brief Application lifecycle implementation.
 *
 * Initialization sequence:
 *   1. QApplication - Qt event loop (Widgets)
 *   2. EventBus - singleton cross-layer communication
 *   3. VulkanContext (Phase 1) - VkInstance creation
 *   4. UIManager + Viewport - Qt window with dockable Viewport
 *   5. Show main window - apply layout so widget has final size
 *   6. VkSurfaceKHR - created from Viewport's native window handle via PlatformSurface
 *   7. VulkanContext (Phase 2) - logical device + queue selection
 *   8. Project::Open/Project::CreateDefault - load or create project scene + load BAKED.png texture
 *   9. DeferredRenderer - swapchain, G-Buffer, geometry pass, lighting pass, composite
 *  10. QTimer-driven render loop - ~60 FPS
 *
 * Cleanup: C++ member destruction in reverse declaration order handles
 *   renderer → editor → screenshot → surface → mainWindow → vkContext automatically.
 */

// Must define platform before including any Vulkan headers
// (Platform defines removed — PlatformSurface handles this)

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
#include "render/RenderContext.h"
#include "render/Screenshot.h"
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

#include <cmath>

#include <iostream>
#include <memory>

namespace {

/// Starter mesh of the default scene, shared by the first-launch fallback and
/// File > New so the two cannot drift. Relative: pooled resources store
/// relative "res/..." paths so project files stay portable, and the asset layer
/// resolves them against the res dir.
constexpr const char* kDefaultSceneObj = "res/obj/sphere.obj";

/**
 * @brief Resolves a resource path relative to the executable directory.
 *
 * The res/ folder is copied alongside the executable by CMake at build time.
 * Resources are resolved as: {exeDir}/res/{relativePath}
 *
 * @param relativePath Path relative to res/ (e.g., "obj/sphere.obj").
 * @return Absolute path to the resource.
 */
static QString resolveResourcePath(const char* relativePath)
{
	return QCoreApplication::applicationDirPath() + "/res/" + relativePath;
}

// Registers the cross-layer components against live data each call:
// Scene + RenderConfig (Editor-owned), the ResourceManager pool (Editor-owned,
// registered FIRST so it deserializes before the Scene resolves references),
// and the UI-state blob (Application-owned).
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
	// files that lack them load cleanly (each Load() falls back instead of
	// throwing). Registration order IS archive read order, so nothing already
	// written moves when a node is appended here.
	proj.Register<neurus::project::HistoryComponent>(editor.GetOperations());
	proj.Register<neurus::project::EditorComponent>(editor);
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
	// ShaderLibrary no longer caches shaders — no explicit cleanup needed.
	// vk::raii::ShaderModule objects created during pipeline building are
	// temporary and destroyed before BuildPipeline returns.
	// The unique_ptr<Shader> members in each pass are cleaned up during
	// normal member destruction.
}

// =========================================================================
// Run() – orchestration
// =========================================================================

int Application::Run()
{
	// --- App-level preferences: the Application is their sole manager. Load
	// BEFORE the window is built so the UI starts in the saved language, and
	// generate ~/.neurus/preferences.json on first run. The UI only ever
	// receives plain values (see the UIManager ctor below). ---
	app_preferences = std::make_unique<neurus::Preferences>();
	const std::string prefsPath = neurus::Preferences::DefaultPath();

	// "auto" is stored verbatim and stays stored verbatim: I18n::setLanguage()
	// resolves the sentinel itself, so nothing here rewrites the preference with
	// a concrete code — otherwise the very first save would erase the user's
	// "follow the system language" choice.
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

	// --- Create UploadManager (transfer queue only; graphics queue passed per-call for IBL) ---
	app_uploadManager = std::make_unique<neurus::UploadManager>(
		app_vkContext->device(),
		app_vkContext->physicalDevice(),
		app_vkContext->transferQueue(),
		app_vkContext->transferQueueFamily());

	// --- Wire LightingCache to RenderCache via transfer queue ---
	app_renderer->GetRenderCache().SetLightingCache(
		app_uploadManager->CreateLightingCache(
			app_vkContext->device(),
			app_vkContext->physicalDevice()));

	// Now renderer + uploadManager exist — safe to create Editor
	InitEditor();

	const auto projectPath = resolveResourcePath("shadow.neurus.json").toStdString();
	const std::string objPath = kDefaultSceneObj;

	try
	{
		// Open the existing project: restores scene, config, AND UI layout.
		// Inlined (rather than OnProjectOpen) so a missing file throws through
		// to the default-project fallback below.
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
		app_editor->CreateDefaultScene(objPath);
		// Save for future runs (captures the current default UI layout too);
		// relative res/... paths are stored inside the pooled data resources.
		OnProjectSave(projectPath);
	}

	app_mainWindow->show();
	ResizeViewport(app_mainWindow->getViewportWidth(), app_mainWindow->getViewportHeight());

	// Upload scene GPU resources AFTER window is shown and surface is ready.
	// Doing this earlier (in Editor::Initialize) causes "Surface lost during recreation"
	// on the first frame because the swapchain isn't fully ready.
	//
	// One call covers the whole scene - meshes, lights, debug meshes, IBL cubemaps
	// and the light SSBO - and that is what makes the first-launch fallback
	// correct: its scene comes from CreateDefaultScene() and never passes through
	// FinishLoad(), so it used to end up with no IBL at all. For a loaded project
	// it is nearly free: FinishLoad() already uploaded everything and each part
	// skips work that is already cached.
	app_editor->UploadSceneResources();

	WireSignals();

	NEURUS_LOG("[Application] Entering event loop");
	app_renderTimer->start();
	int result = app_qtApp->exec();
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
		// Step 1: Create VkInstance with platform extensions
		app_platform = CreatePlatformSurface();
		auto vkInstance = neurus::VulkanContext::CreateInstance(*app_platform);
		app_vkContext = std::make_unique<neurus::VulkanContext>(std::move(vkInstance));

		// Step 2: Create Qt window with Viewport. The Application seeds the UI
		// with plain preference values (language / FPS / file path) — the UI
		// layer never touches the app-layer Preferences type.
		app_mainWindow = std::make_unique<neurus::UIManager>(
			QString::fromStdString(app_preferences->language),
			app_preferences->targetFps,
			QString::fromStdString(neurus::Preferences::DefaultPath()));
		app_mainWindow->show();  // Must show before surface creation on macOS

		// Step 3: Create VkSurfaceKHR via platform abstraction
		NativeWindowHandle hwnd = app_mainWindow->getViewportHwnd();
		app_surface = std::make_unique<vk::raii::SurfaceKHR>(
			app_platform->createSurface(app_vkContext->instance(), hwnd));

		// Step 4: Create logical device
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
		// Step 5: Validate queue family supports presentation, get queue handle
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

	// --- Create screenshot helper (needs Vulkan handles; RenderCache passed per-call) ---
	app_screenshot = std::make_unique<neurus::Screenshot>(
		app_vkContext->device(),
		app_vkContext->physicalDevice(),
		app_vkContext->graphicsQueue(),
		app_vkContext->graphicsQueueFamily());

	return true;
}

void Application::ResizeViewport(int width, int height)
{
	app_platform->onResize(
		app_mainWindow->getViewportHwnd(),
		static_cast<uint32_t>(width), static_cast<uint32_t>(height));
	app_renderer->HandleResize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));

	// The Editor needs both extents: logical for every projection query (and for
	// Camera::cam_w/cam_h), physical for the ID-buffer readback. The physical one is
	// read back from the renderer AFTER its own resize, so it is the extent the
	// swapchain actually got rather than the one we asked for.
	const auto extent = app_renderer->GetExtent();
	app_editor->HandleResize(
		glm::uvec2{static_cast<uint32_t>(width), static_cast<uint32_t>(height)},
		glm::uvec2{extent.width, extent.height});
}

// =========================================================================
// InitEditor – editor with project ownership transfer
// =========================================================================

void Application::InitEditor()
{
	app_editor = std::make_unique<neurus::Editor>(app_renderer.get(), app_uploadManager.get());
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

	// --- Preferences changes (Preferences dialog → Application) ---
	// The dialog's live controls emit through UIEvents; the Application
	// applies the new value, persists it, and (for language) retranslates
	// the whole UI via I18n::languageChanged.
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
	// Same starter content as a first launch: New is a fresh document, not an
	// empty one (a camera-less scene is unrenderable - see Editor::NewScene).
	app_editor->NewScene(kDefaultSceneObj);
	app_projectPath.clear();
	// Layout is intentionally left untouched on New; rebase the dirty
	// baseline so a fresh project isn't reported as having unsaved changes.
	app_savedUiState = app_mainWindow->ExportLayout();
}

void Application::OnProjectOpen(const std::string& path)
{
	try
	{
		app_editor->BeginLoad();                       // WaitIdle + fresh Scene/RenderConfig
		neurus::project::Project proj;
		BuildProject(proj, *app_editor, app_uiLayout); // refs bind to the fresh scene
		proj.Load(path);
		app_editor->FinishLoad();                      // reload mesh data, upload, IBL
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
	// --- Render request (manual frame trigger) ---
	// newFrame pattern: process enqueued camera events → draw → refresh UI
	QObject::connect(&uiEvents, &neurus::UIEvents::newFrame,
	                 [this]() {
	                     if (app_renderer && app_editor)
	                     {
	                         app_editor->Edit();  // Process all enqueued events (camera, etc.)
	                         // The Application assembles both contexts from the Editor's
	                         // shared state; the Editor never builds a RenderContext or
	                         // UIContext itself. DrawFrame returns the per-frame profile,
	                         // which the UIContext carries to the panels.
	                         const neurus::EditorContext editor = app_editor->GetContext();

	                         neurus::RenderContext rctx;
	                         rctx.editor = editor;
	                         auto profile = app_renderer->DrawFrame(rctx);

	                         const neurus::HistoryView history = app_editor->GetHistory();

	                         neurus::UIContext ctx;
	                         ctx.editor = editor;
	                         ctx.profile = &profile;
	                         ctx.log = &neurus::LogBuffer::instance();
	                         ctx.history = &history;
	                         app_mainWindow->Refresh(ctx);
	                     }
	                 });

	// StartRenderLoop – timer-driven ~60 FPS render loop
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
		// Handle Viewport resize
		QObject::connect(viewport, &neurus::Viewport::resized,
		                 [this](int width, int height) {
		                     ResizeViewport(width, height);
		                 });

		// Forward mouse movement to Editor for camera orbit/pan/dolly
		ConnectUIEvent(viewport, &neurus::Viewport::mouseMoved);

		// Forward mouse scroll to Editor for camera zoom
		ConnectUIEvent(viewport, &neurus::Viewport::mouseScrolled);

		// Forward mouse press/release to Editor so it can bound the camera
		// drag gesture (middle-button orbit/pan/dolly → one undo entry). The
		// selection lambda below also listens to mousePressed (Left button);
		// both connections coexist.
		ConnectUIEvent(viewport, &neurus::Viewport::mousePressed);
		ConnectUIEvent(viewport, &neurus::Viewport::mouseReleased);

		// Forward the Delete key (remove all selected objects).
		ConnectUIEvent(viewport, &neurus::Viewport::deleteRequested);

		// Forward modal transform keystrokes (G/R/S, X/Y/Z, Return, Escape) and
		// the focus-loss abort. Both are raw viewport facts: the Editor turns a
		// key into a gizmo intent, and TransformGizmoController decides whether
		// the intent means anything.
		ConnectUIEvent(viewport, &neurus::Viewport::keyPressed);
		ConnectUIEvent(viewport, &neurus::Viewport::focusLost);

		// Handle left-click for pixel-perfect object selection via IDBuffer
		QObject::connect(viewport, &neurus::Viewport::mousePressed,
		                 [this, viewport](const neurus::MousePressEvent& e) {
		                     if (e.button != Input::MouseButton::Left)
		                         return;

		                     // A left click that confirms a modal transform gesture
		                     // must not also re-select whatever happens to be under
		                     // the cursor, and must not pay this path's
		                     // queue.waitIdle ID-buffer readback. This is a separate
		                     // connection from the ConnectUIEvent above, so the
		                     // gesture's own GizmoConfirmed still reaches the Editor.
		                     // Frame-granular and therefore safe: gizmo state only
		                     // ever changes inside Editor::Edit().
		                     if (app_editor->IsGizmoActive())
		                         return;

		                     const auto renderExtent = app_renderer->GetExtent();
		                     const int   widgetW     = viewport->width();
		                     const int   widgetH     = viewport->height();

		                     // Map from logical (Qt widget) to physical (Vulkan) pixels
		                     uint32_t px = static_cast<uint32_t>(
		                         e.position.x * renderExtent.width  / static_cast<float>(widgetW));
		                     uint32_t py = static_cast<uint32_t>(
		                         e.position.y * renderExtent.height / static_cast<float>(widgetH));

		                     // Read the object ID from the IDBuffer at the click
		                     // position (GPU → CPU readback via PickPixel).
		                     uint32_t objectID = app_renderer->ReadIDBufferPixel(px, py);

		                     // Forward to Editor; objectID=0 means background
		                     // (handled by SelectObject → ClearSelection).
		                     ObjectSelected selEvent {
		                         static_cast<int>(objectID),
		                         static_cast<int>(e.modifiers)
		                     };
		                     app_editor->OnUIEvent(selEvent);
		                 });
	}

	// Handle RenderConfig changes → Editor (via ConnectUIEvent → OnUIEvent → EventQueue)
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

	// Handle Transform changes from Property Panel → Editor
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
	// Handle UI recreation (new native window handle → new surface → new swapchain, rebind the signals)
	QObject::connect(&uiEvents, &neurus::UIEvents::uiRecreated,
	                 [this](quintptr newHwnd) {
	                     // Reconnect panel signals (Viewport resize) to the new widget
	                     PanelSignals(neurus::UIEvents::instance());

	                     if (!app_vkContext || !app_renderer)
	                         return;
	                     try
	                     {
	                         auto new_surface = std::make_unique<vk::raii::SurfaceKHR>(
	                             app_platform->createSurface(
	                                 app_vkContext->instance(),
	                                 reinterpret_cast<NativeWindowHandle>(newHwnd)));
	                         app_renderer->HandleSurfaceChange(*new_surface);
	                         app_surface = std::move(new_surface);  // Old surface destroyed AFTER swapchain drops its ref
	                         NEURUS_LOG("[Application] Viewport recreated — new surface + swapchain");
	                     }
	                     catch (const std::exception& e)
	                     {
	                         NEURUS_ERR("Viewport recreation failed: " << e.what());
	                     }
	                 });
}

// =========================================================================
// ScreenShotSignals – screenshot + attachment dump requests
// =========================================================================

void Application::ScreenShotSignals(neurus::UIEvents& uiEvents)
{
	// Handle screenshot requests (F12 / menu action) via UIEvents signal
	QObject::connect(&uiEvents, &neurus::UIEvents::screenshotRequested,
	                 [this]() {
	                     if (app_screenshot && app_renderer)
	                     {
	                         app_screenshot->TakeScreenshot(
	                             app_renderer->GetLastSwapchainImage(),
	                             app_renderer->GetSwapchainFormat(),
	                             app_renderer->GetExtent());
	                     }
	                 });

	// Handle attachment dump requests (Ctrl+F12) via UIEvents signal
	QObject::connect(&uiEvents, &neurus::UIEvents::screenshotAllRequested,
	                 [this]() {
	                     if (app_screenshot && app_renderer)
	                     {
	                         app_screenshot->TakeScreenshotAllAttachments(
	                             app_renderer->GetRenderCache(),
	                             app_renderer->GetExtent(), 1024);
	                     }
	                 });
}
} // namespace neurus
