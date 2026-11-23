#include "app/HeadlessApplication.hpp"

#include "app/ApplicationCore.hpp"
#include "app/Preferences.hpp"
#include "app/VulkanContext.hpp"
#include "app/session/SessionServer.hpp"

#include "core/Log.hpp"
#include "editor/Editor.hpp"
#include "editor/commands/EditorCommandAdapter.hpp"
#include "platform/PlatformProcess.hpp"
#include "render/DeferredRenderer.hpp"
#include "render/RenderCache.hpp"
#include "render_api/RenderFrameSnapshot.hpp"
#include "render/RenderThread.hpp"
#include "render/UploadManager.hpp"
#include "render/resources/LightingCache.hpp"
#include "scene/Scene.hpp"
#include "script/ScriptContext.hpp"
#include "script/ScriptEngine.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <future>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace neurus
{
namespace
{

std::atomic<bool> g_stopRequested{false};

void OnTerminationSignal(int)
{
	g_stopRequested.store(true);
}

constexpr const char* kDefaultSceneObj = "res/obj/sphere.obj";
constexpr const char* kAppVersion = "0.1.0";

std::filesystem::path ExecutableDirectory()
{
	std::error_code error;
	const std::filesystem::path exe = CurrentExecutablePath();
	const std::filesystem::path parent = exe.parent_path();
	return error ? std::filesystem::current_path() : parent;
}

std::vector<std::filesystem::path> ListFiles(const std::filesystem::path& directory)
{
	std::vector<std::filesystem::path> files;
	std::error_code error;
	for (const auto& entry : std::filesystem::recursive_directory_iterator(directory, error))
	{
		if (error)
			break;
		if (entry.is_regular_file())
			files.push_back(entry.path());
	}
	std::sort(files.begin(), files.end());
	return files;
}

/**
 * @brief Splits a preload list on commas and semicolons, trimming each item.
 *
 * Same rule as the GUI: the environment variable is written by hand, so a comma
 * is the documented separator and a semicolon works where a comma is awkward.
 */
std::vector<std::string> SplitPackageList(const std::string& value)
{
	std::vector<std::string> items;
	std::string current;
	auto flush = [&]()
	{
		const std::size_t first = current.find_first_not_of(" \t");
		const std::size_t last = current.find_last_not_of(" \t");
		if (first != std::string::npos)
			items.push_back(current.substr(first, last - first + 1));
		current.clear();
	};
	for (const char ch : value)
	{
		if (ch == ',' || ch == ';')
			flush();
		else
			current.push_back(ch);
	}
	flush();
	return items;
}

} // namespace

HeadlessApplication::HeadlessApplication(std::vector<std::string> arguments)
	: app_args(AppArgs::Parse(arguments))
	, app_startTime(std::chrono::steady_clock::now())
{
}

HeadlessApplication::~HeadlessApplication() = default;

// ---------------------------------------------------------------------------
// Startup and loop
// ---------------------------------------------------------------------------

int HeadlessApplication::Run()
{
	if (!app_args.error.empty())
	{
		NEURUS_ERR("[neurus_headless] " << app_args.error);
		return 2;
	}

	g_stopRequested.store(false);
	std::signal(SIGINT, OnTerminationSignal);
	std::signal(SIGTERM, OnTerminationSignal);

	// --- Preferences and the script engine: shared with the Qt application ---
	app_core = std::make_unique<ApplicationCore>();
	app_core->LoadPreferences();
	const int targetFps = app_core->Prefs().targetFps;

	try
	{
		// --- Vulkan: no surface extensions, no swapchain ---
		VulkanContext::Options options;
		options.surfaceExtensions = false;
		options.swapchain = false;

		app_vkContext = std::make_unique<VulkanContext>(VulkanContext::CreateInstance(options));
		app_vkContext->InitDevice(options);
		app_vkContext->InitQueue();

		const vk::Extent2D extent{app_args.width, app_args.height};
		NEURUS_LOG("[neurus_headless] " << app_vkContext->gpuName()
		           << " headless " << extent.width << "x" << extent.height);

		// --- Renderer on an offscreen target ---
		app_renderer = std::make_unique<DeferredRenderer>(
			app_vkContext->device(), app_vkContext->physicalDevice(),
			app_vkContext->graphicsQueue(), app_vkContext->graphicsQueueFamily(), extent, app_core->RenderDefaults());
		app_renderThread = std::make_unique<RenderThread>(*app_renderer);

		app_uploader = std::make_unique<UploadManager>(
			app_vkContext->device(), app_vkContext->physicalDevice(),
			app_vkContext->transferQueue(), app_vkContext->transferQueueFamily());
		app_renderer->GetRenderCache().SetLightingCache(
			app_uploader->CreateLightingCache(app_vkContext->device(), app_vkContext->physicalDevice()));
		app_renderer->SetUploadManager(app_uploader.get());

		// --- Editor ---
		app_editor = std::make_unique<Editor>(app_renderThread.get(), app_core->RenderDefaults());
		app_editor->Initialize();
		app_editor->HandleViewportResize(glm::uvec2(app_args.width, app_args.height),
		                               glm::uvec2(app_args.width, app_args.height));
		app_commands = std::make_unique<EditorCommandAdapter>(*app_editor);

		// --- Script layer: the same engine the GUI runs, always present ---
		// Preload precedence matches the GUI: --preload, then the persisted
		// preference, then NEURUS_SCRIPT_PRELOAD.
		std::vector<std::string> preload;
		if (!app_args.preload.empty())
			preload = app_args.preload;
		else if (!app_core->Prefs().scriptPreload.empty())
			preload = app_core->Prefs().scriptPreload;
		else
			preload = SplitPackageList(EnvironmentValue("NEURUS_SCRIPT_PRELOAD"));

		app_core->InitializeScripting(*app_editor, preload);

		// --- Document ---
		std::string loadError;
		const std::filesystem::path defaultProject =
			ExecutableDirectory() / "res" / "shadow.neurus.json";
		const std::filesystem::path project = app_args.project
			? std::filesystem::path(*app_args.project)
			: defaultProject;

		std::string uiLayout;
		if (std::filesystem::is_regular_file(project) &&
		    app_core->LoadProject(project, *app_editor, uiLayout, loadError))
		{
			NEURUS_LOG("[neurus_headless] Loaded project " << app_core->ProjectPath());
		}
		else
		{
			if (!loadError.empty())
				NEURUS_ERR("[neurus_headless] " << loadError);

			std::string defaultError;
			if (!app_core->CreateDefaultDocument(*app_editor,
			                                   ExecutableDirectory() / kDefaultSceneObj,
			                                   defaultError))
			{
				NEURUS_ERR("[neurus_headless] Failed to create the default document: " << defaultError);
				return 1;
			}
		}

		app_editor->UploadSceneResources();

		// --- Session server (the CLI's entry point) ---
		if (app_args.session)
		{
			app_core->SetSessionName(*app_args.session);
			app_session = std::make_unique<SessionServer>(
				*this, kAppVersion, app_commands.get(), this, this);
			std::string sessionError;
			if (!app_session->Start(app_core->SessionName(), app_core->ProjectPath(), sessionError))
			{
				NEURUS_ERR("[neurus_headless] Failed to start the session: " << sessionError);
				return 1;
			}
			NEURUS_LOG("[neurus_headless] Session '" << app_core->SessionName()
			          << "' listening on port " << app_session->Port());
		}

		app_renderThread->Start();

		// --- Command/render loop ---
		// Publishing every tick keeps the mailbox live and lets a capture observe
		// the converged frame; NextWork() decides whether a frame is redrawn.
		// The tick rate follows the persisted target FPS; 0 means uncapped, which
		// still yields to the OS for a millisecond so the process is not a spinner.
		const int frameRate = targetFps > 0 ? targetFps : 1000;
		const std::chrono::milliseconds interval(std::max(1, 1000 / frameRate));
		NEURUS_LOG("[neurus_headless] Tick interval " << interval.count()
		          << " ms (targetFps=" << targetFps << ")");
		auto nextTick = std::chrono::steady_clock::now();
		while (!g_stopRequested.load())
		{
			if (app_session)
			{
				app_session->DrainOnMainThread();
				if (app_session->CloseRequested())
				{
					app_session->WaitForWrites(std::chrono::seconds(2));
					break;
				}
			}

			app_editor->Edit();

			// Scripts run before the snapshot is built, so a script mutation is in
			// the frame the renderer receives.
			{
				ScriptContext scriptContext;
				app_editor->UpdateScriptContext(scriptContext);
				app_core->Script().ExecutePending(scriptContext);
			}

			RenderFrameSnapshot snapshot;
			app_editor->BuildSnapshot(snapshot);
			app_renderThread->Publish(std::move(snapshot));

			nextTick += interval;
			std::this_thread::sleep_until(nextTick);
			const auto now = std::chrono::steady_clock::now();
			if (now > nextTick + interval)
			{
				nextTick = now;  // Never spiral after a stall.
			}
		}

		// --- Stop: session first, then the render thread (as the GUI does) ---
		if (app_session)
			app_session->Stop();
		app_renderThread->Stop();
	}
	catch (const std::exception& e)
	{
		NEURUS_ERR("[neurus_headless] Fatal: " << e.what());
		if (app_session)
			app_session->Stop();
		if (app_renderThread)
			app_renderThread->Stop();
		return 1;
	}

	return 0;
}

// ---------------------------------------------------------------------------
// command::ISessionState
// ---------------------------------------------------------------------------

int HeadlessApplication::Pid() const
{
	return CurrentProcessId();
}

std::string HeadlessApplication::SessionName() const
{
	return app_core->SessionName();
}

std::string HeadlessApplication::ProjectPath() const
{
	return app_core->ProjectPath();
}

bool HeadlessApplication::IsDirty() const
{
	return app_editor && app_editor->IsDirty();
}

bool HeadlessApplication::Save(std::string& error)
{
	if (app_core->ProjectPath().empty())
	{
		error = "No project path is set.";
		return false;
	}
	return app_core->SaveProject(app_core->ProjectPath(), *app_editor, {}, error);
}

bool HeadlessApplication::SaveAs(const std::string& path, std::string& error)
{
	return app_core->SaveProject(path, *app_editor, {}, error);
}

bool HeadlessApplication::NewProject(bool force, std::string& error)
{
	if (IsDirty() && !force)
	{
		error = "The project has unsaved changes; pass force to discard them.";
		return false;
	}
	if (!app_core->CreateDefaultDocument(*app_editor, ExecutableDirectory() / kDefaultSceneObj, error))
		return false;
	app_core->SetProjectPath({});
	return true;
}

bool HeadlessApplication::OpenProject(const std::string& path, bool force, std::string& error)
{
	if (IsDirty() && !force)
	{
		error = "The project has unsaved changes; pass force to discard them.";
		return false;
	}

	// A missing file is a pre-load failure: the document was never dropped, so
	// keep it, exactly as the GUI does. ApplicationCore::LoadProject returns
	// before BeginLoad in that case.
	std::error_code fileError;
	if (!std::filesystem::is_regular_file(path, fileError))
	{
		error = "Project file not found: " + path;
		NEURUS_ERR("[neurus_headless] " << error);
		return false;
	}

	std::string uiLayout;
	if (!app_core->LoadProject(path, *app_editor, uiLayout, error))
	{
		// BeginLoad already dropped the previous document, so leave a valid one
		// behind. Creating the default document also clears the project path, so
		// no later save can write over the project that failed to open.
		std::string resetError;
		if (!app_core->CreateDefaultDocument(*app_editor, ExecutableDirectory() / kDefaultSceneObj,
		                                   resetError))
			NEURUS_ERR("[neurus_headless] Recovery after a failed open failed: " << resetError);
		return false;
	}

	app_editor->UploadSceneResources();
	return true;
}

int HeadlessApplication::SceneObjectCount() const
{
	return app_editor ? static_cast<int>(app_editor->GetScene().obj_list.size()) : 0;
}

std::int64_t HeadlessApplication::UptimeSeconds() const
{
	const auto elapsed = std::chrono::steady_clock::now() - app_startTime;
	return std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
}

std::string HeadlessApplication::AppVersion() const
{
	return kAppVersion;
}

// ---------------------------------------------------------------------------
// command::IScriptCommands
// ---------------------------------------------------------------------------

command::CommandResult HeadlessApplication::Execute(const command::CommandRequest& request)
{
	if (!app_editor)
	{
		return command::CommandResult::Failure(command::CommandErrorCode::Internal,
		                                      "The editor is not available.");
	}
	return app_core->ExecuteScript(request, *app_editor);
}

// ---------------------------------------------------------------------------
// command::ISnapshotCommands
// ---------------------------------------------------------------------------

command::CommandResult HeadlessApplication::Capture(const std::string& path, bool all)
{
	if (!app_renderThread)
	{
		return command::CommandResult::Failure(command::CommandErrorCode::Internal,
		                                      "The render thread is not available.");
	}

	std::error_code pathError;
	const std::filesystem::path requested = std::filesystem::absolute(path, pathError);
	const std::filesystem::path base = pathError ? std::filesystem::path(path) : requested;
	const std::string session = app_core->SessionName().empty() ? "session" : app_core->SessionName();

	auto waitForCapture = [](std::future<bool>& future)
	{
		if (future.wait_for(std::chrono::seconds(55)) != std::future_status::ready)
		{
			return command::CommandResult::Failure(
				command::CommandErrorCode::Timeout,
				"Timed out waiting for the render thread to write the snapshot.");
		}
		try
		{
			if (!future.get())
			{
				return command::CommandResult::Failure(
					command::CommandErrorCode::IoError,
					"The renderer did not write a snapshot.");
			}
		}
		catch (const std::exception& e)
		{
			return command::CommandResult::Failure(
				command::CommandErrorCode::IoError,
				std::string("The snapshot capture failed: ") + e.what());
		}
		return command::CommandResult::Success();
	};

	if (all)
	{
		const std::filesystem::path directory = base / session;
		std::error_code directoryError;
		std::filesystem::create_directories(directory, directoryError);
		if (directoryError)
		{
			return command::CommandResult::Failure(
				command::CommandErrorCode::IoError,
				"Cannot create the snapshot directory: " + directoryError.message(),
				{{"path", directory.string()}});
		}

		const auto before = ListFiles(directory);
		auto future = app_renderThread->PostScreenshotSync(true, directory);
		const command::CommandResult waited = waitForCapture(future);
		if (!waited.ok)
			return waited;

		const auto after = ListFiles(directory);
		const std::set<std::filesystem::path> existing(before.begin(), before.end());
		command::Json files = command::Json::array();
		for (const auto& file : after)
		{
			if (existing.count(file) == 0)
				files.push_back(file.string());
		}
		if (files.empty())
		{
			return command::CommandResult::Failure(
				command::CommandErrorCode::IoError,
				"The attachment capture wrote no new files.",
				{{"path", directory.string()}});
		}
		return command::CommandResult::Success({
			{"path", directory.string()},
			{"files", files},
			{"count", files.size()},
			{"all", true},
			{"session", session},
		});
	}

	const std::string stem = base.stem().string().empty() ? "snapshot" : base.stem().string();
	const std::filesystem::path parent = base.has_parent_path() ? base.parent_path()
	                                                            : std::filesystem::current_path();
	std::error_code directoryError;
	std::filesystem::create_directories(parent, directoryError);
	if (directoryError)
	{
		return command::CommandResult::Failure(
			command::CommandErrorCode::IoError,
			"Cannot create the snapshot directory: " + directoryError.message(),
			{{"path", parent.string()}});
	}

	// The renderer writes a timestamped file into a directory it is handed.
	// Capture into a scratch directory, then move the result onto the exact path
	// the command asked for.
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const std::filesystem::path scratch =
		parent / (".neurus-capture-" + session + "-" + std::to_string(stamp));
	std::filesystem::create_directories(scratch, directoryError);
	if (directoryError)
	{
		return command::CommandResult::Failure(
			command::CommandErrorCode::IoError,
			"Cannot create the capture directory: " + directoryError.message(),
			{{"path", scratch.string()}});
	}

	auto future = app_renderThread->PostScreenshotSync(false, scratch);
	const command::CommandResult waited = waitForCapture(future);
	if (!waited.ok)
	{
		std::error_code ignored;
		std::filesystem::remove_all(scratch, ignored);
		return waited;
	}

	const std::vector<std::filesystem::path> files = ListFiles(scratch);
	if (files.empty())
	{
		std::error_code ignored;
		std::filesystem::remove_all(scratch, ignored);
		return command::CommandResult::Failure(
			command::CommandErrorCode::IoError,
			"The renderer wrote no snapshot file.");
	}

	const std::filesystem::path finalPath = parent / (stem + "-" + session + ".png");
	std::error_code moveError;
	std::filesystem::rename(files.front(), finalPath, moveError);
	if (moveError)
	{
		// Different filesystem, or the destination exists: copy instead.
		std::filesystem::copy_file(files.front(), finalPath,
		                           std::filesystem::copy_options::overwrite_existing, moveError);
	}
	{
		std::error_code ignored;
		std::filesystem::remove_all(scratch, ignored);
	}
	if (moveError)
	{
		return command::CommandResult::Failure(
			command::CommandErrorCode::IoError,
			"Cannot write the snapshot: " + moveError.message(),
			{{"path", finalPath.string()}});
	}

	return command::CommandResult::Success({
		{"path", finalPath.string()},
		{"files", command::Json::array({finalPath.string()})},
		{"count", 1},
		{"all", false},
		{"session", session},
	});
}

} // namespace neurus
