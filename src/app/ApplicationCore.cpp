#include "app/ApplicationCore.hpp"

#include "app/Preferences.hpp"

#include "asset/Project.hpp"
#include "shader/BuiltInRenderData.hpp"
#include "shader/registrations/ShaderRegistration.hpp"
#include "asset/components/ConfigComponent.hpp"
#include "editor/project/EditorComponent.hpp"
#include "asset/components/GraphComponent.hpp"
#include "editor/project/HistoryComponent.hpp"
#include "asset/components/ResourceComponent.hpp"
#include "asset/data/render_graph/RenderGraphRecord.hpp"
#include "asset/components/SceneComponent.hpp"
#include "asset/components/UIComponent.hpp"
#include "command/Commands.hpp"
#include "core/Log.hpp"
#include "editor/Editor.hpp"
#include "editor/RenderManager.hpp"
#include "script/ScriptConsole.hpp"
#include "script/ScriptEngine.hpp"
#include "script/ScriptPolicy.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace neurus
{
namespace
{

/**
 * @brief Converts a path to the UTF-8 string the editor and project layer take.
 *
 * path::string() is the ANSI code page on MSVC and throws beyond it.
 */
std::string Utf8Path(const std::filesystem::path& path)
{
	const std::u8string utf8 = path.u8string();
	return std::string(utf8.begin(), utf8.end());
}

/**
 * @brief Registers the live editor components on a project serializer.
 *
 * Order matters: the resource pool is registered first, before the scene
 * resolves its references against it.
 *
 * @param uiLayout Opaque UI layout blob, carried through unchanged. A headless
 *                 caller passes an empty string.
 */
void RegisterProjectComponents(project::Project& proj, Editor& editor, std::string& uiLayout)
{
	// Resource pool first: the Scene's ID references resolve against it.
	auto& render = editor.GetRenderManager();
	auto& resources = editor.GetResourceManager();
	proj.Register<project::ResourceComponent>(resources, neurus::project::MakeGraphReadServices(render.Defaults(), render.GraphDiagnostics()));
	proj.Register<project::SceneComponent>(editor.GetScene(), editor.GetResourceManager());
	proj.Register<project::ConfigComponent>(editor.GetRenderConfigData());

	proj.Register<project::GraphComponent>(render.GraphData(), resources, render.Problems());
	proj.Register<project::UIComponent>(uiLayout);
	// History, then editor view state: both are appended-last nodes, so legacy
	// files that lack them load cleanly (each Load() falls back).
	proj.Register<project::HistoryComponent>(editor.GetOperations());
	proj.Register<project::EditorComponent>(editor.GetViewport());
}

const char* ScriptKindName(ScriptLineKind kind)
{
	switch (kind)
	{
	case ScriptLineKind::Input: return "input";
	case ScriptLineKind::Output: return "output";
	case ScriptLineKind::Error: return "error";
	case ScriptLineKind::Result: return "result";
	}
	return "unknown";
}

constexpr std::size_t kMaxScriptOutput = 1024 * 1024;
constexpr std::size_t kMaxConsoleLine = 64 * 1024;

void AppendLimited(std::string& target, const std::string& text, std::size_t limit)
{
	if (target.size() >= limit)
		return;

	const std::size_t remaining = limit - target.size();
	if (text.size() <= remaining)
	{
		target += text;
		return;
	}
	target.append(text, 0, remaining);
	target += "\n[truncated]\n";
}

command::Json StringArrayJson(const std::vector<std::string>& values)
{
	command::Json array = command::Json::array();
	for (const auto& value : values)
		array.push_back(value);
	return array;
}

} // namespace

ApplicationCore::ApplicationCore()
	: m_renderDefaults(std::make_shared<const BuiltInRenderData>())
	, m_preferences(std::make_unique<Preferences>())
	, m_script(std::make_unique<ScriptEngine>())
{
}

ApplicationCore::~ApplicationCore() = default;

void ApplicationCore::LoadPreferences()
{
	const std::string path = Preferences::DefaultPath();
	const Preferences::LoadResult result = m_preferences->Load(path);
	if (result == Preferences::LoadResult::Corrupt)
	{
		NEURUS_ERR("[ApplicationCore] Preferences at " << path
		          << " are corrupt; moving them aside and using defaults");
		Preferences::Backup(path);
	}
}

bool ApplicationCore::InitializeScripting(Editor& editor, const std::vector<std::string>& preload)
{
	m_script->SetEventQueue(&editor.GetEventQueue());
	if (!m_script->Initialize())
	{
		NEURUS_ERR("[ApplicationCore] Script engine failed to initialize; "
		          "script.* commands will report it as unavailable");
		return false;
	}

	if (!preload.empty())
	{
		// Before the first frame: a heavy import is paid here, once, instead of
		// stalling the first script that needs it. A failure is reported and does
		// not abort startup. The line also lands in the console buffer, which is
		// what the UI panel and `script console` read.
		for (const ScriptPreloadResult& result : m_script->Preload(preload))
		{
			std::ostringstream line;
			if (!result.ok)
			{
				line << "preload " << result.package << " failed: " << result.error;
			}
			else
			{
				line << "preload " << result.package << ": " << std::fixed
				     << std::setprecision(3) << result.loadSeconds << "s ("
				     << (result.cached ? "cached" : "loaded") << ")";
			}

			const std::string text = line.str();
			m_script->Console().Append(
				result.ok ? ScriptLineKind::Output : ScriptLineKind::Error, text + "\n");
			if (result.ok)
			{
				NEURUS_LOG("[ApplicationCore] " << text);
			}
			else
			{
				NEURUS_ERR("[ApplicationCore] " << text);
			}
		}
	}
	UpdateScriptReloadRoots();
	return true;
}

bool ApplicationCore::CreateDefaultDocument(Editor& editor, const std::filesystem::path& objPath,
                                            std::string& error)
{
	try
	{
		editor.NewScene();
		editor.CreateDefaultScene(Utf8Path(objPath));
		editor.ClearDirty();
		editor.UploadSceneResources();
	}
	catch (const std::exception& e)
	{
		error = e.what();
		NEURUS_ERR("[ApplicationCore] Failed to create the default document: " << e.what());
		return false;
	}

	// A new document has no file yet: clearing the path here is what stops a
	// failed project.open from leaving a later save pointed at the old project.
	m_projectPath.clear();
	UpdateScriptReloadRoots();
	return true;
}

bool ApplicationCore::LoadProject(const std::filesystem::path& path, Editor& editor,
                                  std::string& uiLayout, std::string& error)
{
	std::error_code fileError;
	if (!std::filesystem::is_regular_file(path, fileError))
	{
		error = "Project file not found: " + Utf8Path(path);
		NEURUS_ERR("[ApplicationCore] " << error);
		return false;
	}

	try
	{
		editor.BeginLoad();
		project::Project proj;
		RegisterProjectComponents(proj, editor, uiLayout);
		proj.Load(Utf8Path(path));
		editor.FinishLoad();
	}
	catch (const std::exception& e)
	{
		error = e.what();
		NEURUS_ERR("[ApplicationCore] Failed to open project: " << e.what());
		return false;
	}

	m_projectPath = Utf8Path(path);
	UpdateScriptReloadRoots();
	return true;
}

bool ApplicationCore::SaveProject(const std::filesystem::path& path, Editor& editor,
                                  const std::string& uiLayout, std::string& error)
{
	if (path.empty())
	{
		error = "No project path is set.";
		NEURUS_ERR("[ApplicationCore] " << error);
		return false;
	}

	try
	{
		std::string layout = uiLayout;
		project::Project proj;
		RegisterProjectComponents(proj, editor, layout);
		proj.Save(Utf8Path(path));
		editor.ClearDirty();
	}
	catch (const std::exception& e)
	{
		error = e.what();
		NEURUS_ERR("[ApplicationCore] Failed to save project: " << e.what());
		return false;
	}

	m_projectPath = Utf8Path(path);
	UpdateScriptReloadRoots();
	return true;
}

command::CommandResult ApplicationCore::ExecuteScript(const command::CommandRequest& request,
                                                      Editor& editor)
{
	ScriptEngine& script = *m_script;
	const Preferences& preferences = *m_preferences;
	const std::string projectRoot = ScriptProjectRoot();

	if (!script.IsReady())
	{
		return command::CommandResult::Failure(command::CommandErrorCode::Internal,
		                                      "Scripting is not initialized.");
	}

	const command::Json& args = request.args;
	ScriptConsoleBuffer& console = script.Console();

	if (request.name == command::kScriptInfo)
	{
		const bool ready = script.IsReady();

		// Bound the module list so a long session's `script info` stays small;
		// the truncated flag tells the caller the count is a prefix.
		constexpr std::size_t kMaxReportedModules = 200;
		command::Json modules = command::Json::array();
		std::size_t moduleCount = 0;
		if (ready)
		{
			const std::vector<ScriptModuleInfo> loaded = script.LoadedModules();
			moduleCount = loaded.size();
			const std::size_t limit = std::min<std::size_t>(loaded.size(), kMaxReportedModules);
			for (std::size_t i = 0; i < limit; ++i)
			{
				const ScriptModuleInfo& module = loaded[i];
				modules.push_back({
					{"name", module.name},
					{"origin", module.origin},
					{"seconds", module.loadSeconds},
					{"project", module.projectLocal},
					{"cached", module.cached},
				});
			}
		}

		// The summary is the host policy every external script command uses, not
		// a record of the last run. UI runs are trusted and never see this policy.
		const ScriptPolicy sandboxPolicy = preferences.MakeScriptPolicy(projectRoot);
		const std::uint64_t memoryLimitMb =
			sandboxPolicy.memoryLimitBytes / (1024ULL * 1024ULL);
		return command::CommandResult::Success({
			{"ready", ready},
			{"python_version", script.PythonVersion()},
			{"environment", script.EnvironmentPath()},
			{"console_lines", console.Size()},
			{"modules", modules},
			{"module_count", moduleCount},
			{"modules_truncated", moduleCount > modules.size()},
			{"sandbox", {
				{"read_roots", StringArrayJson(sandboxPolicy.readRoots)},
				{"write_roots", StringArrayJson(sandboxPolicy.writeRoots)},
				{"blocked_modules", StringArrayJson(sandboxPolicy.blockedModules)},
				{"network", sandboxPolicy.allowNetwork},
				{"subprocess", sandboxPolicy.allowSubprocess},
				{"ctypes", sandboxPolicy.allowCtypes},
				{"time_limit_s", std::chrono::duration<double>(
					sandboxPolicy.wallClockLimit).count()},
				{"memory_limit_mb", memoryLimitMb},
			}},
		});
	}

	if (request.name == command::kScriptConsole)
	{
		int tail = 50;
		if (args.contains("tail"))
		{
			if (!args["tail"].is_number_integer())
			{
				return command::CommandResult::Failure(
					command::CommandErrorCode::InvalidArgument,
					"'tail' must be a positive integer.");
			}
			tail = args["tail"].get<int>();
			if (tail <= 0)
			{
				return command::CommandResult::Failure(
					command::CommandErrorCode::InvalidArgument,
					"'tail' must be a positive integer.");
			}
		}

		const std::size_t total = console.Size();
		const std::size_t first = total > static_cast<std::size_t>(tail)
			? total - static_cast<std::size_t>(tail)
			: 0;
		command::Json lines = command::Json::array();
		std::size_t emitted = 0;
		bool truncated = false;
		for (std::size_t i = first; i < total; ++i)
		{
			const ScriptLine line = console.At(i);
			std::string text = line.text;
			if (text.size() > kMaxConsoleLine)
			{
				text.resize(kMaxConsoleLine);
				text += "\n[truncated]\n";
			}
			if (emitted >= kMaxScriptOutput)
			{
				truncated = true;
				break;
			}
			if (emitted + text.size() > kMaxScriptOutput)
			{
				text.resize(kMaxScriptOutput - emitted);
				text += "\n[truncated]\n";
				truncated = true;
			}
			emitted += text.size();
			lines.push_back({
				{"seq", line.seq},
				{"kind", ScriptKindName(line.kind)},
				{"text", std::move(text)},
			});
			if (truncated)
				break;
		}
		return command::CommandResult::Success({
			{"lines", lines},
			{"count", lines.size()},
			{"total", total},
			{"truncated", truncated},
		});
	}

	if (request.name == command::kScriptClear)
	{
		console.Clear();
		return command::CommandResult::Success({{"cleared", true}});
	}

	const bool isEval = request.name == command::kScriptEval;
	if (!isEval && request.name != command::kScriptRun)
	{
		return command::CommandResult::Failure(command::CommandErrorCode::InvalidArgument,
		                                      "Unknown command: " + request.name);
	}
	if (!script.IsReady())
	{
		return command::CommandResult::Failure(command::CommandErrorCode::Internal,
		                                      "Scripting is not initialized.");
	}

	// External script commands always execute under the host policy. The CLI
	// cannot choose trusted mode or override the policy.
	const ScriptPolicy policy = preferences.MakeScriptPolicy(projectRoot);
	const ScriptPolicy* policyPtr = &policy;

	std::string code;
	std::string path;
	bool hasPath = false;
	if (isEval)
	{
		if (!args.contains("code") || !args["code"].is_string())
		{
			return command::CommandResult::Failure(command::CommandErrorCode::InvalidArgument,
			                                      "'code' must be a string.");
		}
		code = args["code"].get<std::string>();
	}
	else
	{
		const bool hasCode = args.contains("code");
		hasPath = args.contains("path");
		if (hasCode == hasPath)
		{
			return command::CommandResult::Failure(
				command::CommandErrorCode::InvalidArgument,
				"Pass exactly one of 'code' or 'path'.");
		}

		if (hasCode)
		{
			if (!args["code"].is_string())
			{
				return command::CommandResult::Failure(
					command::CommandErrorCode::InvalidArgument, "'code' must be a string.");
			}
			code = args["code"].get<std::string>();
		}
		else
		{
			if (!args["path"].is_string() || args["path"].get<std::string>().empty())
			{
				return command::CommandResult::Failure(
					command::CommandErrorCode::InvalidArgument,
					"'path' must be a non-empty string.");
			}
			path = args["path"].get<std::string>();
			std::error_code fileError;
			if (!std::filesystem::is_regular_file(path, fileError))
			{
				return command::CommandResult::Failure(
					command::CommandErrorCode::IoError,
					"Script file not found: " + path, {{"path", path}});
			}
		}
	}

	ScriptConsoleBuffer& stream = script.Console();
	std::uint64_t lastSeq = 0;
	if (stream.Size() > 0)
		lastSeq = stream.At(stream.Size() - 1).seq;

	ScriptContext context;
	editor.UpdateScriptContext(context);

	std::string result;
	bool runOk = false;
	if (isEval)
	{
		const std::optional<std::string> evaluated =
			script.EvalStringChecked(code, &context, policyPtr);
		runOk = evaluated.has_value();
		result = evaluated.value_or(std::string{});
	}
	else if (hasPath)
	{
		runOk = script.RunFileChecked(path, &context, policyPtr);
	}
	else
	{
		runOk = script.RunStringChecked(code, &context, policyPtr);
	}

	std::string output;
	std::string scriptError;
	const std::size_t lineCount = stream.Size();
	for (std::size_t i = 0; i < lineCount; ++i)
	{
		const ScriptLine line = stream.At(i);
		if (line.seq <= lastSeq)
			continue;
		if (line.kind == ScriptLineKind::Error)
			AppendLimited(scriptError, line.text, kMaxScriptOutput);
		else if ((line.kind == ScriptLineKind::Output
		          || (line.kind == ScriptLineKind::Result && !isEval)))
			AppendLimited(output, line.text, kMaxScriptOutput);
	}

	if (isEval && result.size() > kMaxScriptOutput)
	{
		result.resize(kMaxScriptOutput);
		result += "\n[truncated]\n";
	}

	if (!runOk)
	{
		if (script.LastRunStatus() == ScriptRunStatus::SandboxViolation)
		{
			const std::string message = script.LastRunMessage().empty()
				? "The script was refused by its sandbox policy."
				: script.LastRunMessage();
			return command::CommandResult::Failure(
				command::CommandErrorCode::ScriptSandboxViolation, message,
				{{"output", output}, {"error", scriptError}, {"reason", "sandbox_violation"}});
		}
		if (script.LastRunStatus() == ScriptRunStatus::Timeout)
		{
			return command::CommandResult::Failure(
				command::CommandErrorCode::ScriptError,
				"Script exceeded its wall-clock limit.",
				{{"output", output}, {"error", scriptError}, {"reason", "timeout"}});
		}

		const std::string message =
			!scriptError.empty()
				? scriptError
				: (!script.LastRunMessage().empty()
				       ? script.LastRunMessage()
				       : std::string("The script raised an exception."));
		return command::CommandResult::Failure(
			command::CommandErrorCode::ScriptError, message,
			{{"output", output}, {"error", scriptError}});
	}
	if (isEval)
	{
		return command::CommandResult::Success(
			{{"result", result}, {"output", output}, {"stderr", scriptError}});
	}
	return command::CommandResult::Success({{"output", output}, {"stderr", scriptError}});
}

void ApplicationCore::UpdateScriptReloadRoots()
{
	if (!m_script->IsReady())
	{
		// Startup order: both roots load the project before Python starts, so the
		// document operations above cannot set roots yet. InitializeScripting()
		// calls this again once the interpreter is ready.
		return;
	}

	std::vector<std::string> roots;
	std::error_code error;
	std::filesystem::path directory;
	if (!m_projectPath.empty())
	{
		directory = std::filesystem::path(m_projectPath).parent_path();
	}
	if (directory.empty())
	{
		directory = std::filesystem::current_path(error);
	}
	if (!directory.empty())
	{
		// Python receives UTF-8; generic separators keep the root comparable on
		// Windows, where path::string() would be the ANSI code page.
		const std::u8string utf8 = directory.generic_u8string();
		roots.emplace_back(reinterpret_cast<const char*>(utf8.data()), utf8.size());
	}

	m_script->SetReloadRoots(roots);
	NEURUS_LOG("[ApplicationCore] script reload roots: "
	           << (roots.empty() ? std::string("(none)") : roots.front()));
}

std::string ApplicationCore::ScriptProjectRoot() const
{	std::filesystem::path root;
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
			NEURUS_ERR("[ApplicationCore] cannot resolve the working directory for the "
			           "script sandbox: " << error.message());
			return {};
		}
	}

	// Python receives UTF-8; do not rely on path::string() (ANSI on MSVC).
	const std::u8string utf8 = root.u8string();
	return std::string(utf8.begin(), utf8.end());
}

} // namespace neurus
