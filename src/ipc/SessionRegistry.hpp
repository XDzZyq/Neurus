#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neurus::ipc
{

/// One live command session as recorded under `~/.neurus/sessions`.
struct SessionRecord
{
	int          formatVersion = 1;
	std::string  session;
	int          pid = 0;
	std::uint16_t port = 0;
	std::string  token;
	std::string  project;
	std::string  appVersion;
	int          protocolVersion = 1;
	std::string  startedAt;
};

/**
 * @brief Returns the registry directory: NEURUS_SESSIONS_DIR when set, else
 *        the per-user sessions directory.
 */
std::filesystem::path ResolveSessionsDirectory();

/**
 * @brief Validates a session name.
 * @param name Candidate name.
 * @return True for `[A-Za-z0-9][A-Za-z0-9_-]{0,63}`.
 *
 * The grammar also protects registry filenames from path traversal.
 */
bool IsValidSessionName(std::string_view name);

/**
 * @brief Reports whether a process is still running.
 * @param pid Operating-system process id.
 */
bool IsProcessAlive(int pid);

/**
 * @brief Owns registry files for named sessions under one directory.
 *
 * All paths are built from a grammar-validated name, so a crafted name cannot
 * escape the directory.
 */
class SessionRegistry
{
public:
	/**
	 * @param directory Directory holding `<name>.json`, `<name>.claim` and logs.
	 */
	explicit SessionRegistry(std::filesystem::path directory);

	/// @return The directory this registry manages.
	const std::filesystem::path& Directory() const;

	/**
	 * @brief Claims a session name for a pending launch.
	 * @param name Grammar-validated session name.
	 * @param error Receives a reason when the claim fails.
	 * @return True when this caller now owns the claim.
	 */
	bool TryClaim(std::string_view name, std::string& error);

	/// Releases a claim created by TryClaim(), if present.
	void ReleaseClaim(std::string_view name);

	/**
	 * @brief Writes a session entry atomically.
	 * @param record Entry to persist.
	 * @param error Receives a reason on failure.
	 */
	bool Write(const SessionRecord& record, std::string& error);

	/**
	 * @brief Reads a session entry by name.
	 * @return The entry, or std::nullopt when absent or unreadable.
	 */
	std::optional<SessionRecord> Read(std::string_view name) const;

	/// Deletes an entry. @return True when a file was removed.
	bool Remove(std::string_view name) const;

	/**
	 * @brief Lists entries whose pid is still alive.
	 *
	 * Dead entries are deleted as they are discovered.
	 */
	std::vector<SessionRecord> ListLive() const;

	/// Lists every readable entry, live or stale.
	std::vector<SessionRecord> ReadAll() const;

private:
	std::filesystem::path PathFor(std::string_view name, std::string_view extension) const;
	bool EnsureDirectory(std::string& error) const;

	std::filesystem::path m_directory;
};

/**
 * @brief Allocates an unused session name derived from a project path.
 * @param projectPath Project file path, or empty for a generic name.
 * @param registry Registry used to avoid collisions.
 * @return A valid, unused name, or an empty string when no name could be found.
 */
std::string AllocateSessionName(const std::string& projectPath, const SessionRegistry& registry);

} // namespace neurus::ipc
