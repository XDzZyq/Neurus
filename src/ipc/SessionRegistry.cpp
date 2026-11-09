#include "ipc/SessionRegistry.h"

#include "platform/PlatformPaths.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <random>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace neurus::ipc
{
namespace
{

using Json = nlohmann::json;

int CurrentPid()
{
#ifdef _WIN32
	return _getpid();
#else
	return static_cast<int>(::getpid());
#endif
}

std::optional<std::string> ReadEnvironment(const char* name)
{
#ifdef _WIN32
	char* value = nullptr;
	size_t length = 0;
	if (_dupenv_s(&value, &length, name) == 0 && value)
	{
		std::string result(value);
		std::free(value);
		if (!result.empty())
			return result;
	}
	std::free(value);
	return std::nullopt;
#else
	const char* value = std::getenv(name);
	if (value && *value)
		return std::string(value);
	return std::nullopt;
#endif
}

Json ToJson(const SessionRecord& record)
{
	return Json{
		{"format_version", record.formatVersion},
		{"session", record.session},
		{"pid", record.pid},
		{"port", record.port},
		{"token", record.token},
		{"project", record.project},
		{"app_version", record.appVersion},
		{"protocol_version", record.protocolVersion},
		{"started_at", record.startedAt},
	};
}

std::optional<SessionRecord> FromJson(const Json& json)
{
	if (!json.is_object())
		return std::nullopt;

	SessionRecord record;
	record.formatVersion = json.value("format_version", 1);
	record.session = json.value("session", std::string{});
	record.pid = json.value("pid", 0);
	const int port = json.value("port", 0);
	if (port < 0 || port > 65535)
		return std::nullopt;
	record.port = static_cast<std::uint16_t>(port);
	record.token = json.value("token", std::string{});
	record.project = json.value("project", std::string{});
	record.appVersion = json.value("app_version", std::string{});
	record.protocolVersion = json.value("protocol_version", 1);
	record.startedAt = json.value("started_at", std::string{});
	return record;
}

bool CreateExclusiveFile(const std::filesystem::path& path, const std::string& contents)
{
#ifdef _WIN32
	const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
	                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle == INVALID_HANDLE_VALUE)
		return false;
	DWORD written = 0;
	const bool ok = WriteFile(handle, contents.data(), static_cast<DWORD>(contents.size()),
	                          &written, nullptr) != 0;
	CloseHandle(handle);
	return ok;
#else
	const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
	if (fd < 0)
		return false;
	const ssize_t written = ::write(fd, contents.data(), contents.size());
	::close(fd);
	return written == static_cast<ssize_t>(contents.size());
#endif
}

bool FileExists(const std::filesystem::path& path)
{
	std::error_code error;
	return std::filesystem::exists(path, error);
}

std::string SanitizeBase(std::string value)
{
	std::string result;
	result.reserve(value.size());
	for (const char character : value)
	{
		const bool allowed = (character >= 'a' && character <= 'z')
			|| (character >= 'A' && character <= 'Z')
			|| (character >= '0' && character <= '9')
			|| character == '-' || character == '_';
		result.push_back(allowed ? character : '-');
	}
	while (!result.empty() && (result.front() == '-' || result.front() == '_'))
		result.erase(result.begin());
	if (result.empty())
		result = "neurus";
	if (result.size() > 48)
		result.resize(48);
	return result;
}

std::string RandomSuffix()
{
	std::random_device device;
	std::mt19937 engine(device());
	std::uniform_int_distribution<int> distribution(0, 35);
	constexpr const char* kAlphabet = "abcdefghijklmnopqrstuvwxyz0123456789";
	std::string suffix;
	suffix.reserve(6);
	for (int i = 0; i < 6; ++i)
		suffix.push_back(kAlphabet[distribution(engine)]);
	return suffix;
}

} // namespace

std::filesystem::path ResolveSessionsDirectory()
{
	if (const auto overridePath = ReadEnvironment("NEURUS_SESSIONS_DIR"))
		return std::filesystem::path(*overridePath);
	return SessionsDirectory();
}

bool IsValidSessionName(std::string_view name)
{
	if (name.empty() || name.size() > 64)
		return false;
	const auto isHead = [](char c)
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
	};
	const auto isTail = [&](char c)
	{
		return isHead(c) || c == '-' || c == '_';
	};
	if (!isHead(name.front()))
		return false;
	return std::all_of(name.begin() + 1, name.end(), isTail);
}

bool IsProcessAlive(int pid)
{
	if (pid <= 0)
		return false;
#ifdef _WIN32
	const HANDLE handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
	                                  FALSE, static_cast<DWORD>(pid));
	if (handle == nullptr)
		return false;
	DWORD exitCode = 0;
	const bool alive = GetExitCodeProcess(handle, &exitCode) != 0 && exitCode == STILL_ACTIVE;
	CloseHandle(handle);
	return alive;
#else
	if (::kill(static_cast<pid_t>(pid), 0) == 0)
		return true;
	return errno == EPERM;
#endif
}

SessionRegistry::SessionRegistry(std::filesystem::path directory)
	: m_directory(std::move(directory))
{
}

const std::filesystem::path& SessionRegistry::Directory() const
{
	return m_directory;
}

std::filesystem::path SessionRegistry::PathFor(std::string_view name, std::string_view extension) const
{
	std::filesystem::path path = m_directory;
	path /= std::string(name);
	path += extension;
	return path;
}

