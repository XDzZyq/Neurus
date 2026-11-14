/**
 * @file ShaderStructSerialize.h
 * @brief Non-intrusive cereal serialization for the ShaderStruct sub-structs.
 *
 * Lives next to the structs it serializes (renderer shader sub-layer) so the
 * future shader-IR serialization can reuse these exact overloads. They are
 * free functions declared in namespace neurus alongside the struct types, so
 * cereal finds them via ADL — ShaderStruct.h itself stays a pure data model
 * with no cereal members. cereal is already a renderer dependency
 * (RenderConfig.h serializes through it), so no new dependency is introduced.
 *
 * They currently exist so SetShaderFieldOp can round-trip a ShaderFieldValue
 * element (a std::variant over these five payload types) through the operation
 * history.
 *
 * @note enum members (ParaType, Interp) serialize natively via cereal's built-in
 *       enum support (same as ShaderSection in the operation payloads).
 */

#pragma once

#include <cereal/cereal.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/tuple.hpp>
#include <cereal/types/variant.hpp>
#include <cereal/types/vector.hpp>

#include "ShaderStruct.h"

namespace neurus {

/** @brief Serializes an S_IO (attribute / pass-output / struct member). */
template<class Archive>
void serialize(Archive& ar, S_IO& v)
{
	ar(cereal::make_nvp("location", v.location),
	   cereal::make_nvp("name", v.name),
	   cereal::make_nvp("type", v.type),
	   cereal::make_nvp("typeName", v.typeName),
	   cereal::make_nvp("interpolation", v.interpolation));
}

/** @brief Serializes an S_Uniform (uniform / input / output variable). */
template<class Archive>
void serialize(Archive& ar, S_Uniform& v)
{
	ar(cereal::make_nvp("name", v.name),
	   cereal::make_nvp("type", v.type),
	   cereal::make_nvp("count", v.count),
	   cereal::make_nvp("binding", v.binding),
	   cereal::make_nvp("qualifiers", v.qualifiers),
	   cereal::make_nvp("actualType", v.actualType),
	   cereal::make_nvp("imageFormat", v.imageFormat));
}

/** @brief Serializes an S_Func (function / const definition). */
template<class Archive>
void serialize(Archive& ar, S_Func& v)
{
	ar(cereal::make_nvp("returnType", v.returnType),
	   cereal::make_nvp("name", v.name),
	   cereal::make_nvp("body", v.body),
	   cereal::make_nvp("args", v.args));
}

/** @brief Serializes an S_PushConstant (push-constant block member). */
template<class Archive>
void serialize(Archive& ar, S_PushConstant& v)
{
	ar(cereal::make_nvp("name", v.name),
	   cereal::make_nvp("offset", v.offset),
	   cereal::make_nvp("size", v.size),
	   cereal::make_nvp("typeName", v.typeName));
}

/** @brief Serializes an S_StructDef (struct / buffer-block definition). */
template<class Archive>
void serialize(Archive& ar, S_StructDef& v)
{
	ar(cereal::make_nvp("binding", v.binding),
	   cereal::make_nvp("name", v.name),
	   cereal::make_nvp("fields", v.fields),
	   cereal::make_nvp("varName", v.varName));
}

/** @brief Serializes an S_Glob (global variable with a scalar default). */
template<class Archive>
void serialize(Archive& ar, S_Glob& v)
{
	ar(cereal::make_nvp("name", v.name),
	   cereal::make_nvp("type", v.type),
	   cereal::make_nvp("defaultVal", v.defaultVal));
}

/** @brief Serializes an S_Var (variable with a string type name). */
template<class Archive>
void serialize(Archive& ar, S_Var& v)
{
	ar(cereal::make_nvp("typeName", v.typeName),
	   cereal::make_nvp("name", v.name),
	   cereal::make_nvp("count", v.count));
}

/** @brief Serializes an S_SpecConstant (specialisation constant). */
template<class Archive>
void serialize(Archive& ar, S_SpecConstant& v)
{
	ar(cereal::make_nvp("binding", v.binding),
	   cereal::make_nvp("name", v.name),
	   cereal::make_nvp("type", v.type),
	   cereal::make_nvp("defaultVal", v.defaultVal));
}

/** @brief Serializes the whole ShaderStruct IR (all containers + scalars). */
template<class Archive>
void serialize(Archive& ar, ShaderStruct& v)
{
	ar(cereal::make_nvp("AB_list", v.AB_list),
	   cereal::make_nvp("pass_list", v.pass_list),
	   cereal::make_nvp("SB_list", v.SB_list),
	   cereal::make_nvp("ubuffer_list", v.ubuffer_list),
	   cereal::make_nvp("struct_def_list", v.struct_def_list),
	   cereal::make_nvp("uniform_list", v.uniform_list),
	   cereal::make_nvp("input_list", v.input_list),
	   cereal::make_nvp("output_list", v.output_list),
	   cereal::make_nvp("const_list", v.const_list),
	   cereal::make_nvp("glob_list", v.glob_list),
	   cereal::make_nvp("vari_list", v.vari_list),
	   cereal::make_nvp("func_list", v.func_list),
	   cereal::make_nvp("Main", v.Main),
	   cereal::make_nvp("push_constants", v.push_constants),
	   cereal::make_nvp("push_constants_var", v.push_constants_var),
	   cereal::make_nvp("spec_constants", v.spec_constants),
	   cereal::make_nvp("local_size_x", v.local_size_x),
	   cereal::make_nvp("local_size_y", v.local_size_y),
	   cereal::make_nvp("local_size_z", v.local_size_z),
	   cereal::make_nvp("extensions", v.extensions),
	   cereal::make_nvp("define_directives", v.define_directives),
	   cereal::make_nvp("version", v.version));
}

} // namespace neurus
