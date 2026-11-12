/**
 * @file test_script_sandbox.cpp
 * @brief In-process guardrail: roots, imports, network, traversal and timeout.
 *
 * Every run here goes through the production `ScriptEngine` API, so the tests
 * cover the policy plumbing, the audit hook and the watchdog together.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

#include "script/ScriptEngine.h"

namespace
{

/// Two scratch directories: one the policy allows, one it refuses.
class SandboxTestPaths
{
public:
	SandboxTestPaths()
	{
		const std::filesystem::path base = std::filesystem::temp_directory_path();
		// A per-instance suffix keeps two test processes from sharing scratch
		// directories when CTest runs the suite in parallel.
		const std::string suffix = std::to_string(
			static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
		m_project = base / ("neurus_sandbox_project_" + suffix);
		m_outside = base / ("neurus_sandbox_outside_" + suffix);
		std::error_code error;
		std::filesystem::remove_all(m_project, error);
		std::filesystem::remove_all(m_outside, error);
		std::filesystem::create_directories(m_project);
		std::filesystem::create_directories(m_outside);
	}

	~SandboxTestPaths()
	{
		std::error_code error;
		std::filesystem::remove_all(m_project, error);
		std::filesystem::remove_all(m_outside, error);
	}

	SandboxTestPaths(const SandboxTestPaths&) = delete;
	SandboxTestPaths& operator=(const SandboxTestPaths&) = delete;

	const std::filesystem::path& Project() const
	{
		return m_project;
	}

	const std::filesystem::path& Outside() const
	{
		return m_outside;
	}

private:
	std::filesystem::path m_project;
	std::filesystem::path m_outside;
};

std::string Generic(const std::filesystem::path& path)
{
	return path.generic_string();
}

}  // namespace

TEST(ScriptSandbox, TrustedWriteOutsideRootsSucceeds)
{
	SandboxTestPaths paths;
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());

	const neurus::ScriptPolicy trusted = neurus::ScriptPolicy::Trusted();
	const std::filesystem::path target = paths.Outside() / "trusted.txt";
	const std::string code =
		"open(r'" + Generic(target) + "', 'w').write('trusted')";

	EXPECT_TRUE(engine.RunStringChecked(code, nullptr, &trusted));
	EXPECT_EQ(engine.LastRunStatus(), neurus::ScriptRunStatus::Ok);
	EXPECT_TRUE(std::filesystem::exists(target));

	engine.Shutdown();
}

TEST(ScriptSandbox, SandboxedWriteOutsideRootsIsRefused)
{
	SandboxTestPaths paths;
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());

	const neurus::ScriptPolicy policy =
		neurus::ScriptPolicy::Sandboxed(Generic(paths.Project()));
	const std::filesystem::path target = paths.Outside() / "denied.txt";
	const std::string code =
		"open(r'" + Generic(target) + "', 'w').write('nope')";

	EXPECT_FALSE(engine.RunStringChecked(code, nullptr, &policy));
	EXPECT_EQ(engine.LastRunStatus(), neurus::ScriptRunStatus::SandboxViolation);
	EXPECT_NE(engine.LastRunMessage().find("write_roots"), std::string::npos);
	EXPECT_FALSE(std::filesystem::exists(target));

	engine.Shutdown();
}

TEST(ScriptSandbox, SandboxedReadAndWriteInsideRootsWorks)
{
	SandboxTestPaths paths;
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());

	const neurus::ScriptPolicy policy =
		neurus::ScriptPolicy::Sandboxed(Generic(paths.Project()));
	const std::filesystem::path target = paths.Project() / "inside.txt";
	const std::string code =
		"path = r'" + Generic(target) + "'\n"
		"with open(path, 'w') as handle:\n"
		"    handle.write('sandboxed')\n"
		"with open(path) as handle:\n"
		"    assert handle.read() == 'sandboxed'\n";

	EXPECT_TRUE(engine.RunStringChecked(code, nullptr, &policy));
	EXPECT_EQ(engine.LastRunStatus(), neurus::ScriptRunStatus::Ok);
	EXPECT_TRUE(std::filesystem::exists(target));
	EXPECT_EQ(engine.LastPolicy().mode, neurus::ScriptMode::Sandboxed);

	engine.Shutdown();
}

TEST(ScriptSandbox, BlockedImportsAreRefused)
{
	SandboxTestPaths paths;
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());

	neurus::ScriptPolicy policy =
		neurus::ScriptPolicy::Sandboxed(Generic(paths.Project()));
	policy.blockedModules = { "subprocess", "ctypes" };

	EXPECT_FALSE(engine.RunStringChecked("import subprocess", nullptr, &policy));
	EXPECT_EQ(engine.LastRunStatus(), neurus::ScriptRunStatus::SandboxViolation);
	EXPECT_NE(engine.LastRunMessage().find("blocked_modules"), std::string::npos);

	EXPECT_FALSE(engine.RunStringChecked("import ctypes", nullptr, &policy));
	EXPECT_EQ(engine.LastRunStatus(), neurus::ScriptRunStatus::SandboxViolation);

	// The expression path classifies through the same policy code.
	EXPECT_FALSE(engine.EvalStringChecked("__import__('subprocess')", nullptr, &policy)
	                 .has_value());
	EXPECT_EQ(engine.LastRunStatus(), neurus::ScriptRunStatus::SandboxViolation);

	engine.Shutdown();
}

TEST(ScriptSandbox, CachedBlockedModuleIsStillRefused)
{
	SandboxTestPaths paths;
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());

	// Warm the module in a trusted run so the refusal cannot come from a file
	// read: the import gate must check sys.modules' cache too.
	ASSERT_TRUE(engine.RunStringChecked("import subprocess"));

	neurus::ScriptPolicy policy =
		neurus::ScriptPolicy::Sandboxed(Generic(paths.Project()));
	policy.blockedModules = { "subprocess" };

	EXPECT_FALSE(engine.RunStringChecked("import subprocess", nullptr, &policy));
	EXPECT_EQ(engine.LastRunStatus(), neurus::ScriptRunStatus::SandboxViolation);

	engine.Shutdown();
}

TEST(ScriptSandbox, SocketImportWorksButConnectIsRefused)
{
	SandboxTestPaths paths;
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());

	const neurus::ScriptPolicy policy =
		neurus::ScriptPolicy::Sandboxed(Generic(paths.Project()));

	// Importing the module is not network access: it stays available so a
	// script can be written once and granted sockets later.
	EXPECT_TRUE(engine.RunStringChecked("import socket\nassert socket is not None", nullptr,
	                                    &policy));
	EXPECT_EQ(engine.LastRunStatus(), neurus::ScriptRunStatus::Ok);

	EXPECT_FALSE(engine.RunStringChecked(
		"import socket\nsocket.getaddrinfo('127.0.0.1', 80)", nullptr, &policy));
	EXPECT_EQ(engine.LastRunStatus(), neurus::ScriptRunStatus::SandboxViolation);
	EXPECT_NE(engine.LastRunMessage().find("allow_network"), std::string::npos);

	engine.Shutdown();
}

TEST(ScriptSandbox, ParentTraversalIsRefused)
{
	SandboxTestPaths paths;
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());

	const neurus::ScriptPolicy policy =
		neurus::ScriptPolicy::Sandboxed(Generic(paths.Project()));
	const std::string escapeName = paths.Project().filename().string() + "_escape.txt";
	const std::filesystem::path escape = paths.Project().parent_path() / escapeName;
	std::error_code cleanupError;
	std::filesystem::remove(escape, cleanupError);
	// The raw `..` form is what a script would try; the hook resolves it before
	// the root check, so the parent directory is still refused.
	const std::string traversal = Generic(paths.Project()) + "/../" + escapeName;
	const std::string code = "open(r'" + traversal + "', 'w').write('escape')";

	EXPECT_FALSE(engine.RunStringChecked(code, nullptr, &policy));
	EXPECT_EQ(engine.LastRunStatus(), neurus::ScriptRunStatus::SandboxViolation);
	EXPECT_FALSE(std::filesystem::exists(escape));

	engine.Shutdown();
}

TEST(ScriptSandbox, WallClockLimitStopsAnInfiniteLoop)
{
	SandboxTestPaths paths;
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());

	neurus::ScriptPolicy policy =
		neurus::ScriptPolicy::Sandboxed(Generic(paths.Project()));
	policy.wallClockLimit = std::chrono::milliseconds(200);

	const auto start = std::chrono::steady_clock::now();
	EXPECT_FALSE(engine.RunStringChecked("while True:\n    pass", nullptr, &policy));
	const auto elapsed = std::chrono::steady_clock::now() - start;

	EXPECT_EQ(engine.LastRunStatus(), neurus::ScriptRunStatus::Timeout);
	EXPECT_LT(elapsed, std::chrono::seconds(10));

	// The engine survives, and no pending timeout leaks into the next run.
	EXPECT_TRUE(engine.RunStringChecked("value = 1 + 1\nassert value == 2"));
	EXPECT_EQ(engine.LastRunStatus(), neurus::ScriptRunStatus::Ok);

	engine.Shutdown();
}
