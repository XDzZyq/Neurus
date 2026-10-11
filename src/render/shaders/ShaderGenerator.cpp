/**
 * @file ShaderGenerator.cpp
 * @brief ShaderStruct IR → valid Vulkan GLSL string.
 */

#include "ShaderGenerator.h"

#include "ShaderStruct.h"

#include <sstream>

namespace neurus {

const char* ShaderGenerator::InterpStr(Interp i)
{
	switch (i) {
		case Interp::Flat:          return "flat ";
		case Interp::Noperspective: return "noperspective ";
		default:                    return "";
	}
}

std::string ShaderGenerator::Generate(ShaderStruct& s)
{
	// Empty shader -> minimal stub
	if (s.IsEmpty())
	{
		return "#version 450 core\nvoid main() {}\n";
	}

	std::ostringstream result;

	// 1. Version
	result << "#version " << (s.version > 0 ? s.version : 450) << " core\n\n";

	// 2. Extension directives
	if (!s.extensions.empty())
	{
		for (const auto& ext : s.extensions)
		{
			result << "#extension " << ext << " : require\n";
		}
		result << "\n";
	}

	// 3. Define directives (emitted early, used by all following declarations)
	if (!s.define_directives.empty())
	{
		for (const auto& def : s.define_directives)
		{
			result << def << "\n";
		}
		result << "\n";
	}

	// 4. AB_list - vertex inputs: layout(location = N) in type name;
	if (!s.AB_list.empty())
	{
		for (const auto& io : s.AB_list)
		{
			result << "layout(location = " << io.location << ") "
			       << InterpStr(io.interpolation)
			       << "in "
			       << (io.type == ParaType::CUSTOM && !io.typeName.empty()
			               ? io.typeName : ShaderStruct::ParseType(io.type))
			       << " " << io.name << ";\n";
		}
		result << "\n";
	}

	// 4. pass_list - render outputs: layout(location = N) out type name;
	if (!s.pass_list.empty())
	{
		for (const auto& io : s.pass_list)
		{
			result << "layout(location = " << io.location << ") "
			       << InterpStr(io.interpolation)
			       << "out "
			       << (io.type == ParaType::CUSTOM && !io.typeName.empty()
			               ? io.typeName : ShaderStruct::ParseType(io.type))
			       << " " << io.name << ";\n";
		}
		result << "\n";
	}

	// 5. struct_def_list - bare struct definitions
	if (!s.struct_def_list.empty())
	{
		for (const auto& def : s.struct_def_list)
		{
			result << "struct " << def.name << "\n{\n";
			for (const auto& field : def.fields)
			{
				result << "\t"
				   << (field.type == ParaType::CUSTOM && !field.typeName.empty()
				           ? field.typeName : ShaderStruct::ParseType(field.type))
				   << " " << field.name << ";\n";
			}
			result << "};\n\n";
		}
	}

	// 6. SB_list - storage buffers
	if (!s.SB_list.empty())
	{
		for (const auto& sb : s.SB_list)
		{
			result << "layout(std430, set = 0, binding = " << sb.binding
			       << ") readonly buffer " << sb.name << "\n{\n";
			for (const auto& field : sb.fields)
			{
				result << "\t"
				   << (field.type == ParaType::CUSTOM && !field.typeName.empty()
				           ? field.typeName : ShaderStruct::ParseType(field.type))
				   << " " << field.name << ";\n";
			}
			result << "};\n";
		}
		result << "\n";
	}

	// 7. ubuffer_list - uniform buffers
	if (!s.ubuffer_list.empty())
	{
		for (const auto& ub : s.ubuffer_list)
		{
			result << "layout(std140, set = 0, binding = " << ub.binding
			       << ") uniform " << ub.name << "\n{\n";
			for (const auto& field : ub.fields)
			{
				result << "\t"
				   << (field.type == ParaType::CUSTOM && !field.typeName.empty()
				           ? field.typeName : ShaderStruct::ParseType(field.type))
				   << " " << field.name << ";\n";
			}
			if (ub.varName.empty())
			{
				result << "};\n";
			}
			else
			{
				result << "} " << ub.varName << ";\n";
			}
		}
		result << "\n";
	}

	// 8. push_constants
	if (!s.push_constants.empty())
	{
		result << "layout(push_constant) uniform PushConstants\n{\n";
		for (const auto& pc : s.push_constants)
		{
			result << "\tlayout(offset = " << pc.offset << ") " << pc.typeName << " " << pc.name << ";\n";
		}
		if (!s.push_constants_var.empty())
		{
			result << "} " << s.push_constants_var << ";\n\n";
		}
		else
		{
			result << "};\n\n";
		}
	}

	// 9. spec_constants
	if (!s.spec_constants.empty())
	{
		for (const auto& sc : s.spec_constants)
		{
			result << "layout(constant_id = " << sc.binding << ") const "
			       << ShaderStruct::ParseType(sc.type) << " " << sc.name << " = "
			       << sc.defaultVal << ";\n";
		}
		result << "\n";
	}

	// 10. uniform_list
	if (!s.uniform_list.empty())
	{
		for (const auto& u : s.uniform_list)
		{
			if (u.binding >= 0)
			{
				if (!u.imageFormat.empty())
					result << "layout(" << u.imageFormat << ", binding = " << u.binding << ") ";
				else
					result << "layout(binding = " << u.binding << ") ";
			}
			result << "uniform ";
			if (!u.qualifiers.empty())
			{
				result << u.qualifiers << " ";
			}
			if (!u.actualType.empty())
			{
				result << u.actualType;
			}
			else
			{
				result << ShaderStruct::ParseType(u.type);
			}
			result << " " << u.name << ShaderStruct::ParseCount(u.count) << ";\n";
		}
		result << "\n";
	}

	// 11. input_list
	if (!s.input_list.empty())
	{
		for (const auto& in : s.input_list)
		{
			result << "in " << ShaderStruct::ParseType(in.type) << " " << in.name
			       << ShaderStruct::ParseCount(in.count) << ";\n";
		}
		result << "\n";
	}

	// 12. output_list
	if (!s.output_list.empty())
	{
		for (const auto& out : s.output_list)
		{
			result << "out " << ShaderStruct::ParseType(out.type) << " " << out.name
			       << ShaderStruct::ParseCount(out.count) << ";\n";
		}
		result << "\n";
	}

	// 13. glob_list
	if (!s.glob_list.empty())
	{
		for (const auto& g : s.glob_list)
		{
			result << ShaderStruct::ParseType(g.type) << " " << g.name << " = "
			       << ShaderStruct::ParseType(g.type) << "(" << g.defaultVal << ");\n";
		}
		result << "\n";
	}

	// 14. const_list
	if (!s.const_list.empty())
	{
		for (const auto& c : s.const_list)
		{
			result << "const " << ShaderStruct::ParseType(c.returnType) << " " << c.name
			       << " = " << c.body << ";\n";
		}
		result << "\n";
	}

	// 15. vari_list
	if (!s.vari_list.empty())
	{
		for (const auto& v : s.vari_list)
		{
			result << v.typeName << " " << v.name << ShaderStruct::ParseCount(v.count) << ";\n";
		}
		result << "\n";
	}

	// 16. func_list
	if (!s.func_list.empty())
	{
		for (const auto& f : s.func_list)
		{
			result << ShaderStruct::ParseType(f.returnType) << " " << f.name
			       << ShaderStruct::ParseArgs(f.args) << "\n{\n"
			       << f.body << "\n}\n\n";
		}
	}

	// 17. Compute workgroup size (only for compute shaders)
	if (s.local_size_x > 0)
	{
		result << "layout(local_size_x = " << s.local_size_x
		       << ", local_size_y = " << s.local_size_y
		       << ", local_size_z = " << s.local_size_z << ") in;\n\n";
	}

	// 18. main() entry point
	std::string mainBody = s.Main;
	if (!mainBody.empty() && mainBody.back() == '\n')
	{
		mainBody.pop_back();
	}
	if (mainBody.empty())
	{
		result << "void main()\n{\n}\n";
	}
	else
	{
		result << "void main()\n{\n" << mainBody << "\n}\n";
	}

	s.is_struct_changed = false;
	return result.str();
}

} // namespace neurus
