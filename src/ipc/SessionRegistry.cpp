#include "ipc/SessionRegistry.h"

#include "platform/PlatformFile.h"
#include "platform/PlatformPaths.h"
#include "platform/PlatformProcess.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <random>
#include <system_error>
#include <utility>

namespace neurus::ipc
{
namespace
{

using Json = nlohmann::json;

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
	const std::string overridePath = neurus::EnvironmentValue("NEURUS_SESSIONS_DIR");
	if (!overridePath.empty())
		return std::filesystem::path(overridePath);
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
	return neurus::IsProcessAlive(pid);
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
	// create_directories leaves levels it creates world-traversable; the
	// per-user root stores session tokens, so a root this call brings into
	// existence must be owner-only. A pre-existing one keeps its mode.
	const auto parent = m_directory.parent_path();
	const bool parentExisted = !parent.empty() && FileExists(parent);
	std::filesystem::create_directories(m_directory, code);
	if (code)
	{
		error = "Cannot create session directory: " + code.message();
		return false;
	}
	if (!parent.empty() && !parentExisted)
	{
		std::string parentError;
		if (!platform::RestrictToOwner(parent, true, parentError))
		{
			error = "Cannot secure the session parent directory: " + parentError;
			return false;
		}
	}
	std::string permissionError;
	if (!platform::RestrictToOwner(m_directory, true, permissionError))
	{
		error = "Cannot secure session directory: " + permissionError;
		return false;
	}
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
		if (platform::CreateExclusiveFile(path, std::to_string(neurus::CurrentProcessId())))
		{
			// The claim only guards a pending launch; a running session owns
			// the name through its entry, and launching over it would orphan it.
			const auto live = Read(name);
			if (live && live->pid > 0 && neurus::IsProcessAlive(live->pid))
			{
				ReleaseClaim(name);
				error = "A session named " + std::string(name) + " is already running.";
				return false;
			}
			return true;
		}

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
		if (owner > 0 && neurus::IsProcessAlive(owner))
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

	// The entry carries the IPC token, so it must be owner-only from the moment
	// it exists rather than chmod'ed after a plain ofstream created it 0644.
	std::string writeError;
	if (!platform::WriteOwnerOnlyFile(temporary, ToJson(record).dump(2), writeError))
	{
		std::error_code ignored;
		std::filesystem::remove(temporary, ignored);
		error = "Cannot write session entry: " + writeError;
		return false;
	}

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
