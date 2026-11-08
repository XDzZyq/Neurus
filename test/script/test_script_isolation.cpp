/**
 * @file test_script_isolation.cpp
 * @brief The script layer must not reach into the renderer or UI.
 *
 * `src/script/` sits on the Editor layer: it may include `editor/`, `core/` and
 * `scene/` headers, but never `render/` or `ui/`. This scans the layer's own
 * sources for a forbidden include so the boundary cannot erode silently.
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

TEST(ScriptIsolation, NoRendererOrUiIncludes)
{
	const fs::path root = fs::path(TEST_RELATIVE_PROJECT_DIR) / "src" / "script";
	ASSERT_TRUE(fs::exists(root)) << "missing script layer at " << root.string();

	std::vector<std::string> violations;
	for (const auto& entry : fs::recursive_directory_iterator(root))
	{
		if (!entry.is_regular_file())
		{
			continue;
		}

		const std::string ext = entry.path().extension().string();
		if (ext != ".h" && ext != ".cpp")
		{
			continue;
		}

		std::ifstream in(entry.path());
		std::string line;
		int lineNo = 0;
		while (std::getline(in, line))
		{
			++lineNo;
			if (line.find("#include") == std::string::npos)
			{
				continue;
			}
			if (line.find("render/") != std::string::npos || line.find("ui/") != std::string::npos)
			{
				violations.push_back(entry.path().filename().string() + ":" + std::to_string(lineNo) + ": " + line);
			}
		}
	}

	for (const auto& v : violations)
	{
		ADD_FAILURE() << "forbidden include in src/script: " << v;
	}
	EXPECT_TRUE(violations.empty());
}
