#include "app/HeadlessApplication.h"

#include "app/Preferences.h"
#include "app/ProjectIo.h"
#include "app/ScriptCommands.h"
#include "app/VulkanContext.h"
#include "app/session/SessionServer.h"

#include "core/Log.h"
#include "editor/Editor.h"
#include "editor/commands/EditorCommandAdapter.h"
#include "platform/PlatformProcess.h"
#include "render/DeferredRenderer.h"
#include "render/RenderCache.h"
#include "render/RenderFrameSnapshot.h"
#include "render/RenderThread.h"
#include "render/UploadManager.h"
#include "render/resources/LightingCache.h"
#include "scene/Scene.h"
#include "script/ScriptConsole.h"
#include "script/ScriptContext.h"
#include "script/ScriptEngine.h"

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
	: m_args(AppArgs::Parse(arguments))
	, m_startTime(std::chrono::steady_clock::now())
{
}

HeadlessApplication::~HeadlessApplication() = default;

// ---------------------------------------------------------------------------
// Startup and loop
// ---------------------------------------------------------------------------

int HeadlessApplication::Run()
{
	if (!m_args.error.empty())
	{
		NEURUS_ERR("[neurus_headless] " << m_args.error);
		return 2;
	}

	g_stopRequested.store(false);
	std::signal(SIGINT, OnTerminationSignal);
	std::signal(SIGTERM, OnTerminationSignal);

	// --- Preferences: shared with the Qt application ---
	m_preferences = std::make_unique<Preferences>();
	const std::string preferencesPath = Preferences::DefaultPath();
	const Preferences::LoadResult preferencesResult = m_preferences->Load(preferencesPath);
	if (preferencesResult == Preferences::LoadResult::Corrupt)
	{
		NEURUS_ERR("[neurus_headless] Preferences at " << preferencesPath
		          << " are corrupt; moving them aside and using defaults");
		Preferences::Backup(preferencesPath);
	}
	const int targetFps = m_preferences->targetFps;

	try
	{
		// --- Vulkan: no surface extensions, no swapchain ---
		VulkanContext::Options options;
		options.surfaceExtensions = false;
		options.swapchain = false;

		m_vkContext = std::make_unique<VulkanContext>(VulkanContext::CreateInstance(options));
		m_vkContext->InitDevice(options);
		m_vkContext->InitQueue();

		const vk::Extent2D extent{m_args.width, m_args.height};
		NEURUS_LOG("[neurus_headless] " << m_vkContext->gpuName()
		           << " headless " << extent.width << "x" << extent.height);

		// --- Renderer on an offscreen target ---
		m_renderer = std::make_unique<DeferredRenderer>(
			m_vkContext->device(), m_vkContext->physicalDevice(),
			m_vkContext->graphicsQueue(), m_vkContext->graphicsQueueFamily(), extent);
		m_renderThread = std::make_unique<RenderThread>(*m_renderer);

		m_uploader = std::make_unique<UploadManager>(
			m_vkContext->device(), m_vkContext->physicalDevice(),
			m_vkContext->transferQueue(), m_vkContext->transferQueueFamily());
		m_renderer->GetRenderCache().SetLightingCache(
			m_uploader->CreateLightingCache(m_vkContext->device(), m_vkContext->physicalDevice()));
		m_renderer->SetUploadManager(m_uploader.get());

		// --- Editor ---
		m_editor = std::make_unique<Editor>(m_renderThread.get());
		m_editor->Initialize();
		m_editor->HandleViewportResize(glm::uvec2(m_args.width, m_args.height),
		                               glm::uvec2(m_args.width, m_args.height));
		m_commands = std::make_unique<EditorCommandAdapter>(*m_editor);

		// --- Script layer: the same engine the GUI runs, always present ---
		m_script = std::make_unique<ScriptEngine>();
		m_script->SetEventQueue(&m_editor->GetEventQueue());
		if (!m_script->Initialize())
		{
			NEURUS_ERR("[neurus_headless] Script engine failed to initialize; "
			          "script.* commands will report it as unavailable");
		}
		if (m_script->IsReady())
		{
			// Preload precedence matches the GUI: --preload, then the persisted
			// preference, then NEURUS_SCRIPT_PRELOAD.
			std::vector<std::string> preload;
			if (!m_args.preload.empty())
				preload = m_args.preload;
			else if (m_preferences && !m_preferences->scriptPreload.empty())
				preload = m_preferences->scriptPreload;
			else
				preload = SplitPackageList(EnvironmentValue("NEURUS_SCRIPT_PRELOAD"));

			if (!preload.empty())
			{
				for (const ScriptPreloadResult& result : m_script->Preload(preload))
				{
					if (result.ok)
						NEURUS_LOG("[neurus_headless] preloaded " << result.package);
					else
						NEURUS_ERR("[neurus_headless] preload failed: " << result.package
						          << ": " << result.error);
				}
			}
		}

		// --- Document ---
		std::string loadError;
		const std::filesystem::path defaultProject =
			ExecutableDirectory() / "res" / "shadow.neurus.json";
		const std::filesystem::path project = m_args.project
			? std::filesystem::path(*m_args.project)
			: defaultProject;

		std::string uiLayout;
		if (std::filesystem::is_regular_file(project) &&
		    LoadProject(project, *m_editor, uiLayout, loadError))
		{
			m_projectPath = project.string();
			NEURUS_LOG("[neurus_headless] Loaded project " << m_projectPath);
		}
		else
		{
			if (!loadError.empty())
				NEURUS_ERR("[neurus_headless] " << loadError);

			std::string defaultError;
			if (!CreateDefaultDocument(defaultError))
			{
				NEURUS_ERR("[neurus_headless] Failed to create the default document: " << defaultError);
				return 1;
			}
			m_projectPath.clear();
		}

		m_editor->UploadSceneResources();

		// --- Session server (the CLI's entry point) ---
		if (m_args.session)
		{
			m_sessionName = *m_args.session;
			m_session = std::make_unique<SessionServer>(
				*this, kAppVersion, m_commands.get(), this, this);
			std::string sessionError;
			if (!m_session->Start(m_sessionName, m_projectPath, sessionError))
			{
				NEURUS_ERR("[neurus_headless] Failed to start the session: " << sessionError);
				return 1;
			}
			NEURUS_LOG("[neurus_headless] Session '" << m_sessionName
			          << "' listening on port " << m_session->Port());
		}

		m_renderThread->Start();

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
			if (m_session)
			{
				m_session->DrainOnMainThread();
				if (m_session->CloseRequested())
				{
					m_session->WaitForWrites(std::chrono::seconds(2));
					break;
				}
			}

			m_editor->Edit();

			// Scripts run before the snapshot is built, so a script mutation is in
			// the frame the renderer receives.
			{
				ScriptContext scriptContext;
				m_editor->UpdateScriptContext(scriptContext);
				m_script->ExecutePending(scriptContext);
			}

			RenderFrameSnapshot snapshot;
			m_editor->BuildSnapshot(snapshot);
			m_renderThread->Publish(std::move(snapshot));

			nextTick += interval;
			std::this_thread::sleep_until(nextTick);
			const auto now = std::chrono::steady_clock::now();
			if (now > nextTick + interval)
			{
				nextTick = now;  // Never spiral after a stall.
			}
		}

		// --- Stop: session first, then the render thread (as the GUI does) ---
		if (m_session)
			m_session->Stop();
		m_renderThread->Stop();
	}
	catch (const std::exception& e)
	{
		NEURUS_ERR("[neurus_headless] Fatal: " << e.what());
		if (m_session)
			m_session->Stop();
		if (m_renderThread)
			m_renderThread->Stop();
		return 1;
	}

	return 0;
}

