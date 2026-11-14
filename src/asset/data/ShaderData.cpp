/**
 * @file ShaderData.cpp
 * @brief Base-class implementations for ShaderData (constructor, stage accessors, TypeToString).
 */

#include "ShaderData.h"

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

} // namespace neurus
