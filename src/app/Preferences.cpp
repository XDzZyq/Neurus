#include "app/Preferences.h"

#include "core/Log.h"
#include "core/Serialize.h"
#include "platform/PlatformFile.h"
#include "platform/PlatformPaths.h"

#include <cereal/archives/json.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace neurus {

namespace {

/**
 * @brief Nested JSON shape for the script settings.
 *
 * The persisted file is tree-like (`script.preload`, `script.sandbox.*`), while
 * the live Preferences struct keeps flat fields for its callers.
 */
struct ScriptSandboxJson
{
	std::vector<std::string> readRoots;
	std::vector<std::string> writeRoots;
	std::vector<std::string> blockedModules;
	bool allowNetwork = false;
	bool allowSubprocess = false;
	bool allowCtypes = false;
	int timeLimitSeconds = 30;
	int memoryLimitMb = 0;

	template<class Archive>
	void save(Archive& ar) const
	{
		ar(cereal::make_nvp("read_roots", readRoots),
		   cereal::make_nvp("write_roots", writeRoots),
		   cereal::make_nvp("blocked_modules", blockedModules),
		   cereal::make_nvp("allow_network", allowNetwork),
		   cereal::make_nvp("allow_subprocess", allowSubprocess),
		   cereal::make_nvp("allow_ctypes", allowCtypes),
		   cereal::make_nvp("time_limit_s", timeLimitSeconds),
		   cereal::make_nvp("memory_limit_mb", memoryLimitMb));
	}

	template<class Archive>
	void load(Archive& ar)
	{
		archive::Optional(ar, cereal::make_nvp("read_roots", readRoots),
		                  cereal::make_nvp("write_roots", writeRoots),
		                  cereal::make_nvp("blocked_modules", blockedModules),
		                  cereal::make_nvp("allow_network", allowNetwork),
		                  cereal::make_nvp("allow_subprocess", allowSubprocess),
		                  cereal::make_nvp("allow_ctypes", allowCtypes),
		                  cereal::make_nvp("time_limit_s", timeLimitSeconds),
		                  cereal::make_nvp("memory_limit_mb", memoryLimitMb));
	}
};

struct ScriptJson
{
	std::vector<std::string> preload;
	ScriptSandboxJson sandbox;

	template<class Archive>
	void save(Archive& ar) const
	{
		ar(cereal::make_nvp("preload", preload),
		   cereal::make_nvp("sandbox", sandbox));
	}

	template<class Archive>
	void load(Archive& ar)
	{
		archive::Optional(ar, cereal::make_nvp("preload", preload));
		archive::OptionalField(ar, cereal::make_nvp("sandbox", sandbox));
	}
};

} // namespace

std::string Preferences::DefaultPath()
{
	return (HomeDirectory() / ".neurus" / "preferences.json").generic_string();
}

Preferences::LoadResult Preferences::Load(const std::string& path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in.is_open())
		return LoadResult::Missing;  // First run: defaults, the file is not created.

	// Parse into a default-constructed Preferences and adopt it only once the
	// parse succeeds: a truncated or schema-drifted file cannot half-overwrite
	// the live values, and a missing key resolves to the in-class default.
	Preferences fresh;

	try
	{
		cereal::JSONInputArchive ar(in);
		ScriptJson script;
		archive::Optional(ar, cereal::make_nvp("script", script));

		// Each key independently: a file missing one of them still yields the
		// other instead of Corrupt.
		archive::Optional(ar, cereal::make_nvp("language", fresh.language),
		                  cereal::make_nvp("target_fps", fresh.targetFps));

		fresh.scriptPreload = std::move(script.preload);
		fresh.scriptSandboxReadRoots = std::move(script.sandbox.readRoots);
		fresh.scriptSandboxWriteRoots = std::move(script.sandbox.writeRoots);
		fresh.scriptSandboxBlockedModules = std::move(script.sandbox.blockedModules);
		fresh.scriptSandboxAllowNetwork = script.sandbox.allowNetwork;
		fresh.scriptSandboxAllowSubprocess = script.sandbox.allowSubprocess;
		fresh.scriptSandboxAllowCtypes = script.sandbox.allowCtypes;
		fresh.scriptSandboxTimeLimitSeconds = script.sandbox.timeLimitSeconds;
		fresh.scriptSandboxMemoryLimitMb = script.sandbox.memoryLimitMb;
	}
	catch (const std::exception& e)
	{
		NEURUS_ERR("[Preferences] failed to load " << path << ": " << e.what());
		return LoadResult::Corrupt;  // Fields untouched: nothing is destroyed.
	}

	language  = fresh.language;
	targetFps = fresh.targetFps;
	scriptPreload = std::move(fresh.scriptPreload);
	scriptSandboxReadRoots = std::move(fresh.scriptSandboxReadRoots);
	scriptSandboxWriteRoots = std::move(fresh.scriptSandboxWriteRoots);
	scriptSandboxBlockedModules = std::move(fresh.scriptSandboxBlockedModules);
	scriptSandboxAllowNetwork = fresh.scriptSandboxAllowNetwork;
	scriptSandboxAllowSubprocess = fresh.scriptSandboxAllowSubprocess;
	scriptSandboxAllowCtypes = fresh.scriptSandboxAllowCtypes;
	scriptSandboxTimeLimitSeconds = fresh.scriptSandboxTimeLimitSeconds;
	scriptSandboxMemoryLimitMb = fresh.scriptSandboxMemoryLimitMb;

	// "auto" is deliberately left as-is: resolving the sentinel is not this
	// type's job, so the stored value survives the round-trip.
	return LoadResult::Ok;
}