bool SessionRegistry::EnsureDirectory(std::string& error) const
{
	std::error_code code;
	std::filesystem::create_directories(m_directory, code);
	if (code)
	{
		error = "Cannot create session directory: " + code.message();
		return false;
	}
#ifndef _WIN32
	std::filesystem::permissions(m_directory,
	                             std::filesystem::perms::owner_all,
	                             std::filesystem::perm_options::replace, code);
	if (code)
	{
		error = "Cannot secure session directory: " + code.message();
		return false;
	}
#endif
	return true;
}

bool SessionRegistry::TryClaim(std::string_view name, std::string& error)
{
	if (!IsValidSessionName(name))
	{
		error = "Invalid session name.";
		return false;
	}
	if (!EnsureDirectory(error))
		return false;

	const auto path = PathFor(name, ".claim");
	for (int attempt = 0; attempt < 2; ++attempt)
	{
		if (CreateExclusiveFile(path, std::to_string(CurrentPid())))
			return true;

		std::error_code code;
		if (!std::filesystem::exists(path, code))
		{
			error = "Cannot create session claim.";
			return false;
		}

		// A claim left by a dead launcher must not block the name forever.
		int owner = 0;
		{
			std::ifstream in(path);
			in >> owner;
		}
		if (owner > 0 && IsProcessAlive(owner))
		{
			error = "Session name is already claimed: " + std::string(name);
			return false;
		}
		std::error_code removeError;
		std::filesystem::remove(path, removeError);
		if (removeError)
		{
			error = "Cannot remove a stale session claim: " + removeError.message();
			return false;
		}
	}
	error = "Cannot claim session name.";
	return false;
}

void SessionRegistry::ReleaseClaim(std::string_view name)
{
	if (!IsValidSessionName(name))
		return;
	std::error_code code;
	std::filesystem::remove(PathFor(name, ".claim"), code);
}

bool SessionRegistry::Write(const SessionRecord& record, std::string& error)
{
	if (!IsValidSessionName(record.session))
	{
		error = "Invalid session name.";
		return false;
	}
	if (!EnsureDirectory(error))
		return false;

	const auto target = PathFor(record.session, ".json");
	const auto temporary = PathFor(record.session, ".json.tmp");

	{
		std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
		if (!out.is_open())
		{
			error = "Cannot open session entry for writing.";
			return false;
		}
		out << ToJson(record).dump(2);
		if (!out.good())
		{
			error = "Cannot write session entry.";
			return false;
		}
	}
#ifndef _WIN32
	std::error_code permissionError;
	std::filesystem::permissions(temporary,
	                             std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
	                             std::filesystem::perm_options::replace, permissionError);
#endif

	std::error_code code;
	std::filesystem::remove(target, code);
	code.clear();
	std::filesystem::rename(temporary, target, code);
	if (code)
	{
		std::error_code ignored;
		std::filesystem::remove(temporary, ignored);
		error = "Cannot publish session entry: " + code.message();
		return false;
	}
	return true;
}

std::optional<SessionRecord> SessionRegistry::Read(std::string_view name) const
{
	if (!IsValidSessionName(name))
		return std::nullopt;

	std::ifstream in(PathFor(name, ".json"), std::ios::binary);
	if (!in.is_open())
		return std::nullopt;

	Json json;
	try
	{
		in >> json;
	}
	catch (const nlohmann::json::exception&)
	{
		return std::nullopt;
	}
	auto record = FromJson(json);
	if (!record)
		return std::nullopt;
	if (record->session.empty())
		record->session = std::string(name);
	return record;
}

bool SessionRegistry::Remove(std::string_view name) const
{
	if (!IsValidSessionName(name))
		return false;
	std::error_code code;
	return std::filesystem::remove(PathFor(name, ".json"), code);
}

std::vector<SessionRecord> SessionRegistry::ReadAll() const
{
	std::vector<SessionRecord> records;
	std::error_code code;
	if (!std::filesystem::is_directory(m_directory, code))
		return records;

	for (const auto& entry : std::filesystem::directory_iterator(m_directory, code))
	{
		if (code)
			break;
		if (!entry.is_regular_file())
			continue;
		if (entry.path().extension() != ".json")
			continue;

		const std::string name = entry.path().stem().string();
		if (auto record = Read(name))
			records.push_back(std::move(*record));
	}
	std::sort(records.begin(), records.end(),
	          [](const SessionRecord& left, const SessionRecord& right)
	          {
		          return left.session < right.session;
	          });
	return records;
}

std::vector<SessionRecord> SessionRegistry::ListLive() const
{
	std::vector<SessionRecord> live;
	for (auto& record : ReadAll())
	{
		if (IsProcessAlive(record.pid))
			live.push_back(std::move(record));
		else
			Remove(record.session);
	}
	return live;
}

std::string AllocateSessionName(const std::string& projectPath, const SessionRegistry& registry)
{
	std::string base = "neurus";
	if (!projectPath.empty())
	{
		std::filesystem::path path(projectPath);
		std::string filename = path.filename().string();
		constexpr const char* kProjectSuffix = ".neurus.json";
		constexpr std::size_t kSuffixLength = 12;
		if (filename.size() > kSuffixLength
		    && filename.compare(filename.size() - kSuffixLength, kSuffixLength, kProjectSuffix) == 0)
		{
			filename.resize(filename.size() - kSuffixLength);
		}
		else
		{
			filename = path.stem().string();
		}
		base = SanitizeBase(filename);
	}

	for (int attempt = 0; attempt < 64; ++attempt)
	{
		const std::string candidate = base + "-" + RandomSuffix();
		if (!registry.Read(candidate))
			return candidate;
	}
	return {};
}

} // namespace neurus::ipc
