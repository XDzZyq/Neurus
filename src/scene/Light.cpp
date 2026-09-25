/**
 * @file Light.cpp
 * @brief Implementation of Light class.
 */

#include "scene/Light.h"

namespace neurus
{

Light::Light()
	: ObjectID()
	, Transform3D()
{
	o_type = ObjectID::GOType::GO_LIGHT;
	// Untyped default: ParseLightName() would say "None", which reads as an
	// error in the outliner. A load overwrites o_name from the archive anyway.
	o_name = "Light";
}

Light::Light(LightType type, float power, glm::vec3 color)
	: ObjectID()
	, Transform3D()
	, light_type(type)
	, light_power(power)
	, light_color(color)
{
	o_type = ObjectID::GOType::GO_LIGHT;
	o_name = ParseLightName(light_type).second;
}

std::pair<SpriteType, std::string> Light::ParseLightName(LightType _type)
{
	switch (_type)
	{
	case POINTLIGHT:
		return { static_cast<SpriteType>(1), "Point Light" };
	case SUNLIGHT:
		return { static_cast<SpriteType>(2), "Sun Light" };
	case SPOTLIGHT:
		return { static_cast<SpriteType>(3), "Spot Light" };
	case AREALIGHT:
		return { static_cast<SpriteType>(4), "Area Light" };
	case NONELIGHT:
	default:
		return { static_cast<SpriteType>(0), "None" };
	}
}

void Light::SetColor(const glm::vec3& _col)
{
	light_color = _col;
	is_light_changed = true;
}

void Light::SetPower(float _power)
{
	light_power = _power;
	is_light_changed = true;
}

void Light::SetShadow(bool _state)
{
	use_shadow = _state;
	is_light_changed = true;
}

void Light::SetRadius(float _rad)
{
	light_radius = _rad;
	is_light_changed = true;
}

void Light::SetCutoff(float _ang)
{
	spot_cutoff = _ang;
	is_light_changed = true;
}

void Light::SetOuterCutoff(float _ang)
{
	spot_outer_cutoff = _ang;
	is_light_changed = true;
}

void Light::SetRatio(float _ratio)
{
	area_ratio = _ratio;
	is_light_changed = true;
}

} // namespace neurus
