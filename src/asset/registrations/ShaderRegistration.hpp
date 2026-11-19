/**
 * @file ShaderRegistration.hpp
 * @brief Forces linking of the pooled shader polymorphic registrations.
 *
 * The registrations live in ShaderRegistration.cpp (RenderShader as the
 * concrete pooled leaf of the UID base). Include this header at any
 * serialization site that touches the ResourceManager pool (e.g.
 * ResourceComponent.cpp) so the static-library TU is not dead-stripped.
 */

#pragma once

#include <cereal/types/polymorphic.hpp>

CEREAL_FORCE_DYNAMIC_INIT(neurus_shader_types)
