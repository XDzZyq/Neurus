/**
 * @file Sprite.h
 * @brief Billboard icon sprite for in-viewport object markers.
 *
 * Sprites are 2D billboards drawn in the viewport as visual markers for
 * scene objects (lights, cameras, environment probes, etc.). Pure CPU data:
 * the icon reference, type and opacity.
 *
 * @note Texture is forward-declared only. shared_ptr works with incomplete
 *       types because the deleter is type-erased at construction time.
 */

#pragma once

#include <memory>
#include <string>

#include <cereal/types/base_class.hpp>

#include "core/Serialize.h"
#include "scene/ObjectID.h"

namespace neurus
{

// --- Forward declarations --------------------------------------------------

class Texture;

// -----------------------------------------------------------------------
// SpriteType enumeration
// -----------------------------------------------------------------------

/**
 * @brief Types of billboard sprites, determining the icon texture to display.
 */
enum SpriteType : int
{
	NONE_SPRITE = 0,          ///< No sprite (inactive / uninitialized)
	POINT_LIGHT_SPRITE,       ///< Point light source marker
	SUN_LIGHT_SPRITE,         ///< Directional / sun light marker
	SPOT_LIGHT_SPRITE,        ///< Spot light marker
	CAM_SPRITE,               ///< Camera marker
	ENVIRN_SPRITE             ///< Environment / IBL probe marker
};

// -----------------------------------------------------------------------
// Sprite class
// -----------------------------------------------------------------------

/**
 * @brief Billboard icon sprite for scene object markers.
 *
 * Resolves the icon path for its type via ParsePath() and carries the shared
 * icon texture and opacity.
 *
 * @note Non-copyable (inherited from UID). Default opacity is 0.9f.
 */
class Sprite : public ObjectID
{
public:
	// -------------------------------------------------------------------
	// Constants
	// -------------------------------------------------------------------

	/** @brief Default icon opacity (0.9 = slightly transparent). */
	static constexpr float kDefaultOpacity = 0.9f;

	// -------------------------------------------------------------------
	// Public members (direct access)
	// -------------------------------------------------------------------

	/** @brief Shared reference to the icon texture. */
	std::shared_ptr<Texture> spr_tex;

	/** @brief Opacity for alpha-blended rendering (0.0 = invisible, 1.0 = opaque). */
	float spr_opacity = kDefaultOpacity;

	/** @brief Sprite type determining which icon texture is used. */
	SpriteType spr_type = SpriteType::NONE_SPRITE;

	// -------------------------------------------------------------------
	// Construction / destruction
	// -------------------------------------------------------------------

	/** @brief Constructs a Sprite with default values (GO_SPRITE, null texture). */
	Sprite();

	/**
	 * @brief Default virtual destructor.
	 *
	 * shared_ptr<Texture> works with the forward-declared Texture because the
	 * deleter is type-erased at construction time, not destruction.
	 */
	~Sprite() override = default;

	/**
	 * @brief Cereal serialization for sprite.
	 * @tparam Archive Cereal archive type (input or output).
	 * @param ar Archive to serialize to/from.
	 * @note spr_tex is not serialized; it must be re-resolved after
	 *       deserialization.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::base_class<ObjectID>(this));
		archive::Optional(ar, CEREAL_NVP(spr_opacity), CEREAL_NVP(spr_type));
	}

	// -------------------------------------------------------------------
	// Type-to-path resolution
	// -------------------------------------------------------------------

	/**
	 * @brief Resolves the icon file path for the current spr_type.
	 *
	 * @return Icon file path, or empty string for NONE_SPRITE.
	 * @note Paths are relative to the application resource directory.
	 */
	std::string ParsePath() const;
};

} // namespace neurus

