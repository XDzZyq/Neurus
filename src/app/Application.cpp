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

#include "app/Preferences.h"
#include "app/session/SessionServer.h"
#include "asset/Project.h"
#include "asset/components/ConfigComponent.h"
#include "asset/components/EditorComponent.h"
#include "asset/components/HistoryComponent.h"
#include "asset/components/ResourceComponent.h"
#include "asset/components/SceneComponent.h"
#include "asset/components/UIComponent.h"
#include "core/Log.h"
#include "editor/commands/EditorCommandAdapter.h"
#include "editor/events/SceneEvents.h"
#include "editor/events/UIEvents.h"
#include "ui/UIManager.h"
#include "ui/panels/Viewport.h"
#include "ui/utils/I18n.h"

#include <QApplication>
#include <QCoreApplication>
#include <QIcon>
#include <QObject>
#include <QString>
#include <QTimer>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <utility>

namespace {

/// Starter mesh of the default scene, shared by the first-launch fallback and
/// File > New so the two cannot drift. Relative, so project files stay portable.
constexpr const char* kDefaultSceneObj = "res/obj/sphere.obj";

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
	// Capture our own flags before QApplication can consume them: Qt treats
	// --session as its -session option and removes it from its argument list.
	app_rawArguments.reserve(static_cast<std::size_t>(argc));
	for (int i = 0; i < argc; ++i)
		app_rawArguments.emplace_back(argv[i] ? argv[i] : "");

	app_qtApp = std::make_unique<QApplication>(argc, argv);
	app_renderTimer = std::make_unique<QTimer>();
	app_renderTimer->setInterval(16);  // ~60 FPS
	app_qtApp->setApplicationName("Neurus");
	app_qtApp->setApplicationVersion("0.1.0");
	app_startTime = std::chrono::steady_clock::now();
	// Set before any window exists so every top-level widget (the main window,
	// dialogs) inherits the window and taskbar icon. Skipped on macOS: the Dock
	// shows the bundle's neurus.icns, which the system masks to the rounded
	// app-icon shape, and setWindowIcon() would replace it at runtime with the
	// raw square SVG (NSApp.applicationIconImage is drawn unmasked).
#ifndef __APPLE__
	app_qtApp->setWindowIcon(QIcon(":/app/neurus.svg"));
#endif
}

Application::~Application()
{
}

// =========================================================================
// Run() – orchestration
// =========================================================================

