/**
 * @file ShaderRegistration.hpp
 * @brief Forces linking of the pooled shader polymorphic registrations.
 *
 * Registers the RenderShader and ComputeShader leaves of UID. Application
 * composition and direct shader-persistence tests include this header so the
 * static-library registration TU is not discarded. Generic Asset persistence
 * does not depend on these concrete shader types.
 */

#pragma once

#include <cereal/types/polymorphic.hpp>

CEREAL_FORCE_DYNAMIC_INIT(neurus_shader_types)
