#pragma once

#include "ShaderUnit.h"

#include <cereal/types/base_class.hpp>
#include <cereal/types/string.hpp>

#include "core/UID.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace neurus {

/**
 * @brief Shader stage types supported by the renderer.
 */
enum class ShaderType
{
	VERTEX = 0,   ///< Vertex shader stage
	FRAGMENT = 1, ///< Fragment (pixel) shader stage
	COMPUTE = 2,  ///< Compute shader stage (GPGPU)
	GEOMETRY = 3  ///< Geometry shader stage
};

/**
 * @brief Abstract base class for all shader types in the dynamic shader system.
 *
 * Defines the common CPU-only interface for parsing, generating source and
 * accessing per-stage IR data. Subclasses (RenderShader, ComputeShader)
 * implement stage-specific parsing and generation logic.
 *
 * @note Non-copyable, non-movable (UID semantics - held via shared_ptr).
 * @note Not thread-safe; must be used from the main thread.
 */
class Shader : public UID
{
public:
	/**
	 * @brief Constructs a shader with a human-readable name.
	 * @param name Human-readable shader name (for logging and debugging).
	 */
	explicit Shader(std::string name = "");
	virtual ~Shader() = default;

	// Non-copyable / non-movable (inherits UID semantics).
	Shader(const Shader&) = delete;
	Shader& operator=(const Shader&) = delete;
	Shader(Shader&&) = delete;
	Shader& operator=(Shader&&) = delete;

	/**
	 * @brief Cereal serialization - forwards to UID + name.
	 *
	 * Concrete leaves must forward cereal::base_class<Shader>(this) and their
	 * own source paths so they can re-parse after load.
	 *
	 * @tparam Archive Cereal archive type (input or output).
	 * @param ar Archive to serialize to/from.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::base_class<UID>(this), CEREAL_NVP(m_name));
	}

	/**
	 * @brief Parses shader source files and generates GLSL code.
	 *
	 * Derived classes implement stage-specific parsing and code generation.
	 *
	 * @return true if all stages were parsed and generated successfully.
	 */
	virtual bool ParseAndGenerate() = 0;

	/**
	 * @brief Returns the primary shader type.
	 * @return The ShaderType this shader represents.
	 */
	virtual ShaderType GetType() const = 0;

	/** Returns true if the shader has the given stage. */
	bool HasStage(ShaderType type) const;

	/**
	 * @brief Returns a mutable reference to the ShaderUnit for a stage.
	 * @param type The shader stage to look up.
	 * @return Reference to the ShaderUnit. Default-inserts if not present.
	 */
	ShaderUnit& GetStage(ShaderType type);

	/** @brief Const overload of GetStage. Throws if type not found. */
	const ShaderUnit& GetStage(ShaderType type) const;

	/**
	 * @brief Returns a mutable reference to the ShaderStruct IR for a stage.
	 * @param type The shader stage to look up.
	 * @return Reference to the ShaderStruct IR.
	 */
	ShaderStruct& GetParsedStruct(ShaderType type);

	/** @brief Const overload of GetParsedStruct. Throws if type not found. */
	const ShaderStruct& GetParsedStruct(ShaderType type) const;

	/**
	 * @brief Returns the generated GLSL code for a stage.
	 * @param type The shader stage to look up.
	 * @return Reference to the generated code string.
	 */
	const std::string& GetGeneratedCode(ShaderType type) const;

	/**
	 * @brief Converts a ShaderType to its string representation.
	 * @param type The shader stage type.
	 * @return String name (e.g., "VERTEX", "FRAGMENT", "COMPUTE", "GEOMETRY").
	 */
	static std::string TypeToString(ShaderType type);

	/** @brief Returns the shader name. */
	const std::string& GetName() const { return m_name; }

	/** @brief Returns the last error message, if any. */
	const std::string& GetErrorMessage() const { return m_errorMessage; }

	/** @brief Monotonic version; bumped on each successful compile of any stage. */
	int  GetVersion() const            { return m_version; }
	void BumpVersion()                 { m_version++; }

protected:
	std::string m_name;          ///< Human-readable shader name
	std::string m_errorMessage;  ///< Last error message
	int         m_version = 0;   ///< Version counter for pipeline cache invalidation

	/** @brief Map of ShaderUnits, keyed by ShaderType. */
	std::unordered_map<ShaderType, ShaderUnit> m_stages;
};

} // namespace neurus
