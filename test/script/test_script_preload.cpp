/**
 * @file test_script_preload.cpp
 * @brief Startup preload, module reporting and hot reload of project modules.
 *
 * The tests drive only the public engine API. Hot reload is exercised with a
 * temporary module whose top-level code appends to a counter file, so a second
 * run of an unchanged module is proven not to re-execute it; mtimes are bumped
 * with std::filesystem so the test does not depend on clock granularity.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "script/ScriptConsole.h"
#include "script/ScriptEngine.h"

namespace fs = std::filesystem;

namespace
{

/**
 * @brief RAII temporary directory removed recursively on destruction.
 */
class TempDir
{
public:
	TempDir()
		: m_path(fs::temp_directory_path() / ("neurus_script_preload_" + NextId()))
	{
		std::error_code ec;
		fs::create_directories(m_path, ec);
	}

	~TempDir()
	{
		std::error_code ec;
		fs::remove_all(m_path, ec);
	}

	TempDir(const TempDir&) = delete;
	TempDir& operator=(const TempDir&) = delete;

	const fs::path& path() const
	{
		return m_path;
	}

private:
	static std::string NextId()
	{
		const auto tick = static_cast<unsigned long long>(
			std::chrono::steady_clock::now().time_since_epoch().count());
		static unsigned long long counter = 0;
		return std::to_string(tick) + "_" + std::to_string(++counter);
	}

	fs::path m_path;
};

void WriteText(const fs::path& path, const std::string& text)
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(out.is_open()) << path.string();
	out << text;
}

/// @brief Escapes a path for use inside a single-quoted Python literal.
std::string PythonLiteral(const std::string& value)
{
	std::string escaped;
	escaped.reserve(value.size());
	for (const char character : value)
	{
		if (character == '\\' || character == '\'')
		{
			escaped.push_back('\\');
		}
		escaped.push_back(character);
	}
	return escaped;
}

/// @brief Module source that counts its own top-level executions.
std::string ModuleSource(const fs::path& counter, int value)
{
	return "with open(r'" + PythonLiteral(counter.generic_string())
	       + "', 'a', encoding='utf-8') as f:\n"
	         "    f.write('run\\n')\n"
	         "VALUE = " + std::to_string(value) + "\n";
}

std::size_t CountLines(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	std::size_t lines = 0;
	std::string line;
	while (std::getline(in, line))
	{
		++lines;
	}
	return lines;
}

/// @brief Moves the file's mtime far enough ahead to be unambiguous.
bool BumpMtime(const fs::path& path)
{
	std::error_code ec;
	const fs::file_time_type current = fs::last_write_time(path, ec);
	if (ec)
	{
		return false;
	}
	fs::last_write_time(path, current + std::chrono::seconds(10), ec);
	return !ec;
}

bool ConsoleContains(neurus::ScriptEngine& engine, neurus::ScriptLineKind kind,
                     const std::string& needle)
{
	const neurus::ScriptConsoleBuffer& console = engine.Console();
	for (std::size_t i = 0; i < console.Size(); ++i)
	{
		const neurus::ScriptLine line = console.At(i);
		if (line.kind == kind && line.text.find(needle) != std::string::npos)
		{
			return true;
		}
	}
	return false;
}

} // namespace

TEST(ScriptPreload, PreloadsAStdlibPackageAndReportsCache)
{
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());

	const std::vector<neurus::ScriptPreloadResult> first = engine.Preload({"json"});
	ASSERT_EQ(first.size(), 1u);
	EXPECT_EQ(first[0].package, "json");
	EXPECT_TRUE(first[0].ok) << first[0].error;
	EXPECT_GE(first[0].loadSeconds, 0.0);

	const std::vector<neurus::ScriptPreloadResult> second = engine.Preload({"json"});
	ASSERT_EQ(second.size(), 1u);
	EXPECT_TRUE(second[0].ok) << second[0].error;
	EXPECT_TRUE(second[0].cached);
	EXPECT_GE(second[0].loadSeconds, 0.0);

	engine.Shutdown();
}

