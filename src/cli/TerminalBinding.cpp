#include "cli/TerminalBinding.h"

#include "ipc/SessionRegistry.h"
#include "platform/PlatformFile.h"
#include "platform/PlatformProcess.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace neurus::cli
{
namespace
{

using Json = nlohmann::json;

std::string HashIdentity(const std::string& identity)
{
	// FNV-1a: stable across processes and builds, unlike std::hash.
	std::uint64_t hash = 1469598103934665603ull;
	for (const unsigned char byte : identity)
	{
		hash ^= byte;
		hash *= 1099511628211ull;
	}
	std::ostringstream out;
	out << std::hex << std::setw(16) << std::setfill('0') << hash;
	return out.str();
}

bool EnsureDirectory(const std::filesystem::path& directory, std::string& error)
{
	std::error_code code;
	// A directory this call creates is tightened before it is used; a
	// pre-existing one keeps the mode its owner chose.
	const auto parent = directory.parent_path();
	const bool parentExisted = !parent.empty() && std::filesystem::exists(parent, code);
	code.clear();
	std::filesystem::create_directories(directory, code);
	if (code)
	{
		error = "Cannot create the terminal binding directory: " + code.message();
		return false;
	}
	if (!parent.empty() && !parentExisted)
	{
		std::string parentError;
		if (!platform::RestrictToOwner(parent, true, parentError))
		{
			error = "Cannot secure the terminal binding parent directory: " + parentError;
			return false;
		}
	}
	std::string permissionError;
	if (!platform::RestrictToOwner(directory, true, permissionError))
	{
		error = "Cannot secure the terminal binding directory: " + permissionError;
		return false;
	}
	return true;
}

} // namespace

std::optional<std::string> EnvironmentSession()
{
	const std::string value = neurus::EnvironmentValue("NEURUS_SESSION");
	if (value.empty())
		return std::nullopt;
	return value;
}

std::string TerminalIdentity()
{
	return neurus::TerminalIdentity();
}

std::filesystem::path TerminalBindingDirectory()
{
	return ipc::ResolveSessionsDirectory() / "terminals";
}

std::filesystem::path TerminalBindingPath()
{
	const std::string identity = TerminalIdentity();
	if (identity.empty())
		return {};
	return TerminalBindingDirectory() / (HashIdentity(identity) + ".json");
}

std::optional<std::string> ReadTerminalBinding()
{
	const std::string identity = TerminalIdentity();
	const auto path = TerminalBindingPath();
	if (identity.empty() || path.empty())
		return std::nullopt;

	std::ifstream in(path, std::ios::binary);
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

	if (!json.is_object() || json.value("identity", std::string{}) != identity)
		return std::nullopt;
	const std::string session = json.value("session", std::string{});
	if (!ipc::IsValidSessionName(session))
		return std::nullopt;
	return session;
}

bool WriteTerminalBinding(const std::string& session, std::string& error)
{
	const std::string identity = TerminalIdentity();
	if (identity.empty())
	{
		error = "Cannot determine this terminal's identity.";
		return false;
	}
	if (!ipc::IsValidSessionName(session))
	{
		error = "Invalid session name: " + session;
		return false;
	}

	const auto directory = TerminalBindingDirectory();
	if (!EnsureDirectory(directory, error))
		return false;

	const Json json{{"v", 1}, {"identity", identity}, {"session", session}};
	const auto path = directory / (HashIdentity(identity) + ".json");
	const auto temporary = directory / (HashIdentity(identity) + ".tmp");

	// The binding names the session a command reaches, so it is owner-only from
	// creation instead of being chmod'ed after a readable window existed.
	std::string writeError;
	if (!platform::WriteOwnerOnlyFile(temporary, json.dump(2) + '\n', writeError))
	{
		std::error_code ignored;
		std::filesystem::remove(temporary, ignored);
		error = "Cannot write the terminal binding: " + writeError;
		return false;
	}

	std::error_code code;
	std::filesystem::remove(path, code);
	code.clear();
	std::filesystem::rename(temporary, path, code);
	if (code)
	{
		std::filesystem::remove(temporary, code);
		error = "Cannot publish the terminal binding: " + code.message();
		return false;
	}
	return true;
}

bool ClearTerminalBinding(std::string& error)
{
	const std::string identity = TerminalIdentity();
	const auto path = TerminalBindingPath();
	if (identity.empty() || path.empty())
	{
		error = "Cannot determine this terminal's identity.";
		return false;
	}

	std::error_code code;
	std::filesystem::remove(path, code);
	if (code)
	{
		error = "Cannot remove the terminal binding: " + code.message();
		return false;
	}
	return true;
}

void ClearTerminalBindingIfMatches(const std::string& session)
{
	const auto bound = ReadTerminalBinding();
	if (!bound || *bound != session)
		return;
	std::string error;
	(void)ClearTerminalBinding(error);
}

} // namespace neurus::cli
