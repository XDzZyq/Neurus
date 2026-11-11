/**
 * @file test_process_mac.cpp
 * @brief macOS-specific terminal identity and detached launch tests.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <cerrno>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "platform/PlatformProcess.h"

using namespace neurus;

namespace
{

/// Removes a variable around a test and restores the previous value.
class EnvironmentGuard
{
public:
	explicit EnvironmentGuard(const char* name)
		: m_name(name)
		, m_previous(EnvironmentValue(name))
	{
		SetEnvironmentValue(name, nullptr);
	}

	~EnvironmentGuard()
	{
		SetEnvironmentValue(m_name.c_str(),
		                    m_previous.empty() ? nullptr : m_previous.c_str());
	}

private:
	std::string m_name;
	std::string m_previous;
};

} // namespace

TEST(ProcessMac, ChildInTheSameSessionSharesTheIdentity)
{
	// A `sh -c` wrapper changes the parent pid but not the POSIX session, so the
	// identity must follow the session for a terminal binding to survive it.
	EnvironmentGuard terminalId("NEURUS_TERMINAL_ID");
	EnvironmentGuard termSession("TERM_SESSION_ID");
	EnvironmentGuard tmuxPane("TMUX_PANE");

	int fds[2] = {-1, -1};
	ASSERT_EQ(::pipe(fds), 0);

	const pid_t pid = ::fork();
	ASSERT_GE(pid, 0);

	if (pid == 0)
	{
		::close(fds[0]);
		const std::string identity = TerminalIdentity();
		(void)!::write(fds[1], identity.data(), identity.size());
		::_exit(0);
	}

	::close(fds[1]);
	std::string childIdentity;
	char buffer[512];
	for (;;)
	{
		const ssize_t bytes = ::read(fds[0], buffer, sizeof(buffer));
		if (bytes > 0)
		{
			childIdentity.append(buffer, static_cast<std::size_t>(bytes));
			continue;
		}
		if (bytes < 0 && errno == EINTR)
			continue;
		break;
	}
	::close(fds[0]);

	int status = 0;
	ASSERT_EQ(::waitpid(pid, &status, 0), pid);
	ASSERT_TRUE(WIFEXITED(status));

	const std::string identity = TerminalIdentity();
	EXPECT_EQ(identity.rfind("session:", 0), 0u) << identity;
	EXPECT_EQ(childIdentity, identity);
}

TEST(ProcessMac, DetachedLaunchDoesNotInheritTheCallersDescriptors)
{
	const auto root = std::filesystem::temp_directory_path()
		/ ("neurus-process-mac-" + std::to_string(CurrentProcessId()));
	std::error_code ignored;
	std::filesystem::remove_all(root, ignored);
	std::filesystem::create_directories(root);

	// A descriptor without O_CLOEXEC would follow a forking launcher into the app
	// and keep the CLI's files open, so the spawn must close it.
	const auto marker = root / "marker";
	const int created = ::open(marker.c_str(), O_CREAT | O_RDWR, 0600);
	ASSERT_GE(created, 0);
	::close(created);
	const int opened = ::open(marker.c_str(), O_RDWR);
	ASSERT_GE(opened, 0);
	const int leaked = ::fcntl(opened, F_DUPFD, 37);
	::close(opened);
	ASSERT_GE(leaked, 37) << "cannot place the probe descriptor";

	const auto logPath = root / "launch.log";
	std::string error;
	const auto pid = SpawnDetached("/bin/sh", {"-c", "ls /dev/fd"}, logPath, error);
	ASSERT_TRUE(pid.has_value()) << error;

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (IsProcessAlive(*pid) && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	EXPECT_FALSE(IsProcessAlive(*pid));

	std::ifstream log(logPath);
	std::vector<std::string> lines;
	for (std::string line; std::getline(log, line);)
	{
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		lines.push_back(std::move(line));
	}
	::close(leaked);

	ASSERT_FALSE(lines.empty()) << "the launch log is empty";
	// The redirect must have delivered the descriptor listing itself.
	EXPECT_TRUE(std::find(lines.begin(), lines.end(), "1") != lines.end()) << "no listing";
	std::string listing;
	for (const auto& line : lines)
	{
		listing += line;
		listing.push_back(' ');
	}
	// `ls` writes one descriptor per line when its output is a file.
	const std::string probe = std::to_string(leaked);
	EXPECT_TRUE(std::find(lines.begin(), lines.end(), probe) == lines.end())
		<< "leaked descriptor " << probe << " in: " << listing;

	std::filesystem::remove_all(root, ignored);
}
