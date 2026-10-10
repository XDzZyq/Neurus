/**
 * @file Sprite.h
 * @brief Billboard icon sprite for in-viewport object markers.
 *
 * Sprites are simple 2D billboards rendered in the viewport as visual
 * markers for scene objects (lights, cameras, environment probes, etc.).
 *
 * Architecture:
 * - Sprite inherits ObjectID for scene graph identity and type tracking
 * - Holds a shared_ptr<Texture> reference (GPU resource owned by Renderer)
 * - ParsePath() resolves icon file paths based on SpriteType
 * - Pure data class - no GPU rendering logic
 *
 * @note GPU rendering of sprites is a separate concern and lives in the
 *       Renderer layer (future: sprite shader pipeline).
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
 * Provides:
 * - Type-to-path resolution via ParsePath()
 * - Shared Texture reference for icon rendering
 * - Opacity control for alpha blending
 *
 * Usage:
 * @code
 *   Sprite sprite;
 *   sprite.spr_type = SpriteType::POINT_LIGHT_SPRITE;
 *   std::string iconPath = sprite.ParsePath();
 * @endcode
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
	// Public members (direct access for scene / renderer iteration)
	// -------------------------------------------------------------------

	/** @brief Shared reference to the icon texture (owned by Renderer / Resource layer). */
	std::shared_ptr<Texture> spr_tex;

	/** @brief Opacity for alpha-blended rendering (0.0 = invisible, 1.0 = opaque). */
	float spr_opacity = kDefaultOpacity;

	/** @brief Sprite type determining which icon texture is used. */
	SpriteType spr_type = SpriteType::NONE_SPRITE;

	// -------------------------------------------------------------------
	// Construction / destruction
	// -------------------------------------------------------------------

	/**
	 * @brief Constructs a Sprite with default values.
	 *
	 * Initializes base ObjectID with GO_SPRITE type.
	 * Texture pointer is null by default.
	 */
	Sprite();

	/**
	 * @brief Default virtual destructor.
	 *
	 * shared_ptr<Texture> works with forward-declared Texture because
	 * the deleter is type-erased at construction time, not destruction.
	 */
	~Sprite() override = default;

	/**
	 * @brief Cereal serialization for sprite.
	 * @tparam Archive Cereal archive type (input or output).
	 * @param ar Archive to serialize to/from.
	 * @note spr_tex (shared_ptr<Texture>) is a GPU resource and is
	 *       NOT serialized. It must be re-resolved after deserialization.
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
	 * @brief Resolves the file path to the icon texture for the current spr_type.
	 *
	 * @return Icon file path string, or empty string for NONE_SPRITE.
	 * @note Paths are relative to the application resource directory.
	 */
	std::string ParsePath() const;
};

} // namespace neurus