TEST(ScriptPreload, MissingPackageIsReportedAndTheEngineStillRuns)
{
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());

	const std::vector<neurus::ScriptPreloadResult> results =
		engine.Preload({"neurus_missing_package_for_preload_test"});
	ASSERT_EQ(results.size(), 1u);
	EXPECT_FALSE(results[0].ok);
	EXPECT_FALSE(results[0].error.empty());
	EXPECT_NE(results[0].error.find("neurus_missing_package_for_preload_test"),
	          std::string::npos);

	// A failed preload must not poison the interpreter.
	EXPECT_TRUE(engine.RunStringChecked("preload_survivor = 40 + 2"));
	EXPECT_EQ(engine.EvalStringChecked("preload_survivor").value_or(""), "42");

	engine.Shutdown();
}

TEST(ScriptPreload, LoadedModulesReportsThePreloadedPackage)
{
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());
	ASSERT_TRUE(engine.Preload({"json"})[0].ok);

	const std::vector<neurus::ScriptModuleInfo> modules = engine.LoadedModules();
	ASSERT_FALSE(modules.empty());
	ASSERT_LE(modules.size(), 500u);

	bool found = false;
	for (const neurus::ScriptModuleInfo& module : modules)
	{
		if (module.name == "json")
		{
			found = true;
			EXPECT_FALSE(module.origin.empty());
			EXPECT_GE(module.loadSeconds, 0.0);
		}
	}
	EXPECT_TRUE(found) << "loaded modules did not include the preloaded package";

	engine.Shutdown();
}

TEST(ScriptHotReload, ReloadsAChangedProjectModuleOnce)
{
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());

	TempDir dir;
	const std::string moduleName = "neurus_hot_reload_module";
	const fs::path modulePath = dir.path() / (moduleName + ".py");
	const fs::path counterPath = dir.path() / "counter.txt";
	WriteText(modulePath, ModuleSource(counterPath, 1));

	const std::string directory = PythonLiteral(dir.path().generic_string());
	ASSERT_TRUE(engine.RunStringChecked("import sys\nsys.path.insert(0, r'" + directory + "')"));
	engine.SetReloadRoots({dir.path().generic_string()});
	ASSERT_EQ(engine.ReloadRoots().size(), 1u);

	// First run executes the top-level code.
	ASSERT_TRUE(engine.RunStringChecked("import " + moduleName));
	EXPECT_EQ(CountLines(counterPath), 1u);
	EXPECT_EQ(engine.EvalStringChecked(moduleName + ".VALUE").value_or(""), "1");

	// Second run of the unchanged module: neither reload nor re-execution.
	ASSERT_TRUE(engine.RunStringChecked("import " + moduleName));
	EXPECT_EQ(CountLines(counterPath), 1u);
	EXPECT_EQ(engine.EvalStringChecked(moduleName + ".VALUE").value_or(""), "1");

	// Rewrite, move mtime ahead, and run again: the module is reloaded once.
	WriteText(modulePath, ModuleSource(counterPath, 2));
	ASSERT_TRUE(BumpMtime(modulePath));
	engine.Console().Clear();
	ASSERT_TRUE(engine.RunStringChecked("import " + moduleName));
	EXPECT_EQ(CountLines(counterPath), 2u);
	EXPECT_EQ(engine.EvalStringChecked(moduleName + ".VALUE").value_or(""), "2");
	EXPECT_TRUE(ConsoleContains(engine, neurus::ScriptLineKind::Output,
	                            "reloaded module " + moduleName));

	engine.Shutdown();
}