bool HeadlessApplication::CreateDefaultDocument(std::string& error)
{
	try
	{
		m_editor->NewScene();
		const std::filesystem::path obj = ExecutableDirectory() / kDefaultSceneObj;
		m_editor->CreateDefaultScene(obj.string());
		m_editor->ClearDirty();
		m_editor->UploadSceneResources();
	}
	catch (const std::exception& e)
	{
		error = e.what();
		return false;
	}
	return true;
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
	return m_sessionName;
}

std::string HeadlessApplication::ProjectPath() const
{
	return m_projectPath;
}

bool HeadlessApplication::IsDirty() const
{
	return m_editor && m_editor->IsDirty();
}

bool HeadlessApplication::Save(std::string& error)
{
	if (m_projectPath.empty())
	{
		error = "No project path is set.";
		return false;
	}
	return SaveProject(m_projectPath, *m_editor, {}, error);
}

bool HeadlessApplication::SaveAs(const std::string& path, std::string& error)
{
	if (!SaveProject(path, *m_editor, {}, error))
		return false;
	m_projectPath = path;
	return true;
}

bool HeadlessApplication::NewProject(bool force, std::string& error)
{
	if (IsDirty() && !force)
	{
		error = "The project has unsaved changes; pass force to discard them.";
		return false;
	}
	if (!CreateDefaultDocument(error))
		return false;
	m_projectPath.clear();
	return true;
}

