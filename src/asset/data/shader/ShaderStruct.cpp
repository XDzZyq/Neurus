/**
 * @file ShaderStruct.cpp
 * @brief Implementation of ShaderStruct - Reset(), static helpers, and setters.
 */

#include "ShaderStruct.h"

#include <sstream>

namespace neurus {

// --------------------------------------
// Static type table - maps ParaType indices and custom types to GLSL strings
// --------------------------------------

std::vector<std::string> ShaderStruct::type_table = {
	"float", "int", "uint", "bool", "none", "vec2", "vec3", "vec4", "mat3", "mat4", "sampler2D", "samplerCube", "image2D"
};

void ShaderStruct::ResetTypeTable()
{
	type_table = {"float", "int", "uint", "bool", "none", "vec2", "vec3", "vec4", "mat3", "mat4", "sampler2D", "samplerCube", "image2D"};
}

// --------------------------------------
// ParseType - ParaType ??? string
// --------------------------------------

std::string ShaderStruct::ParseType(ParaType type)
{
	// Only neurus::ToString() is authoritative: the ParaType enum no longer
	// aligns with the legacy type_table layout, so indexing it by enum value
	// would be wrong.
	return neurus::ToString(type);
}

ParaType ShaderStruct::ParseType(const std::string& type)
{
	// Entries beyond the built-in range are custom types and must round-trip
	// as ParaType::CUSTOM.
	for (size_t i = 0; i < type_table.size(); ++i)
	{
		if (type_table[i] == type)
		{
			if (i <= static_cast<size_t>(ParaType::TEXTURE))
			{
				return static_cast<ParaType>(i);
			}
			return ParaType::CUSTOM;
		}
	}

	// Fall back to Parameters.h for built-in types
	ParaType result = neurus::FromString(type);
	if (result != ParaType::NONE)
	{
		return result;
	}

	// Unknown type - register it as a custom type
	ADD_TYPE(type);
	return ParaType::CUSTOM;
}

// --------------------------------------
// ParseCount - array suffix
// --------------------------------------

std::string ShaderStruct::ParseCount(int count)
{
	return count > 1 ? "[" + std::to_string(count) + "]" : "";
}

// --------------------------------------
// ParseArgs - function arguments ??? GLSL string
// --------------------------------------

std::string ShaderStruct::ParseArgs(const Args& args)
{
	std::string result = "(";

	if (!args.empty())
	{
		for (const auto& [type, name, typeName] : args)
		{
			// Use the original type name for CUSTOM types; otherwise convert ParaType.
			const std::string typeStr = (type == ParaType::CUSTOM && !typeName.empty())
				? typeName : ParseType(type);
			result += typeStr + " " + name + ", ";
		}
		// Remove trailing ", "
		result.erase(result.end() - 2, result.end());
	}

	return result + ")";
}

Args ShaderStruct::ParseArgs(const std::string& args)
{
	Args result;
	std::istringstream str(args);
	std::string word;

	while (str >> word)
	{
		ParaType type = ParseType(word);
		str >> word;
		// Remove trailing comma or closing parenthesis
		if (!word.empty() && (word.back() == ',' || word.back() == ')'))
		{
			word.pop_back();
		}
		result.emplace_back(type, word, "");
	}

	return result;
}

// --------------------------------------
// IsAvailType - type lookup
// --------------------------------------

bool ShaderStruct::IsAvailType(const std::string& type)
{
	for (const auto& t : type_table)
	{
		if (t == type)
		{
			return true;
		}
	}
	return neurus::IsKnownType(type);
}

// --------------------------------------
// ADD_TYPE - register a custom type
// --------------------------------------

void ShaderStruct::ADD_TYPE(const std::string& name)
{
	// Only add if not already present among the custom types.
	static constexpr size_t kBuiltinCount = 11; // float through samplerCube

	for (size_t i = kBuiltinCount; i < type_table.size(); ++i)
	{
		if (type_table[i] == name)
		{
			return; // Already registered
		}
	}
	type_table.emplace_back(name);
}

// --------------------------------------
// Setters - OpenGL-ported (each sets is_struct_changed = true)
// --------------------------------------

void ShaderStruct::SetAB(int loc, ParaType type, const std::string& name, Interp interp)
{
	is_struct_changed = true;
	AB_list.push_back({loc, name, type, "", interp});
}

void ShaderStruct::SetPass(int loc, ParaType type, const std::string& name, Interp interp)
{
	is_struct_changed = true;
	pass_list.push_back({loc, name, type, "", interp});
}

void ShaderStruct::SetSB(int loc, const std::string& name, const Args& args)
{
	is_struct_changed = true;
	S_StructDef def;
	def.binding = loc;
	def.name = name;
	for (const auto& [argType, argName, argTypeName] : args)
	{
		def.fields.push_back({0, argName, argType,
		                      (argType == ParaType::CUSTOM ? argTypeName : "")});
	}
	SB_list.emplace_back(std::move(def));
}

void ShaderStruct::SetUB(std::string type, std::string name, const Args& args, int binding)
{
	is_struct_changed = true;
	S_StructDef def;
	def.binding = binding;
	def.name = std::move(type);
	def.varName = std::move(name);
	for (const auto& [argType, argName, argTypeName] : args)
	{
		def.fields.push_back({0, argName, argType,
		                      (argType == ParaType::CUSTOM ? argTypeName : "")});
	}
	ubuffer_list.emplace_back(std::move(def));
}

void ShaderStruct::SetUni(ParaType type, int count, const std::string& name, int binding,
                          const std::string& qualifiers, const std::string& actualType,
                          const std::string& imageFormat)
{
	is_struct_changed = true;
	S_Uniform uni;
	uni.name = name;
	uni.type = type;
	uni.count = count;
	uni.binding = binding;
	uni.qualifiers = qualifiers;
	uni.actualType = actualType;
	uni.imageFormat = imageFormat;
	uniform_list.push_back(uni);
}

void ShaderStruct::SetInp(ParaType type, int count, const std::string& name)
{
	is_struct_changed = true;
	input_list.push_back({name, type, count});
}

void ShaderStruct::SetOut(ParaType type, int count, const std::string& name)
{
	is_struct_changed = true;
	output_list.push_back({name, type, count});
}

void ShaderStruct::SetGlob(ParaType type, float defult, const std::string& name)
{
	is_struct_changed = true;
	glob_list.push_back({name, type, defult});
}

void ShaderStruct::DefStruct(const std::string& name, const Args& args)
{
	is_struct_changed = true;
	S_StructDef def;
	def.binding = 0; // Bare struct - no binding
	def.name = name;
	for (const auto& [argType, argName, argTypeName] : args)
	{
		def.fields.push_back({0, argName, argType,
		                      (argType == ParaType::CUSTOM ? argTypeName : "")});
	}
	struct_def_list.emplace_back(std::move(def));
}

void ShaderStruct::DefFunc(ParaType type, std::string name, const std::string& content, const Args& args)
{
	is_struct_changed = true;
	func_list.push_back({type, std::move(name), content, args});
}

void ShaderStruct::SetConst(ParaType type, const std::string& name, const std::string& content)
{
	is_struct_changed = true;
	const_list.push_back({type, name, content, Args{}});
}

void ShaderStruct::SetVar(const std::string& typeName, const std::string& name, int count)
{
	is_struct_changed = true;
	vari_list.push_back({typeName, name, count});
}

// --------------------------------------
// Vulkan-specific setters
// --------------------------------------

void ShaderStruct::SetPushConstant(const std::string& name, uint32_t offset, uint32_t size,
                                 const std::string& typeName)
{
	is_struct_changed = true;
	push_constants.push_back({name, offset, size, typeName});
}

void ShaderStruct::SetPushConstantVar(const std::string& var)
{
	is_struct_changed = true;
	push_constants_var = var;
}

void ShaderStruct::SetLocalSize(uint32_t x, uint32_t y, uint32_t z)
{
	is_struct_changed = true;
	local_size_x = x;
	local_size_y = y;
	local_size_z = z;
}

void ShaderStruct::SetVersion(int v)
{
	is_struct_changed = true;
	version = v;
}

void ShaderStruct::AddExtension(const std::string& ext)
{
	is_struct_changed = true;
	for (const auto& existing : extensions)
	{
		if (existing == ext)
		{
			return;
		}
	}
	extensions.push_back(ext);
}

void ShaderStruct::AddDefine(const std::string& directive)
{
	is_struct_changed = true;
	for (const auto& existing : define_directives)
	{
		if (existing == directive) return;
	}
	define_directives.push_back(directive);
}

// --------------------------------------
// Reset - clears ALL containers to default/empty state
// --------------------------------------

void ShaderStruct::Reset()
{
	AB_list.clear();
	pass_list.clear();
	SB_list.clear();
	ubuffer_list.clear();
	struct_def_list.clear();
	uniform_list.clear();
	input_list.clear();
	output_list.clear();
	const_list.clear();
	glob_list.clear();
	vari_list.clear();
	func_list.clear();

	Main.clear();

	push_constants.clear();
	push_constants_var.clear();
	spec_constants.clear();
	extensions.clear();
	define_directives.clear();

	local_size_x = 0;
	local_size_y = 0;
	local_size_z = 0;
	version = 0;

	is_struct_changed = true;
}

// --------------------------------------
// IsEmpty - true when no data has been set
// --------------------------------------

bool ShaderStruct::IsEmpty() const
{
	return AB_list.empty()
	    && pass_list.empty()
	    && SB_list.empty()
	    && ubuffer_list.empty()
	    && struct_def_list.empty()
	    && uniform_list.empty()
	    && input_list.empty()
	    && output_list.empty()
	    && const_list.empty()
	    && glob_list.empty()
	    && vari_list.empty()
	    && func_list.empty()
	    && Main.empty()
	    && push_constants.empty()
	    && spec_constants.empty()
	    && extensions.empty()
	    && define_directives.empty()
	    && version == 0
	    && local_size_x == 0;
}

} // namespace neurus
