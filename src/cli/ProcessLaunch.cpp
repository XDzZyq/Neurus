#include "cli/ProcessLaunch.h"

#include <array>
#include <cstdlib>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace neurus::cli
{
namespace
{

bool IsFile(const std::filesystem::path& path)
{
	std::error_code error;
	return std::filesystem::is_regular_file(path, error);
}

std::filesystem::path AppFileName()
{
#ifdef _WIN32
	return "Neurus.exe";
#else
	return "Neurus";
#endif
}

#ifdef _WIN32
std::wstring QuoteArgument(const std::wstring& argument)
{
	if (argument.empty())
		return L"\"\"";

	if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
		return argument;

	std::wstring result = L"\"";
	for (auto it = argument.begin();; ++it)
	{
		std::size_t backslashes = 0;
		while (it != argument.end() && *it == L'\\')
		{
			++it;
			++backslashes;
		}
		if (it == argument.end())
		{
			result.append(backslashes * 2, L'\\');
			break;
		}
		if (*it == L'"')
		{
			result.append(backslashes * 2 + 1, L'\\');
			result.push_back(*it);
		}
		else
		{
			result.append(backslashes, L'\\');
			result.push_back(*it);
		}
	}
	result.push_back(L'"');
	return result;
}
#endif

} // namespace

std::optional<std::filesystem::path> ResolveAppExecutable(
	const std::filesystem::path& cliPath,
	const std::optional<std::filesystem::path>& overridePath,
	const std::string& envValue)
{	if (overridePath && IsFile(*overridePath))
		return *overridePath;
	if (!envValue.empty() && IsFile(envValue))
		return std::filesystem::path(envValue);

	const std::filesystem::path base = cliPath.empty()
		? std::filesystem::current_path()
		: std::filesystem::absolute(cliPath);
	const auto directory = base.parent_path();
	const auto fileName = AppFileName();

	const auto parent = directory.parent_path() / fileName;
	if (IsFile(parent))
		return parent;

	const auto sibling = directory / fileName;
	if (IsFile(sibling))
		return sibling;

	const auto grandparent = directory.parent_path().parent_path() / fileName;
	if (IsFile(grandparent))
		return grandparent;

	return std::nullopt;
}

std::optional<int> SpawnDetached(const std::filesystem::path& app,
                                 const std::vector<std::string>& arguments,
                                 const std::filesystem::path& logPath,
                                 std::string& error)
{
	std::error_code directoryError;
	std::filesystem::create_directories(logPath.parent_path(), directoryError);

#ifdef _WIN32
	std::wstring commandLine = QuoteArgument(app.wstring());
	for (const auto& argument : arguments)
	{
		commandLine.push_back(L' ');
		commandLine += QuoteArgument(std::wstring(argument.begin(), argument.end()));
	}

	SECURITY_ATTRIBUTES attributes{};
	attributes.nLength = sizeof(attributes);
	attributes.bInheritHandle = TRUE;

	const HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
	                               &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (log == INVALID_HANDLE_VALUE)
	{
		error = "Cannot open the session log.";
		return std::nullopt;
	}

	// Inherit only the log handle. Passing TRUE to CreateProcessW without a
	// handle list would also leak the caller's stdout pipe into the detached
	// child, which keeps a terminal or test harness waiting for EOF.
	SIZE_T attributeSize = 0;
	InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
	std::vector<unsigned char> attributeBuffer(attributeSize);
	auto* attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());
	if (!InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeSize))
	{
		CloseHandle(log);
		error = "Cannot prepare the child process attributes.";
		return std::nullopt;
	}
	HANDLE inheritedHandles[1] = {log};
	if (!UpdateProcThreadAttribute(attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
	                               inheritedHandles, sizeof(inheritedHandles), nullptr, nullptr))
	{
		DeleteProcThreadAttributeList(attributeList);
		CloseHandle(log);
		error = "Cannot restrict the inherited handles.";
		return std::nullopt;
	}

	STARTUPINFOEXW startup{};
	startup.StartupInfo.cb = sizeof(startup);
	startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	startup.StartupInfo.hStdOutput = log;
	startup.StartupInfo.hStdError = log;
	startup.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
	startup.lpAttributeList = attributeList;

	PROCESS_INFORMATION process{};
	const BOOL created = CreateProcessW(
		app.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
		EXTENDED_STARTUPINFO_PRESENT | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
		nullptr, nullptr, &startup.StartupInfo, &process);
	DeleteProcThreadAttributeList(attributeList);
	CloseHandle(log);

	if (!created)
	{
		error = "Cannot launch the Neurus application.";
		return std::nullopt;
	}

	const int pid = static_cast<int>(process.dwProcessId);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return pid;
#else
	const int pid = fork();
	if (pid < 0)
	{
		error = "Cannot fork the Neurus application.";
		return std::nullopt;
	}
	if (pid == 0)
	{
		setsid();
		const int log = open(logPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
		if (log >= 0)
		{
			dup2(log, STDOUT_FILENO);
			dup2(log, STDERR_FILENO);
			close(log);
		}

		std::vector<std::string> storage;
		storage.push_back(app.string());
		storage.insert(storage.end(), arguments.begin(), arguments.end());
		std::vector<char*> argv;
		argv.reserve(storage.size() + 1);
		for (auto& value : storage)
			argv.push_back(value.data());
		argv.push_back(nullptr);
		execv(app.c_str(), argv.data());
		_exit(127);
	}
	return pid;
#endif
}

std::vector<std::string> BuildAppArguments(const std::string& session,
                                          const std::optional<std::string>& project)
{
	std::vector<std::string> arguments{"--session", session};
	if (project)
	{
		arguments.push_back("--project");
		arguments.push_back(*project);
	}
	return arguments;
}

std::filesystem::path CurrentExecutablePath()
{
#ifdef _WIN32
	std::wstring buffer;
	buffer.resize(32768);
	const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
	                                        static_cast<DWORD>(buffer.size()));
	if (length == 0)
		return {};
	buffer.resize(length);
	return std::filesystem::path(buffer);
#else
	std::array<char, 4096> buffer{};
	const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
	if (length <= 0)
		return {};
	return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(length)));
#endif
}

std::string AppEnvironmentPath()
{
#ifdef _WIN32
	char* value = nullptr;
	size_t length = 0;
	if (_dupenv_s(&value, &length, "NEURUS_APP") == 0 && value)
	{
		std::string result(value);
		std::free(value);
		return result;
	}
	std::free(value);
	return {};
#else
	const char* value = std::getenv("NEURUS_APP");
	return value ? std::string(value) : std::string{};
#endif
}

} // namespace neurus::cli
