#pragma once

#include "ShaderStruct.h"
#include "ShaderStructSerialize.h"

#include <string>

namespace neurus {

struct ShaderUnit
{
	std::string path;                ///< Path to GLSL source file
	std::string code;                ///< Generated GLSL code (ShaderGenerator output)
	ShaderStruct parsed;             ///< Parsed IR (ShaderParser output)
	std::vector<uint32_t> spv;       ///< Compiled SPIR-V (ShaderLibrary::Compile output)
	int         m_version = 0;       ///< Monotonic version counter; bumped on successful compile

	int  GetVersion() const          { return m_version; }
	void BumpVersion()               { m_version++; }

	/**
	 * @brief Serializes the editable content only: path, code and parsed IR.
	 *
	 * SPIR-V and the version are runtime state and are rebuilt on load.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::make_nvp("path", path),
		   cereal::make_nvp("code", code),
		   cereal::make_nvp("parsed", parsed));
	}
};

} // namespace neurus
