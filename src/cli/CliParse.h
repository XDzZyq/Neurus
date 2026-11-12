#pragma once

#include "command/CommandError.h"
#include "command/CommandTypes.h"
#include "ipc/SessionRegistry.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace neurus::cli
{

enum class OutputFormat
{
	Human,
	Json
};

/// Process exit codes shared by every command.
enum class CliExit
{
	Ok = 0,
	Usage = 2,
	NoSession = 3,
	Transport = 4,
	Rejected = 5,
	AppMissing = 6
};

struct OpenOptions
{
	std::optional<std::string>      project;
	std::optional<std::string>      name;
	std::optional<std::filesystem::path> app;
	int                             timeoutSeconds = 30;
	bool                            noWait = false;
	bool                            noBind = false;
	OutputFormat                    format = OutputFormat::Human;
};

struct CloseOptions
{
	std::optional<std::string> name;
	bool                       all = false;
	bool                       save = false;
	bool                       force = false;
	int                        timeoutSeconds = 10;
	OutputFormat               format = OutputFormat::Human;
};

struct StatusOptions
{
	std::optional<std::string> name;
	int                        timeoutSeconds = 10;
	OutputFormat               format = OutputFormat::Human;
};

struct ListOptions
{
	OutputFormat format = OutputFormat::Human;
};

bool ValidateOpenOptions(const OpenOptions& options, std::string& error);
bool ValidateCloseOptions(const CloseOptions& options, std::string& error);
bool ValidateStatusOptions(const StatusOptions& options, std::string& error);

/// Maps a command error to the process exit code.
CliExit ExitCodeFor(command::CommandErrorCode code);

/// Result of resolving a name (or the absence of one) against live sessions.
enum class TargetResolution
{
	Ok,
	NotFound,
	Ambiguous,
	NoSessions
};

/**
 * @brief Resolves which sessions a close/status command should target.
 * @param name Explicit name, or std::nullopt.
 * @param all True for `--all`.
 * @param live Live sessions from the registry.
 * @param targets Receives the selected sessions.
 * @param ambiguousNames Receives names when the result is Ambiguous.
 */
TargetResolution ResolveTargets(const std::optional<std::string>& name,
                                bool all,
                                const std::vector<ipc::SessionRecord>& live,
                                std::vector<ipc::SessionRecord>& targets,
                                std::vector<std::string>& ambiguousNames);

} // namespace neurus::cli