bool HeadlessApplication::OpenProject(const std::string& path, bool force, std::string& error)
{
	if (IsDirty() && !force)
	{
		error = "The project has unsaved changes; pass force to discard them.";
		return false;
	}

	std::string uiLayout;
	if (!LoadProject(path, *m_editor, uiLayout, error))
	{
		// A failed load dropped the previous document; leave a valid one behind.
		std::string resetError;
		if (!CreateDefaultDocument(resetError))
			NEURUS_ERR("[neurus_headless] Recovery after a failed open failed: " << resetError);
		return false;
	}

	m_projectPath = path;
	m_editor->UploadSceneResources();
	return true;
}

int HeadlessApplication::SceneObjectCount() const
{
	return m_editor ? static_cast<int>(m_editor->GetScene().obj_list.size()) : 0;
}

std::int64_t HeadlessApplication::UptimeSeconds() const
{
	const auto elapsed = std::chrono::steady_clock::now() - m_startTime;
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
	if (!m_script || !m_editor || !m_preferences)
	{
		return command::CommandResult::Failure(command::CommandErrorCode::Internal,
		                                      "Scripting is not initialized.");
	}
	return ExecuteScriptCommand(request, *m_script, *m_editor, *m_preferences,
	                            ScriptProjectRoot());
}

std::string HeadlessApplication::ScriptProjectRoot() const
{
	std::filesystem::path root;
	if (!m_projectPath.empty())
	{
		root = std::filesystem::path(m_projectPath).parent_path();
	}
	if (root.empty())
	{
		std::error_code error;
		root = std::filesystem::current_path(error);
		if (error)
		{
			NEURUS_ERR("[neurus_headless] cannot resolve the working directory for the "
			           "script sandbox: " << error.message());
			return {};
		}
	}

	// Python receives UTF-8; do not rely on path::string() (ANSI on MSVC).
	const std::u8string utf8 = root.u8string();
	return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

// ---------------------------------------------------------------------------
// command::ISnapshotCommands
// ---------------------------------------------------------------------------

command::CommandResult HeadlessApplication::Capture(const std::string& path, bool all)
{
	if (!m_renderThread)
	{
		return command::CommandResult::Failure(command::CommandErrorCode::Internal,
		                                      "The render thread is not available.");
	}

	std::error_code pathError;
	const std::filesystem::path requested = std::filesystem::absolute(path, pathError);
	const std::filesystem::path base = pathError ? std::filesystem::path(path) : requested;
	const std::string session = m_sessionName.empty() ? "session" : m_sessionName;

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
		auto future = m_renderThread->PostScreenshotSync(true, directory);
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

	auto future = m_renderThread->PostScreenshotSync(false, scratch);
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
