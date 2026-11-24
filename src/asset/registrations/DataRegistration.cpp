/**
 * @file DataRegistration.cpp
 * @brief Cereal polymorphic registration for pooled data resources.
 *
 * Registers mesh, image and render graph resources as polymorphic leaves of the UID base so
 * the resource pool can serialize them by type name.
 */

#include <cereal/archives/json.hpp>
#include <nlohmann/json.hpp>
#include "asset/ResourceType.hpp"
#include "asset/components/GraphRecordJson.hpp"

#include "asset/data/ImageData.hpp"
#include "asset/data/MeshData.hpp"
#include "asset/data/RenderGraphData.hpp"
#include "core/UID.hpp"

// --- Derived data-resource registration ---
NEURUS_REGISTER_RESOURCE_TYPE(MeshData, 1)
NEURUS_REGISTER_RESOURCE_TYPE(ImageData, 1)
NEURUS_REGISTER_RESOURCE_TYPE(RenderGraphData, 1, [](const nlohmann::ordered_json& data) {
	return data.contains("topology") && neurus::project::ValidGraphShape(data.at("topology"));
})

// --- Polymorphic relations (UID base -> data resource leaves) ---
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::UID, neurus::MeshData)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::UID, neurus::ImageData)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::UID, neurus::RenderGraphData)

CEREAL_REGISTER_DYNAMIC_INIT(neurus_data_types)