TEST(ScriptHotReload, DoesNotReloadAModuleOutsideTheRoots)
{
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());

	TempDir projectDir;
	TempDir otherDir;
	const std::string moduleName = "neurus_outside_root_module";
	const fs::path modulePath = otherDir.path() / (moduleName + ".py");
	const fs::path counterPath = otherDir.path() / "counter.txt";
	WriteText(modulePath, ModuleSource(counterPath, 1));

	ASSERT_TRUE(engine.RunStringChecked(
		"import sys\nsys.path.insert(0, r'" + PythonLiteral(otherDir.path().generic_string()) + "')"));
	// The module's directory is importable but is not a reload root.
	engine.SetReloadRoots({projectDir.path().generic_string()});

	ASSERT_TRUE(engine.RunStringChecked("import " + moduleName));
	EXPECT_EQ(CountLines(counterPath), 1u);

	WriteText(modulePath, ModuleSource(counterPath, 2));
	ASSERT_TRUE(BumpMtime(modulePath));
	ASSERT_TRUE(engine.RunStringChecked("import " + moduleName));

	EXPECT_EQ(engine.EvalStringChecked(moduleName + ".VALUE").value_or(""), "1");
	EXPECT_EQ(CountLines(counterPath), 1u)
		<< "a module outside the reload roots must not be re-executed";

	engine.Shutdown();
}

TEST(ScriptHotReload, AFileRunTreatsItsOwnDirectoryAsAReloadRoot)
{
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());

	TempDir dir;
	const std::string moduleName = "neurus_file_root_module";
	const fs::path modulePath = dir.path() / (moduleName + ".py");
	const fs::path counterPath = dir.path() / "counter.txt";
	const fs::path scriptPath = dir.path() / "run_script.py";
	WriteText(modulePath, ModuleSource(counterPath, 1));
	WriteText(scriptPath, "import " + moduleName + "\n");

	ASSERT_TRUE(engine.RunStringChecked(
		"import sys\nsys.path.insert(0, r'" + PythonLiteral(dir.path().generic_string()) + "')"));
	// No configured roots: the file run's own directory must be enough.
	engine.SetReloadRoots({});

	ASSERT_TRUE(engine.RunFileChecked(scriptPath.string()));
	EXPECT_EQ(CountLines(counterPath), 1u);

	WriteText(modulePath, ModuleSource(counterPath, 2));
	ASSERT_TRUE(BumpMtime(modulePath));
	ASSERT_TRUE(engine.RunFileChecked(scriptPath.string()));

	EXPECT_EQ(CountLines(counterPath), 2u);
	EXPECT_EQ(engine.EvalStringChecked(moduleName + ".VALUE").value_or(""), "2");

	engine.Shutdown();
}

TEST(ScriptHotReload, ReportsAFailedReloadAndSkipsTheRun)
{
	neurus::ScriptEngine engine;
	ASSERT_TRUE(engine.Initialize());

	TempDir dir;
	const std::string moduleName = "neurus_broken_reload_module";
	const fs::path modulePath = dir.path() / (moduleName + ".py");
	WriteText(modulePath, "VALUE = 1\n");

	ASSERT_TRUE(engine.RunStringChecked(
		"import sys\nsys.path.insert(0, r'" + PythonLiteral(dir.path().generic_string()) + "')"));
	engine.SetReloadRoots({dir.path().generic_string()});
	ASSERT_TRUE(engine.RunStringChecked("import " + moduleName));

	// Break the module and move its mtime ahead so the next run tries to reload.
	WriteText(modulePath, "VALUE = (\n");
	ASSERT_TRUE(BumpMtime(modulePath));

	const fs::path marker = dir.path() / "run_marker.txt";
	const std::string source =
		"open(r'" + PythonLiteral(marker.generic_string()) + "', 'w').write('ran')";

	engine.Console().Clear();
	EXPECT_FALSE(engine.RunStringChecked(source));
	EXPECT_FALSE(fs::exists(marker)) << "the new source ran despite a failed reload";
	EXPECT_TRUE(ConsoleContains(engine, neurus::ScriptLineKind::Error, moduleName));

	engine.Shutdown();
}
