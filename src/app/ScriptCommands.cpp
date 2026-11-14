#include "app/ScriptCommands.h"

#include "app/Preferences.h"
#include "command/Commands.h"
#include "editor/Editor.h"
#include "script/ScriptConsole.h"
#include "script/ScriptEngine.h"
#include "script/ScriptPolicy.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace neurus
{
namespace
{

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

command::CommandResult ExecuteScriptCommand(const command::CommandRequest& request,
                                            ScriptEngine& script,
                                            Editor& editor,
                                            const Preferences& preferences,
                                            const std::string& projectRoot)
{
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

} // namespace neurus
