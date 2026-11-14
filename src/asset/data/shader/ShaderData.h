#pragma once

#include "ShaderStage.h"
#include "ShaderUnit.h"

#include <cereal/types/base_class.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/unordered_map.hpp>

#include "core/Serialize.h"
#include "core/UID.h"

#include <string>
#include <unordered_map>
#include <utility>

namespace neurus
{

/**
 * @brief CPU-side shader bundle: a name and one ShaderUnit per stage.
 *
 * `ShaderData` is the UID-tagged document a script addresses. The renderer's
 * `RenderShader` (vertex + fragment) and `ComputeShader` (compute) are its
 * concrete leaves; a `ShaderUnit` is one stage inside a bundle and is never
 * pooled on its own.
 *
 * @note Non-copyable, non-movable (UID semantics - held via shared_ptr).
 * @note Not thread-safe; must be used from the main thread.
 */
class ShaderData : public UID
{
public:
	/**
	 * @brief Constructs a shader bundle with a human-readable name.
	 * @param name Human-readable name (for logging and the pool registry).
	 */
	explicit ShaderData(std::string name = "");
	virtual ~ShaderData() = default;

	// Non-copyable / non-movable (inherits UID semantics).
	ShaderData(const ShaderData&) = delete;
	ShaderData& operator=(const ShaderData&) = delete;
	ShaderData(ShaderData&&) = delete;
	ShaderData& operator=(ShaderData&&) = delete;

	/**
	 * @brief Returns the bundle's primary shader stage.
	 * @return VERTEX for a render shader, COMPUTE for a compute shader.
	 */
	virtual ShaderType GetType() const = 0;

	/**
	 * @brief Cereal serialization - identity plus the human-readable name.
	 *
	 * Concrete leaves forward this base, serialize their own source paths, then
	 * call SerializeStages/DeserializeStages for the per-stage content.
	 *
	 * @tparam Archive Cereal archive type (input or output).
	 * @param ar Archive to serialize to/from.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::base_class<UID>(this), CEREAL_NVP(m_name));
	}

	/** @brief Returns true if the bundle has the given stage. */
	bool HasStage(ShaderType type) const;

	/**
	 * @brief Returns the stage's unit, or nullptr when the stage is absent.
	 *
	 * Unlike GetStage(), this never default-inserts a stage.
	 */
	ShaderUnit* FindStage(ShaderType type);
	const ShaderUnit* FindStage(ShaderType type) const;

	/** @brief Returns a mutable reference to a stage, default-inserting it. */
	ShaderUnit& GetStage(ShaderType type);
	/** @brief Const overload of GetStage; throws if the stage is absent. */
	const ShaderUnit& GetStage(ShaderType type) const;

	/**
	 * @brief Converts a ShaderType to its string representation.
	 * @param type The shader stage type.
	 * @return "VERTEX", "FRAGMENT", "COMPUTE" or "GEOMETRY".
	 */
	static std::string TypeToString(ShaderType type);

	/** @brief Returns the human-readable name. */
	const std::string& GetName() const { return m_name; }

	/** @brief Sets the human-readable name (and the pool registry key, by caller). */
	void SetName(std::string name) { m_name = std::move(name); }

	/** @brief Returns the last error message, if any. */
	const std::string& GetErrorMessage() const { return m_errorMessage; }

	/** @brief Monotonic version; bumped on each successful compile. */
	int  GetVersion() const { return m_version; }
	void BumpVersion() { m_version++; }

protected:
	/**
	 * @brief Writes the per-stage content map (leaf save path).
	 * @param ar Output archive.
	 */
	template<class Archive>
	void SerializeStages(Archive& ar)
	{
		ar(cereal::make_nvp("stages", m_stages));
	}

	/**
	 * @brief Reads the per-stage content map, keeping the constructor stages
	 *        when the key is absent (legacy path-only project).
	 * @param ar Input archive.
	 * @return true when the map was present and restored.
	 */
	template<class Archive>
	bool DeserializeStages(Archive& ar)
	{
		std::unordered_map<ShaderType, ShaderUnit> stages;
		if (archive::OptionalField(ar, cereal::make_nvp("stages", stages)))
		{
			m_stages = std::move(stages);
			return true;
		}
		return false;
	}

	std::string m_name;          ///< Human-readable shader name
	std::string m_errorMessage;  ///< Last error message
	int         m_version = 0;   ///< Version counter for pipeline invalidation

	/** @brief Map of ShaderUnits, keyed by ShaderType. */
	std::unordered_map<ShaderType, ShaderUnit> m_stages;
};

} // namespace neurus
