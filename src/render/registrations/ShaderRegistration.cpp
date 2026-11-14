/**
 * @file ShaderRegistration.cpp
 * @brief Cereal polymorphic registration for pooled shaders.
 *
 * Registers the concrete RenderShader and ComputeShader leaves of the
 * asset-layer ShaderData base, so the ResourceManager pool can serialize
 * pooled shaders by type name. ShaderData is abstract and is deliberately
 * not registered.
 */

#include <cereal/archives/json.hpp>
#include <cereal/types/polymorphic.hpp>

#include "core/UID.h"
#include "render/shaders/ComputeShader.h"
#include "render/shaders/RenderShader.h"

// --- Derived shader registration (concrete leaves only) ---
CEREAL_REGISTER_TYPE(neurus::RenderShader)
CEREAL_REGISTER_TYPE(neurus::ComputeShader)

// --- Polymorphic relations (UID base -> shader leaves) ---
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::UID, neurus::RenderShader)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::UID, neurus::ComputeShader)

CEREAL_REGISTER_DYNAMIC_INIT(neurus_shader_types)
