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

#include "app/ApplicationCore.h"
#include "app/Preferences.h"
#include "app/session/SessionServer.h"
#include "core/Log.h"
#include "editor/commands/EditorCommandAdapter.h"
#include "editor/events/SceneEvents.h"
#include "editor/events/UIEvents.h"
#include "platform/PlatformProcess.h"
#include "platform/PlatformSystem.h"
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
#include <stdexcept>
#include <filesystem>
#include <future>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

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


/**
 * @brief Splits a preload list on commas and semicolons, trimming each item.
 *
 * Both separators are accepted because the environment variable is written by
 * hand: a comma-separated list is the documented form and a semicolon works on
 * shells where the comma is inconvenient.
 */
std::vector<std::string> SplitPackageList(const std::string& value)
{
	std::vector<std::string> items;
	std::size_t start = 0;
	while (start <= value.size())
	{
		const std::size_t end = value.find_first_of(",;", start);
		std::string item = value.substr(
			start, end == std::string::npos ? std::string::npos : end - start);
		const std::size_t first = item.find_first_not_of(" \t");
		const std::size_t last = item.find_last_not_of(" \t");
		if (first != std::string::npos)
		{
			items.push_back(item.substr(first, last - first + 1));
		}
		if (end == std::string::npos)
		{
			break;
		}
		start = end + 1;
	}
	return items;
}

/// @brief Joins preference list entries for the Preferences dialog.
QString JoinPreferenceList(const std::vector<std::string>& values)
{
	QStringList parts;
	for (const std::string& value : values)
	{
		parts.append(QString::fromStdString(value));
	}
	return parts.join(QStringLiteral("; "));
}

