/**
 * @file DataRegistration.cpp
 * @brief Cereal polymorphic registration for pooled data resources.
 *
 * Registers mesh, image and render graph resources as polymorphic leaves of the UID base so
 * the resource pool can serialize them by type name.
 */

#include <cereal/archives/json.hpp>
#include <cereal/types/polymorphic.hpp>

#include "asset/data/ImageData.hpp"
#include "asset/data/MeshData.hpp"
#include "asset/data/RenderGraphData.hpp"
#include "core/UID.hpp"

// --- Derived data-resource registration ---
CEREAL_REGISTER_TYPE(neurus::MeshData)
CEREAL_REGISTER_TYPE(neurus::ImageData)
CEREAL_REGISTER_TYPE(neurus::RenderGraphData)

// --- Polymorphic relations (UID base -> data resource leaves) ---
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::UID, neurus::MeshData)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::UID, neurus::ImageData)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::UID, neurus::RenderGraphData)

CEREAL_REGISTER_DYNAMIC_INIT(neurus_data_types)
