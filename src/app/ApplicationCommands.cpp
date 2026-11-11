/**
 * @file ApplicationCommands.cpp
 * @brief Command-interface implementations on the running application.
 *
 * Holds the narrow ISessionState view, Python execution and snapshot capture.
 * Project lifecycle and Qt wiring stay in Application.cpp / ApplicationSignals.cpp.
 */

#include "app/Application.h"

#include "command/Commands.h"
#include "platform/PlatformProcess.h"
#include "scene/Scene.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

const char* ScriptKindName(neurus::ScriptLineKind kind)
{
	switch (kind)
	{
	case neurus::ScriptLineKind::Input: return "input";
	case neurus::ScriptLineKind::Output: return "output";
	case neurus::ScriptLineKind::Error: return "error";
	case neurus::ScriptLineKind::Result: return "result";
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

} // namespace

namespace neurus {

bool Application::IsDirty() const
{
	return app_editor->IsDirty() || (app_mainWindow->ExportLayout() != app_savedUiState);
}

// --- ISessionState -------------------------------------------------------

int Application::Pid() const
{
	return neurus::CurrentProcessId();
}

std::string Application::SessionName() const
{
	return app_sessionName;
}

std::string Application::ProjectPath() const
{
	return app_projectPath;
}

bool Application::Save(std::string& error)
{
	return OnProjectSave({}, &error);
}

bool Application::SaveAs(const std::string& path, std::string& error)
{
	return OnProjectSave(path, &error);
}

bool Application::NewProject(bool force, std::string& error)
{
	if (IsDirty() && !force)
	{
		error = "The project has unsaved changes; pass force to discard them.";
		return false;
	}
	return OnProjectNew(&error);
}

bool Application::OpenProject(const std::string& path, bool force, std::string& error)
{
	if (IsDirty() && !force)
	{
		error = "The project has unsaved changes; pass force to discard them.";
		return false;
	}
	return OnProjectOpen(path, &error);
}

int Application::SceneObjectCount() const
{
	return app_editor ? static_cast<int>(app_editor->GetScene().obj_list.size()) : 0;
}

command::CommandResult Application::Execute(const command::CommandRequest& request)
{
	const command::Json& args = request.args;
	ScriptConsoleBuffer* console = app_script ? &app_script->Console() : nullptr;

	if (request.name == command::kScriptInfo)
	{
		const bool ready = app_script && app_script->IsReady();
		return command::CommandResult::Success({
			{"ready", ready},
			{"python_version", app_script ? app_script->PythonVersion() : std::string{}},
			{"environment", app_script ? app_script->EnvironmentPath() : std::string{}},
			{"console_lines", console ? console->Size() : 0},
		});
	}

	if (request.name == command::kScriptConsole)
	{
		if (!console)
		{
			return command::CommandResult::Failure(command::CommandErrorCode::Internal,
			                                      "Scripting is not initialized.");
		}
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

		const std::size_t total = console->Size();
		const std::size_t first = total > static_cast<std::size_t>(tail)
			? total - static_cast<std::size_t>(tail)
			: 0;
		command::Json lines = command::Json::array();
		std::size_t emitted = 0;
		bool truncated = false;
		for (std::size_t i = first; i < total; ++i)
		{
			const ScriptLine line = console->At(i);
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
		if (!console)
		{
			return command::CommandResult::Failure(command::CommandErrorCode::Internal,
			                                      "Scripting is not initialized.");
		}
		console->Clear();
		return command::CommandResult::Success({{"cleared", true}});
	}

	const bool isEval = request.name == command::kScriptEval;
	if (!isEval && request.name != command::kScriptRun)
	{
		return command::CommandResult::Failure(command::CommandErrorCode::InvalidArgument,
		                                      "Unknown command: " + request.name);
	}
	if (!app_script || !app_script->IsReady())
	{
		return command::CommandResult::Failure(command::CommandErrorCode::Internal,
		                                      "Scripting is not initialized.");
	}
	if (!app_editor)
	{
		return command::CommandResult::Failure(command::CommandErrorCode::Internal,
		                                      "The editor is not available.");
	}

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

	ScriptConsoleBuffer& stream = app_script->Console();
	std::uint64_t lastSeq = 0;
	if (stream.Size() > 0)
		lastSeq = stream.At(stream.Size() - 1).seq;

	ScriptContext context;
	app_editor->UpdateScriptContext(context);

	std::string result;
	bool runOk = false;
	if (isEval)
	{
		const std::optional<std::string> evaluated =
			app_script->EvalStringChecked(code, &context);
		runOk = evaluated.has_value();
		result = evaluated.value_or(std::string{});
	}
	else if (hasPath)
	{
		runOk = app_script->RunFileChecked(path, &context);
	}
	else
	{
		runOk = app_script->RunStringChecked(code, &context);
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
		const std::string message = scriptError.empty()
			? "The script raised an exception."
			: scriptError;
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

command::CommandResult Application::Capture(const std::string& path, bool all)
{
	if (!app_renderThread)
	{
		return command::CommandResult::Failure(command::CommandErrorCode::Internal,
		                                      "The render thread is not available.");
	}

	std::error_code pathError;
	const std::filesystem::path requested = std::filesystem::absolute(path, pathError);
	const std::filesystem::path base = pathError ? std::filesystem::path(path) : requested;
	const std::string session = app_sessionName.empty() ? "session" : app_sessionName;

	auto listFiles = [](const std::filesystem::path& directory)
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
	};
	auto filesJson = [](const std::vector<std::filesystem::path>& files)
	{
		command::Json list = command::Json::array();
		for (const auto& file : files)
			list.push_back(file.string());
		return list;
	};
	auto waitForCapture = [this](std::future<bool>& future)
	{
		if (future.wait_for(std::chrono::seconds(55)) != std::future_status::ready)
		{
			return command::CommandResult::Failure(
				command::CommandErrorCode::Timeout,
				"Timed out waiting for the render thread to write the snapshot.");
		}
		try
		{
			(void)future.get();
			return command::CommandResult::Success();
		}
		catch (const std::exception& e)
		{
			return command::CommandResult::Failure(
				command::CommandErrorCode::IoError,
				std::string("The snapshot capture failed: ") + e.what());
		}
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

		const auto before = listFiles(directory);
		auto future = app_renderThread->PostScreenshotSync(true, directory);
		const command::CommandResult waited = waitForCapture(future);
		if (!waited.ok)
			return waited;

		const auto after = listFiles(directory);
		std::set<std::filesystem::path> existing(before.begin(), before.end());
		std::vector<std::filesystem::path> files;
		for (const auto& file : after)
		{
			if (existing.count(file) == 0)
				files.push_back(file);
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
			{"files", filesJson(files)},
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

	const std::filesystem::path finalPath = parent / (stem + "-" + session + ".png");
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const std::filesystem::path temporary =
		parent / (".neurus-capture-" + session + "-" + std::to_string(stamp));
	std::filesystem::create_directories(temporary, directoryError);
	if (directoryError)
	{
		return command::CommandResult::Failure(
			command::CommandErrorCode::IoError,
			"Cannot create the capture directory: " + directoryError.message(),
			{{"path", temporary.string()}});
	}

	auto future = app_renderThread->PostScreenshotSync(false, temporary);
	const command::CommandResult waited = waitForCapture(future);
	if (!waited.ok)
	{
		std::filesystem::remove_all(temporary, directoryError);
		return waited;
	}

	const auto files = listFiles(temporary);
	if (files.empty())
	{
		std::filesystem::remove_all(temporary, directoryError);
		return command::CommandResult::Failure(
			command::CommandErrorCode::IoError,
			"The swapchain capture wrote no file.",
			{{"path", temporary.string()}});
	}

	std::filesystem::path captured = files.front();
	for (const auto& file : files)
	{
		if (file.filename().string().find("swapchain") != std::string::npos)
		{
			captured = file;
			break;
		}
	}

	std::filesystem::remove(finalPath, directoryError);
	std::filesystem::rename(captured, finalPath, directoryError);
	if (directoryError)
	{
		directoryError.clear();
		std::filesystem::copy_file(captured, finalPath,
		                           std::filesystem::copy_options::overwrite_existing,
		                           directoryError);
	}
	std::filesystem::remove_all(temporary, directoryError);
	if (directoryError)
	{
		return command::CommandResult::Failure(
			command::CommandErrorCode::IoError,
			"Cannot publish the snapshot: " + directoryError.message(),
			{{"path", finalPath.string()}});
	}
	if (!std::filesystem::is_regular_file(finalPath, directoryError))
	{
		return command::CommandResult::Failure(
			command::CommandErrorCode::IoError,
			"The snapshot file was not written.",
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

std::int64_t Application::UptimeSeconds() const
{
	return std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::steady_clock::now() - app_startTime).count();
}

std::string Application::AppVersion() const
{
	return app_qtApp->applicationVersion().toStdString();
}
} // namespace neurus
