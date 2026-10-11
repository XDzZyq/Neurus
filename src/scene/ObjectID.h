/**
 * @file ObjectID.h
 * @brief Scene identity + metadata base class for all scene graph objects.
 *
 * ObjectID extends the core UID primitive with scene-specific metadata:
 * - Object name for display
 * - Type enumeration for runtime type discrimination
 * - Visibility flags for viewport and rendering
 * - Polymorphic accessors for optional components (Transform, Shader, Material)
 */

#pragma once

#include <string>

#include <cereal/cereal.hpp>
#include <cereal/types/string.hpp>

#include "core/Serialize.h"
#include "core/UID.h"

namespace neurus
{

/**
 * @brief Base class for all scene graph objects with type and component access.
 *
 * Extends UID with the object name, GOType tag, visibility flags and the
 * optional-component accessors; type-specific data lives in derived classes.
 */
class ObjectID : public UID
{
public:
	/**
	 * @brief Scene object type, used for runtime type identification without RTTI.
	 */
	enum class GOType
	{
		NONE_GO = -1,   ///< Invalid or uninitialized object
		GO_CAM,         ///< Camera object
		GO_MESH,        ///< Mesh geometry
		GO_LIGHT,       ///< Light source (point, sun, spot, area)
		GO_POLYLIGHT,   ///< Polygonal area light
		GO_ENVIR,       ///< Environment map for IBL
		GO_SPRITE,      ///< 2D sprite
		GO_DL,          ///< Debug line primitive
		GO_DP,          ///< Debug point primitive
		GO_DM,          ///< Debug mesh
		GO_SDFFIELD     ///< SDF volume for soft shadows
	};

public:
	std::string o_name;                        ///< Display name
	GOType o_type = GOType::NONE_GO;           ///< Runtime type identifier

	mutable bool is_viewport = true;           ///< Visible in viewport
	mutable bool is_rendered = true;           ///< Included in rendering pipeline

	/**
	 * @brief True for either light type (GO_LIGHT, GO_POLYLIGHT).
	 *
	 * A member rather than a predicate in each caller, so the two-type test
	 * lives in one place.
	 */
	bool IsLight() const
	{
		return o_type == GOType::GO_LIGHT || o_type == GOType::GO_POLYLIGHT;
	}

	/**
	 * @brief Sets visibility flags for viewport and rendering.
	 * @param v Viewport visibility.
	 * @param r Render visibility (included in render passes).
	 */
	void SetVisible(bool v, bool r)
	{
		is_viewport = v;
		is_rendered = r;
	}

	/**
	 * @brief Returns pointer to object's shader, or nullptr if it has none.
	 * @return Void pointer cast to Shader*.
	 */
	virtual void* GetShader()
	{
		return nullptr;
	}

	/**
	 * @brief Returns pointer to the ShaderUnit for a shader stage, or nullptr.
	 * @param shaderType Integer representing the ShaderType to query.
	 * @return Void pointer cast to ShaderUnit*.
	 */
	virtual void* GetShaderUnit(int /*shaderType*/) const
	{
		return nullptr;
	}

	/**
	 * @brief Returns pointer to object's transform, or nullptr if it has none.
	 * @return Void pointer cast to Transform*.
	 */
	virtual void* GetTransform()
	{
		return nullptr;
	}

	/**
	 * @brief Returns pointer to object's material, or nullptr if it has none.
	 * @return Void pointer cast to Material*.
	 */
	virtual void* GetMaterial()
	{
		return nullptr;
	}

	/**
	 * @brief Constructs an ObjectID with default values.
	 */
	ObjectID() = default;

	/**
	 * @brief Destroys the ObjectID.
	 */
	~ObjectID() override = default;

	/**
	 * @brief Cereal serialization for object identity and metadata.
	 *
	 * The UID hook stays strict - identity is what cross-references resolve
	 * through - while the metadata around it is optional: each concrete
	 * default constructor already sets `o_type` and `o_name`.
	 *
	 * @tparam Archive Cereal archive type (input or output).
	 * @param ar Archive to serialize to/from.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::base_class<UID>(this));
		archive::Optional(ar, CEREAL_NVP(o_name), CEREAL_NVP(o_type),
		                  CEREAL_NVP(is_viewport), CEREAL_NVP(is_rendered));
	}
};

} // namespace neurus
