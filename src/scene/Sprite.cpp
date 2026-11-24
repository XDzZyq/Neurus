/**
 * @file Sprite.cpp
 * @brief Implementation of Sprite - billboard icon data object.
 */

#include "scene/Sprite.hpp"

namespace neurus
{

// -----------------------------------------------------------------------
// Construction
// -----------------------------------------------------------------------

Sprite::Sprite()
	: ObjectID(GOType::GO_SPRITE, "")
{
}

// -----------------------------------------------------------------------
// Type-to-path resolution
// -----------------------------------------------------------------------

std::string Sprite::ParsePath() const
{
	switch (spr_type)
	{
		case SpriteType::POINT_LIGHT_SPRITE:
			return "res/textures/icons/point_light.png";
		case SpriteType::SUN_LIGHT_SPRITE:
			return "res/textures/icons/sun_light.png";
		case SpriteType::SPOT_LIGHT_SPRITE:
			return "res/textures/icons/spot_light.png";
		case SpriteType::CAM_SPRITE:
			return "res/textures/icons/camera.png";
		case SpriteType::ENVIRN_SPRITE:
			return "res/textures/icons/environment.png";
		case SpriteType::NONE_SPRITE:
		default:
			return "";
	}
}

} // namespace neurus
