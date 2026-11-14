/**
 * @file ApplicationCommands.cpp
 * @brief Command-interface implementations on the running application.
 *
 * Holds the narrow ISessionState view, Python execution and snapshot capture.
 * Project lifecycle and Qt wiring stay in Application.cpp / ApplicationSignals.cpp.
 */

#include "app/Application.h"

#include "app/ApplicationCore.h"

#include "app/Preferences.h"
#include "app/ScriptCommands.h"
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
	return app_core->SessionName();
}

std::string Application::ProjectPath() const
{
	return app_core->ProjectPath();
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
	// One implementation, shared with the headless renderer (ApplicationCore ->
	// app/ScriptCommands.h), so the CLI sees the same script behaviour on every
	// platform. The core owns the engine and the host sandbox policy.
	if (!app_editor)
	{
		return command::CommandResult::Failure(command::CommandErrorCode::Internal,
		                                      "The editor is not available.");
	}
	return app_core->ExecuteScript(request, *app_editor);
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
	const std::string session = app_core->SessionName().empty() ? "session" : app_core->SessionName();

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
