/**
 * @file ShaderData.cpp
 * @brief ShaderData stage access, source file IO and identity.
 */

#include "ShaderData.hpp"

#include "core/Log.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace neurus {

// --------------------------------------
// Constructor - initialises name only; m_stages starts empty
// --------------------------------------

ShaderData::ShaderData(std::string name)
	: m_name(std::move(name))
{
}

// --------------------------------------
// Stage accessors
// --------------------------------------

bool ShaderData::HasStage(ShaderType type) const
{
	return m_stages.count(type) > 0;
}

ShaderUnit* ShaderData::FindStage(ShaderType type)
{
	auto it = m_stages.find(type);
	return it == m_stages.end() ? nullptr : &it->second;
}

const ShaderUnit* ShaderData::FindStage(ShaderType type) const
{
	auto it = m_stages.find(type);
	return it == m_stages.end() ? nullptr : &it->second;
}

ShaderUnit& ShaderData::GetStage(ShaderType type)
{
	return m_stages[type];
}

const ShaderUnit& ShaderData::GetStage(ShaderType type) const
{
	return m_stages.at(type);
}

// --------------------------------------
// Type -> string conversion
// --------------------------------------

std::string ShaderData::TypeToString(ShaderType type)
{
	switch (type)
	{
	case ShaderType::VERTEX:   return "VERTEX";
	case ShaderType::FRAGMENT: return "FRAGMENT";
	case ShaderType::COMPUTE:  return "COMPUTE";
	case ShaderType::GEOMETRY: return "GEOMETRY";
	default:                   return "UNKNOWN";
	}
}

// --------------------------------------
// Source file IO
// --------------------------------------

std::string ShaderData::ResolveSourcePath(const std::string& path)
{
	// Keep the established shader search order: the configured source directory,
	// then ancestors, then the working directory. Absolute paths pass through.
	if (!path.empty()
		&& (path[0] == '/' || path[0] == '\\'
			|| (path.size() > 1 && path[1] == ':')))
	{
		return path;
	}

	std::string relative = path;
	constexpr const char* prefix = "res/shaders/";
	constexpr size_t prefixLength = 12;
	if (relative.size() > prefixLength && relative.compare(0, prefixLength, prefix) == 0)
		relative.erase(0, prefixLength);

	const std::string configured = std::string(NEURUS_SHADER_DIR) + relative;
	if (std::filesystem::exists(configured))
	{
		NEURUS_LOG("[ShaderData] Resolved '" << path << "' -> '" << configured << "'");
		return configured;
	}

	for (const char* ancestor : {"../../../", "../../", "../", ""})
	{
		const std::string candidate = std::string(ancestor) + path;
		if (std::filesystem::exists(candidate))
		{
			NEURUS_LOG("[ShaderData] Resolved '" << path << "' -> '" << candidate << "'");
			return candidate;
		}
	}

	NEURUS_LOG("[ShaderData] Could NOT resolve shader path '" << path << "'");
	return path;
}

std::optional<std::string> ShaderData::ReadSource(const std::string& path, std::string* error)
{
	if (error)
		error->clear();
	const std::string resolved = ResolveSourcePath(path);
	const auto fail = [&path, &resolved, error](const char* reason) -> std::optional<std::string>
	{
		const std::string message = std::string(reason) + " '" + path
		                          + "' (resolved to '" + resolved + "')";
		if (error)
			*error = message;
		else
			NEURUS_ERR("[ShaderData] " << message);
		return std::nullopt;
	};

	std::ifstream file(resolved);
	if (!file.is_open())
		return fail("cannot open file");
	std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	if (file.bad())
		return fail("cannot read file");
	if (source.empty())
		return fail("file is empty");
	return source;
}

} // namespace neurus
