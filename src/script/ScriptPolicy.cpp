/**
 * @file ScriptPolicy.cpp
 * @brief Value constructors for the Python-free script policy.
 */

#include "script/ScriptPolicy.h"

namespace neurus
{

ScriptPolicy ScriptPolicy::Trusted()
{
	return ScriptPolicy{};
}

ScriptPolicy ScriptPolicy::Sandboxed(const std::string& projectRoot)
{
	ScriptPolicy policy;
	policy.mode = ScriptMode::Sandboxed;
	if (!projectRoot.empty())
	{
		policy.readRoots.push_back(projectRoot);
		policy.writeRoots.push_back(projectRoot);
	}
	policy.allowNetwork = false;
	policy.allowSubprocess = false;
	policy.allowCtypes = false;
	policy.wallClockLimit = std::chrono::seconds(30);
	return policy;
}

const char* ScriptPolicy::ModeName(ScriptMode mode)
{
	switch (mode)
	{
	case ScriptMode::Trusted:
		return "trusted";
	case ScriptMode::Sandboxed:
		return "sandboxed";
	}
	return "trusted";
}

}  // namespace neurus
