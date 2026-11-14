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

} // namespace neurus