int Application::Run()
{
	app_args = AppArgs::Parse(app_rawArguments);
	if (!app_args.error.empty())
	{
		NEURUS_ERR("[Application] " << app_args.error);
		return -1;
	}
	if (app_args.session && !ipc::IsValidSessionName(*app_args.session))
	{
		NEURUS_ERR("[Application] Invalid session name: " << *app_args.session);
		return -1;
	}

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
	app_editorCommands = std::make_unique<EditorCommandAdapter>(*app_editor);
	InitScripting();

	std::string projectPath;
	if (app_args.project)
	{
		std::error_code pathError;
		if (!std::filesystem::is_regular_file(*app_args.project, pathError))
		{
			NEURUS_ERR("[Application] Project file not found: " << *app_args.project);
			return -1;
		}
		projectPath = *app_args.project;
	}
	else
	{
		projectPath = resolveResourcePath("shadow.neurus.json").toStdString();
	}
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

	if (app_args.session)
	{
		app_sessionName = *app_args.session;
		app_session = std::make_unique<SessionServer>(
			*this, app_qtApp->applicationVersion().toStdString(), app_editorCommands.get(),
			this, this);
		std::string sessionError;
		if (!app_session->Start(app_sessionName, app_projectPath, sessionError))
		{
			NEURUS_ERR("[Application] Cannot start command session: " << sessionError);
			return -1;
		}
	}

	// Requests and resizes posted before this point are queued and serviced by
	// the thread's first iteration, so nothing posted above is lost.
	app_renderThread->Start();

	WireSignals();

	NEURUS_LOG("[Application] Entering event loop");
	app_renderTimer->start();
	int result = app_qtApp->exec();

	if (app_session)
		app_session->Stop();

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
		auto vkInstance = neurus::VulkanContext::CreateInstance();
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
			neurus::platform::CreateSurface(app_vkContext->instance(), hwnd));

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
	neurus::platform::OnResize(
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

	// The viewport needs both extents: logical for every projection query,
	// physical for the ID-buffer readback. The physical one is estimated from the
	// widget's device pixel ratio because the swapchain now rebuilds on the render
	// thread; ReconcileRenderExtent() corrects the estimate once the thread
	// reports the extent it actually got. The Editor forwards the logical extent
	// to whichever camera is active.
	app_editor->HandleViewportResize(
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
	// extent is the truth, and the viewport derives its DPI ratio from it. Go
	// through the Editor so the active camera is re-framed for the real extent,
	// not only the estimate handed to the render thread.
	const glm::uvec2 render = viewport.RenderExtent();
	if (render.x == actual.width && render.y == actual.height)
	{
		return;
	}

	app_editor->HandleViewportResize(logical, glm::uvec2{actual.width, actual.height});
}

void Application::ResolvePendingPick()
{
	if (!app_pendingPick || !app_editor)
	{
		return;
	}

	if (app_pendingPick->objectId.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
	{
		return;
	}

	PendingPick pick = std::move(*app_pendingPick);
	app_pendingPick.reset();

	// A modal transform gesture that began after the click owns the selection
	// now; a late answer must not re-select under it.
	if (app_editor->GetViewport().IsGizmoActive())
	{
		return;
	}

	uint32_t objectID = 0;
	try
	{
		objectID = pick.objectId.get();
	}
	catch (const std::exception& ex)
	{
		// Broken promise (thread stopped) or a readback error inside the
		// renderer.
		NEURUS_ERR("[Application] Pick readback failed: " << ex.what());
		return;
	}

	// objectID 0 means background (clears the selection).
	app_editor->OnUIEvent(ObjectSelected{static_cast<int>(objectID), pick.modifiers});
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

	// Created here with the other subsystems so NewFrameSignals has a stable
	// Console() buffer; python startup happens in InitScripting().
	app_script = std::make_unique<neurus::ScriptEngine>();

	NEURUS_LOG("[Application] Editor initialized, IBL handled by Editor");
}

void Application::InitScripting()
{
	// Scripted writes go through the editor's own dispatch, so a script mutation
	// is applied by the same controller as the equivalent panel edit.
	app_script->SetEventQueue(&app_editor->GetEventQueue());

	// The engine resolves its own environment (NEURUS_PYTHON_ENV override, else
	// the build-time lib/.venv default). A failure degrades to a no-op engine.
	app_script->Initialize();
}


bool Application::OnProjectNew(std::string* error)
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
		if (error)
			*error = e.what();
		return false;
	}

	app_projectPath.clear();
	// Layout is intentionally left untouched on New; rebase the dirty
	// baseline so a fresh project isn't reported as having unsaved changes.
	app_savedUiState = app_mainWindow->ExportLayout();
	return true;
}

bool Application::OnProjectOpen(const std::string& path, std::string* error)
{
	std::error_code fileError;
	if (!std::filesystem::is_regular_file(path, fileError))
	{
		NEURUS_ERR("[Application] Project file not found: " << path);
		if (error)
			*error = "Project file not found: " + path;
		return false;
	}

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
		if (error)
			*error = e.what();

		// BeginLoad() already dropped the previous document, so a failed read
		// must leave something valid behind rather than a half-loaded scene.
		try
		{
			app_editor->NewScene();
			app_editor->CreateDefaultScene(kDefaultSceneObj);
			app_editor->ClearDirty();
			app_editor->UploadSceneResources();
			app_projectPath.clear();
			app_savedUiState = app_mainWindow->ExportLayout();
		}
		catch (const std::exception& resetError)
		{
			NEURUS_ERR("[Application] Failed to recover after a project open failure: "
			           << resetError.what());
		}
		return false;
	}
	return true;
}

bool Application::OnProjectSave(const std::string& path, std::string* error)
{
	const std::string target = path.empty() ? app_projectPath : path;
	if (target.empty())
	{
		NEURUS_ERR("[Application] Save requested but no project path is set.");
		if (error)
			*error = "No project path is set.";
		return false;
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
		if (error)
			*error = e.what();
		return false;
	}
	return true;
}

} // namespace neurus
