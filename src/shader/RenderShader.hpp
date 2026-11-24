/**
 * @file RenderShader.hpp
 * @brief CPU-only render shader - vertex+fragment pipeline with parse->generate flow.
 *
 * RenderShader owns ShaderUnit IR representations for both vertex and fragment
 * stages via the base-class sh_stages map, and generated GLSL source code.
 *
 * Architecture:
 *   - Inherits Shader - base class with sh_stages ShaderUnit map.
 *   - Uses ShaderParser to populate ShaderStruct IRs.
 *   - Uses ShaderGenerator to emit GLSL source.
 *
 * Lifecycle:
 *   1. Construct: RenderShader(name, vertPath, fragPath)
 *   2. ParseAndGenerate: parse -> generate GLSL for both stages
 *   3. Access: GetStage(), parsed IR and generated code
 *   4. Recompile: Recompile(type) -> re-parse single stage, re-generate
 */
#pragma once

#include "asset/data/ShaderData.hpp"

namespace neurus {

/**
 * @brief Standard render shader with vertex and fragment stages.
 *
 * Manages parsing GLSL source into ShaderStruct IR and generating Vulkan
 * GLSL for both stages.
 *
 * @note Non-copyable, non-movable (UID semantics). Not thread-safe.
 */
class RenderShader : public ShaderData
{
public:
	/**
	 * @brief Constructs a render shader with file paths to GLSL source.
	 *
	 * The default arguments make this the constructor cereal's polymorphic
	 * load uses; content loads later via ReloadContent().
	 *
	 * @param name     Human-readable shader name (for logging and cache keys).
	 * @param vertPath Path to the vertex shader GLSL source file.
	 * @param fragPath Path to the fragment shader GLSL source file.
	 */
	RenderShader(std::string name = "", std::string vertPath = "", std::string fragPath = "");

	/**
	 * @brief Constructs a render shader from already resolved stage units.
	 *
	 * Used when the source stages are pooled copies rather than files: the
	 * units carry their own path, IR and generated code, so nothing is re-read
	 * from disk. The stage paths become the shader's source paths, which is
	 * what a later ReloadContent() and a project save use.
	 *
	 * @param name Human-readable shader name (for logging and cache keys).
	 * @param vert Fully resolved vertex stage.
	 * @param frag Fully resolved fragment stage.
	 */
	RenderShader(std::string name, ShaderUnit vert, ShaderUnit frag);

	~RenderShader() override = default;

	// Non-copyable / non-movable (inherits UID semantics).
	RenderShader(const RenderShader&) = delete;
	RenderShader& operator=(const RenderShader&) = delete;
	RenderShader(RenderShader&&) = delete;
	RenderShader& operator=(RenderShader&&) = delete;

	/**
	 * @brief (Re)loads both shader stages from disk.
	 *
	 * Called at the end of serialize(load); no-op for empty paths.
	 */
	void ReloadContent();

	/**
	 * @brief Compiles GLSL and validates the vertex/fragment location interface.
	 *
	 * @param outError Optional compiler or interface validation diagnostic.
	 * @return true when compilation and interface validation succeed; empty shells remain uncompiled.
	 */
	bool CompileToSpv(std::string* outError = nullptr);

	/**
	 * @brief Cereal serialization - base content + both source paths.
	 *
	 * Source-backed stages with empty code reload from their paths.
	 * CompileToSpv() then rebuilds the runtime SPIR-V, so
	 * pipelines are ready immediately after a project load.
	 *
	 * @tparam Archive Cereal archive type (input or output).
	 * @param ar Archive to serialize to/from.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::base_class<ShaderData>(this),
		   cereal::make_nvp("m_vertPath", sh_vertPath),
		   cereal::make_nvp("m_fragPath", sh_fragPath),
		   cereal::make_nvp("stages", sh_stages));
		if constexpr (Archive::is_loading::value)
		{
			const bool hasContent =
				!sh_stages[ShaderType::VERTEX].code.empty() ||
				!sh_stages[ShaderType::FRAGMENT].code.empty();
			if (!hasContent)
			{
				ReloadContent();
			}
			CompileToSpv();
		}
	}

	// ----------------------------------
	// Shader interface (override)
	// ----------------------------------

	/**
	 * @brief Parses both shader files and generates GLSL code.
	 *
	 * @return true if both stages parsed and generated successfully.
	 */
	bool ParseAndGenerate();

	/**
	 * @brief Returns the primary shader type (VERTEX).
	 */
	ShaderType GetType() const override { return ShaderType::VERTEX; }

	// ----------------------------------
	// RenderShader-specific API
	// ----------------------------------

	/** @brief Convenience: returns the vertex ShaderUnit. */
	ShaderUnit& GetVertex() { return GetStage(ShaderType::VERTEX); }

	/** @brief Convenience: returns the fragment ShaderUnit. */
	ShaderUnit& GetFragment() { return GetStage(ShaderType::FRAGMENT); }

	/** @brief Returns the vertex shader file path. */
	const std::string& GetVertPath() const;

	/** @brief Returns the fragment shader file path. */
	const std::string& GetFragPath() const;

	/**
	 * @brief Re-parses and re-generates a single shader stage (CPU-only).
	 *
	 * @param type Stage to recompile (VERTEX or FRAGMENT).
	 * @return true if re-parse and re-generation succeeded.
	 */
	bool Recompile(ShaderType type);

private:
	std::string sh_vertPath; ///< Path to vertex shader source file
	std::string sh_fragPath; ///< Path to fragment shader source file
};

} // namespace neurus
