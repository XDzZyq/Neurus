#include "app/Preferences.h"

#include "core/Log.h"
#include "core/Serialize.h"
#include "platform/PlatformPaths.h"

#include <cereal/archives/json.hpp>

#include <filesystem>
#include <fstream>
#include <system_error>

namespace neurus {

std::string Preferences::DefaultPath()
{
	return (HomeDirectory() / ".neurus" / "preferences.json").generic_string();
}

Preferences::LoadResult Preferences::Load(const std::string& path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in.is_open())
		return LoadResult::Missing;  // First run: keep defaults, caller saves.

	// Parse into a default-constructed Preferences and adopt it only once the
	// parse succeeds: a truncated or schema-drifted file cannot half-overwrite
	// the live values, and a missing key resolves to the in-class default.
	Preferences fresh;

	try
	{
		cereal::JSONInputArchive ar(in);
		// Each key independently: a file missing one of them still yields the
		// other instead of Corrupt.
		archive::Optional(ar, cereal::make_nvp("language", fresh.language),
		                  cereal::make_nvp("target_fps", fresh.targetFps));
	}
	catch (const std::exception& e)
	{
		NEURUS_ERR("[Preferences] failed to load " << path << ": " << e.what());
		return LoadResult::Corrupt;  // Fields untouched: nothing is destroyed.
	}

	language  = fresh.language;
	targetFps = fresh.targetFps;

	// "auto" is deliberately left as-is: resolving the sentinel is not this
	// type's job, so the stored value survives the round-trip.
	return LoadResult::Ok;
}

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
		std::filesystem::create_directories(parent, ec);
		if (ec)
		{
			NEURUS_ERR("[Preferences] failed to create parent directory for "
			           << path << ": " << ec.message());
			return false;
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
		cereal::JSONOutputArchive ar(out);
		ar(cereal::make_nvp("language", language),
		   cereal::make_nvp("target_fps", targetFps));
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