#ifdef NEURUS_ENABLE_SCRIPTING
ScriptPolicy Preferences::MakeScriptPolicy(const std::string& projectRoot) const
{
	ScriptPolicy policy = ScriptPolicy::Sandboxed(projectRoot);
	if (!scriptSandboxReadRoots.empty())
	{
		policy.readRoots = scriptSandboxReadRoots;
	}
	if (!scriptSandboxWriteRoots.empty())
	{
		policy.writeRoots = scriptSandboxWriteRoots;
	}
	policy.blockedModules = scriptSandboxBlockedModules;
	policy.allowNetwork = scriptSandboxAllowNetwork;
	policy.allowSubprocess = scriptSandboxAllowSubprocess;
	policy.allowCtypes = scriptSandboxAllowCtypes;
	policy.wallClockLimit =
		std::chrono::seconds(scriptSandboxTimeLimitSeconds > 0
		                         ? scriptSandboxTimeLimitSeconds
		                         : 0);
	policy.memoryLimitBytes =
		static_cast<std::uint64_t>(scriptSandboxMemoryLimitMb > 0
		                               ? scriptSandboxMemoryLimitMb
		                               : 0)
		* 1024ULL * 1024ULL;
	return policy;
}
#endif

bool Preferences::Backup(const std::string& path)
{
	const std::filesystem::path from(path);
	std::filesystem::path       to = from;
	to += ".bak";

	std::error_code ec;
	// rename() over an existing file is the platform's atomic replace; remove
	// first anyway because Windows' rename fails when the target exists.
	std::filesystem::remove(to, ec);
	ec.clear();
	std::filesystem::rename(from, to, ec);
	if (ec)
	{
		NEURUS_ERR("[Preferences] failed to move " << path << " aside: "
		           << ec.message());
		return false;
	}

	NEURUS_LOG("[Preferences] kept the unreadable file as " << to.generic_string());
	return true;
}

bool Preferences::Save(const std::string& path) const
{
	const std::filesystem::path parent =
		std::filesystem::path(path).parent_path();
	if (!parent.empty())
	{
		std::error_code ec;
		const bool parentExisted = std::filesystem::exists(parent, ec);
		ec.clear();
		std::filesystem::create_directories(parent, ec);
		if (ec)
		{
			NEURUS_ERR("[Preferences] failed to create parent directory for "
			           << path << ": " << ec.message());
			return false;
		}
		// create_directories creates the per-user root 0755; it also holds the
		// session registry (tokens), so a root this call creates is owner-only.
		// A pre-existing root keeps its mode - this is not the place to change it.
		if (!parentExisted)
		{
			std::string permissionError;
			if (!platform::RestrictToOwner(parent, true, permissionError))
			{
				NEURUS_ERR("[Preferences] failed to secure "
				           << parent.generic_string() << ": " << permissionError);
				return false;
			}
		}
	}

	std::ofstream out(path, std::ios::binary);
	if (!out.is_open())
	{
		NEURUS_ERR("[Preferences] failed to create " << path);
		return false;
	}

	try
	{
		ScriptJson script;
		script.preload = scriptPreload;
		script.sandbox.readRoots = scriptSandboxReadRoots;
		script.sandbox.writeRoots = scriptSandboxWriteRoots;
		script.sandbox.blockedModules = scriptSandboxBlockedModules;
		script.sandbox.allowNetwork = scriptSandboxAllowNetwork;
		script.sandbox.allowSubprocess = scriptSandboxAllowSubprocess;
		script.sandbox.allowCtypes = scriptSandboxAllowCtypes;
		script.sandbox.timeLimitSeconds = scriptSandboxTimeLimitSeconds;
		script.sandbox.memoryLimitMb = scriptSandboxMemoryLimitMb;

		cereal::JSONOutputArchive ar(out);
		ar(cereal::make_nvp("language", language),
		   cereal::make_nvp("target_fps", targetFps),
		   cereal::make_nvp("script", script));
	}
	catch (const std::exception& e)
	{
		NEURUS_ERR("[Preferences] failed to save " << path << ": " << e.what());
		return false;
	}

	NEURUS_LOG("[Preferences] saved " << path << " (language=" << language
	           << ", target_fps=" << targetFps << ")");
	return true;
}

} // namespace neurus