/// @brief Builds the UI-side script settings from the persisted preferences.
neurus::ScriptPreferencesUi MakeScriptPreferencesUi(const neurus::Preferences& preferences)
{
	neurus::ScriptPreferencesUi script;
	script.readRoots = JoinPreferenceList(preferences.scriptSandboxReadRoots);
	script.writeRoots = JoinPreferenceList(preferences.scriptSandboxWriteRoots);
	script.blockedModules = JoinPreferenceList(preferences.scriptSandboxBlockedModules);
	script.preload = JoinPreferenceList(preferences.scriptPreload);
	script.allowNetwork = preferences.scriptSandboxAllowNetwork;
	script.allowSubprocess = preferences.scriptSandboxAllowSubprocess;
	script.allowCtypes = preferences.scriptSandboxAllowCtypes;
	script.timeLimitSeconds = preferences.scriptSandboxTimeLimitSeconds;
	script.memoryLimitMb = preferences.scriptSandboxMemoryLimitMb;
	return script;
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
	// The Qt-free core: preferences, the script engine and the open project.
	// Created after QApplication so anything that ever needs the app object
	// (nothing in the core does) would still find it, and before Run() loads
	// preferences for the window.
	app_core = std::make_unique<neurus::ApplicationCore>();
	app_qtApp->setApplicationName("Neurus");
	app_qtApp->setApplicationVersion("0.1.0");
	app_startTime = std::chrono::steady_clock::now();
	// Set before any window exists so every top-level widget (the main window,
	// dialogs) inherits the window and taskbar icon. Skipped when the OS draws
	// the icon from the installed bundle (macOS): the Dock shows the bundle's
	// neurus.icns, which the system masks to the rounded app-icon shape, and
	// setWindowIcon() would replace it at runtime with the raw square SVG
	// (NSApp.applicationIconImage is drawn unmasked).
	if (!platform::BundleProvidesAppIcon())
		app_qtApp->setWindowIcon(QIcon(":/app/neurus.svg"));
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
	// window is built so the UI starts in the saved language. Nothing is written
	// unless a value changes: a run that only reads them (a test session, which
	// redirects NEURUS_SESSIONS_DIR but shares the real home) must leave the
	// user's file alone. ApplicationCore owns the file and the policy built from
	// it; ApplicationCore::LoadPreferences() moves a corrupt file aside instead of
	// overwriting the evidence. ---
	app_core->LoadPreferences();

	neurus::I18n::instance().setLanguage(
		QString::fromStdString(app_core->Prefs().language));
	ApplyTargetFps(app_core->Prefs().targetFps);

	// Clean-shutdown safety net: edits already save eagerly through
	// ApplicationSignals, so only a value that differs from the loaded snapshot
	// is written here.
	const neurus::Preferences loadedPreferences = app_core->Prefs();
	QObject::connect(app_qtApp.get(), &QCoreApplication::aboutToQuit,
	                 [this, loadedPreferences]() {
		                 if (app_core->Prefs() != loadedPreferences)
			                 app_core->Prefs().Save(neurus::Preferences::DefaultPath());
	                 });

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
	std::string startupError;

	try
	{
		// Open the existing project (scene, config and UI layout all live in
		// it). Inlined rather than OnProjectOpen so a missing file falls through
		// to the default-project fallback below.
		if (!app_core->LoadProject(projectPath, *app_editor, app_uiLayout, startupError))
			throw std::runtime_error(startupError);
		app_mainWindow->ApplyLayout(app_uiLayout);
		// Rebase on what Qt re-exports, not the file blob: restoring a layout
		// saved on another screen normalises the geometry, so the stored bytes
		// never compare equal and a just-opened project would read as dirty.
		app_savedUiState = app_mainWindow->ExportLayout();
		NEURUS_LOG("[Application] Loaded project: " << projectPath);
	}
	catch (const std::exception& e)
	{
		NEURUS_LOG("[Application] Project file not found, creating default: " << e.what());
		// Same two steps as File > New: an empty document, then the default
		// content. The upload waits until after show(), below.
		std::string defaultError;
		if (!app_core->CreateDefaultDocument(*app_editor, objPath, defaultError))
			NEURUS_ERR("[Application] Failed to create the default document: " << defaultError);
		// Save for future runs (captures the current default UI layout too).
		// This also rebases the dirty baseline.
		OnProjectSave(projectPath);
	}

	// The project path is now known (or the fallback saved one); project-local
	// modules live beside it, so they become the session's reload roots.
	UpdateScriptReloadRoots();

	app_mainWindow->show();
	ResizeViewport(app_mainWindow->getViewportWidth(), app_mainWindow->getViewportHeight());

	// show() can normalise the window geometry after the load-time rebase (for
	// example when the saved layout came from another screen). Rebase again on
	// what Qt now exports, or a freshly opened project reads as dirty.
	app_savedUiState = app_mainWindow->ExportLayout();

	// Upload scene GPU resources AFTER the window is shown and the surface is
	// ready: doing it earlier causes "Surface lost during recreation" on the
	// first frame because the swapchain is not fully ready.
	app_editor->UploadSceneResources();

	if (app_args.session)
	{
		app_core->SetSessionName(*app_args.session);
		app_session = std::make_unique<SessionServer>(
			*this, app_qtApp->applicationVersion().toStdString(), app_editorCommands.get(),
			this, this);
		std::string sessionError;
		if (!app_session->Start(app_core->SessionName(), app_core->ProjectPath(), sessionError))
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
			QString::fromStdString(app_core->Prefs().language),
			app_core->Prefs().targetFps,
			MakeScriptPreferencesUi(app_core->Prefs()),
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

	// The script engine lives in app_core, which the constructor created: its
	// Console() buffer is stable from here on, and python startup still happens
	// in InitScripting().

	NEURUS_LOG("[Application] Editor initialized, IBL handled by Editor");
}

void Application::InitScripting()
{
	// Preload precedence for one launch: CLI --preload, then the persisted
	// preference, then NEURUS_SCRIPT_PRELOAD. The first non-empty source wins,
	// so an explicit flag never silently merges with stale preferences.
	std::vector<std::string> preload;
	if (!app_args.preload.empty())
	{
		preload = app_args.preload;
	}
	else if (!app_core->Prefs().scriptPreload.empty())
	{
		preload = app_core->Prefs().scriptPreload;
	}
	else
	{
		preload = SplitPackageList(EnvironmentValue("NEURUS_SCRIPT_PRELOAD"));
	}

	// The core wires the engine to the editor's own dispatch, so a script
	// mutation is applied by the same controller as the equivalent panel edit.
	app_core->InitializeScripting(*app_editor, preload);

	UpdateScriptReloadRoots();
}

void Application::UpdateScriptReloadRoots()
{
	std::vector<std::string> roots;
	std::error_code pathError;
	std::filesystem::path directory;
	if (!app_core->ProjectPath().empty())
	{
		directory = std::filesystem::path(app_core->ProjectPath()).parent_path();
	}
	if (directory.empty())
	{
		directory = std::filesystem::current_path(pathError);
	}
	if (!directory.empty())
	{
		roots.push_back(directory.generic_string());
	}

	app_core->Script().SetReloadRoots(roots);
	NEURUS_LOG("[Application] script reload roots: "
	           << (roots.empty() ? std::string("(none)") : roots.front()));
}


bool Application::OnProjectNew(std::string* error)
{
	// File > New is the shared default document; the UI keeps its layout.
	std::string documentError;
	if (!app_core->CreateDefaultDocument(*app_editor, kDefaultSceneObj, documentError))
	{
		NEURUS_ERR("Failed to create new project: " << documentError);
		if (error)
			*error = documentError;
		return false;
	}
	app_core->SetProjectPath({});

	// Project-local modules follow the project: with none open, the process
	// current directory is the reload root.
	UpdateScriptReloadRoots();
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

	std::string failure;
	if (!app_core->LoadProject(path, *app_editor, app_uiLayout, failure))
	{
		NEURUS_ERR("[Application] Failed to open project: " << failure);
		if (error)
			*error = failure;

		// BeginLoad() already dropped the previous document, so a failed read
		// must leave something valid behind rather than a half-loaded scene.
		std::string resetError;
		if (!app_core->CreateDefaultDocument(*app_editor, kDefaultSceneObj, resetError))
		{
			NEURUS_ERR("[Application] Failed to recover after a project open failure: "
			           << resetError);
		}
		app_core->SetProjectPath({});
		UpdateScriptReloadRoots();
		app_savedUiState = app_mainWindow->ExportLayout();
		return false;
	}

	app_mainWindow->ApplyLayout(app_uiLayout);
	// The opened project's directory becomes the reload root for its modules.
	UpdateScriptReloadRoots();
	// Same rebase as the startup load: the re-exported layout, not the blob.
	app_savedUiState = app_mainWindow->ExportLayout();
	return true;
}

bool Application::OnProjectSave(const std::string& path, std::string* error)
{
	const std::string target = path.empty() ? app_core->ProjectPath() : path;
	if (target.empty())
	{
		NEURUS_ERR("[Application] Save requested but no project path is set.");
		if (error)
			*error = "No project path is set.";
		return false;
	}

	app_uiLayout = app_mainWindow->ExportLayout();
	std::string failure;
	if (!app_core->SaveProject(target, *app_editor, app_uiLayout, failure))
	{
		NEURUS_ERR("[Application] Failed to save project: " << failure);
		if (error)
			*error = failure;
		return false;
	}

	// Save As can move the document to another directory; reload roots follow
	// the project file.
	UpdateScriptReloadRoots();
	app_savedUiState = app_uiLayout;
	return true;
}

} // namespace neurus
