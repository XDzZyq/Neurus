/**
 * @file Parameters.h
 * @brief ParaType enum and GLSL type string conversion utilities.
 *
 * Pure type metadata: identifies shader parameter types (float, vec2, mat4,
 * sampler2D, etc.) and maps between them and GLSL type strings. Holds no
 * values and no runtime state.
 */

#pragma once

#include <string>
#include <unordered_set>

namespace neurus {

/**
 * @brief Enumeration of supported shader parameter types.
 *
 * Maps to GLSL shader uniform types; NONE is the invalid/sentinel value.
 */
enum class ParaType : int
{
	NONE = -1, ///< Uninitialized or invalid parameter

	FLOAT,  ///< GLSL float
	INT,    ///< GLSL int
	UINT,   ///< GLSL uint
	BOOL,   ///< GLSL bool
	STRING, ///< Generic string (not a GLSL uniform type)

	VEC2, ///< GLSL vec2
	VEC3, ///< GLSL vec3
	VEC4, ///< GLSL vec4

	MAT3, ///< GLSL mat3
	MAT4, ///< GLSL mat4

	TEXTURE, ///< GLSL sampler2D
	CUSTOM   ///< User-defined custom type
};

/**
 * @brief Converts a ParaType to its GLSL type string representation.
 * @param type The ParaType value to convert.
 * @return GLSL type string (e.g., "float" for FLOAT, "vec3" for VEC3).
 *         Returns empty string for CUSTOM and "unknown" for unrecognized values.
 */
inline std::string ToString(ParaType type)
{
	switch (type)
	{
		case ParaType::FLOAT:   return "float";
		case ParaType::INT:     return "int";
		case ParaType::UINT:    return "uint";
		case ParaType::BOOL:    return "bool";
		case ParaType::STRING:  return "string";
		case ParaType::VEC2:    return "vec2";
		case ParaType::VEC3:    return "vec3";
		case ParaType::VEC4:    return "vec4";
		case ParaType::MAT3:    return "mat3";
		case ParaType::MAT4:    return "mat4";
		case ParaType::TEXTURE: return "sampler2D";
		case ParaType::CUSTOM:  return "";
		default:                return "unknown";
	}
}

/**
 * @brief Parses a GLSL type string into its corresponding ParaType.
 * @param type GLSL type string (e.g., "float", "vec3", "sampler2D").
 * @return Corresponding ParaType value, or ParaType::NONE if unrecognized.
 */
inline ParaType FromString(const std::string& type)
{
	if (type == "float")       return ParaType::FLOAT;
	if (type == "int")         return ParaType::INT;
	if (type == "uint")        return ParaType::UINT;
	if (type == "bool")        return ParaType::BOOL;
	if (type == "string")      return ParaType::STRING;
	if (type == "vec2")        return ParaType::VEC2;
	if (type == "vec3")        return ParaType::VEC3;
	if (type == "vec4")        return ParaType::VEC4;
	if (type == "mat3")        return ParaType::MAT3;
	if (type == "mat4")        return ParaType::MAT4;
	if (type == "sampler2D")   return ParaType::TEXTURE;
	return ParaType::NONE;
}

/**
 * @brief Checks whether a string corresponds to a known GLSL type.
 * @param type The string to check.
 * @return true if the string is a recognized GLSL type name.
 *
 * Covers a broader set than FromString, including integer vector types
 * (ivec2, uvec3, etc.) and sampler types beyond sampler2D.
 */
inline bool IsKnownType(const std::string& type)
{
	static const std::unordered_set<std::string> knownTypes = {
		"float", "int", "uint", "bool",
		"vec2", "vec3", "vec4",
		"ivec2", "ivec3", "ivec4",
		"uvec2", "uvec3", "uvec4",
		"mat3", "mat4",
		"sampler2D", "samplerCube", "sampler2DShadow",
		"image2D", "imageCube",
	};
	return knownTypes.count(type) > 0;
}

} // namespace neurus
