/**
 * @file OperationRegistration.cpp
 * @brief Cereal polymorphic type registration for undoable operations.
 *
 * Registers every concrete Operation subclass with cereal's polymorphic
 * serialization system so the undo/redo stacks — held as
 * std::vector<std::unique_ptr<Operation>> — round-trip through a project file:
 * serializing a base-class pointer emits the concrete type's stable name and
 * payload, and loading reconstructs the right subclass automatically.
 *
 * Mirrors src/scene/registrations/TypeRegistration.cpp (scene objects). These
 * macros generate template specializations at global scope, so they MUST
 * reside in a .cpp file (not a header) to avoid ODR violations. The abstract
 * Operation base is NOT registered — only concrete leaf types are.
 */

#include <cereal/archives/json.hpp>
#include <cereal/types/polymorphic.hpp>

#include "editor/operations/ConfigOperations.h"
#include "editor/operations/Operation.h"
#include "editor/operations/SceneOperations.h"
#include "editor/operations/ShaderOperations.h"

// --- Concrete type registration (stable, stringized token names) ---
CEREAL_REGISTER_TYPE(neurus::SetLightPowerOp)
CEREAL_REGISTER_TYPE(neurus::SetLightColorOp)
CEREAL_REGISTER_TYPE(neurus::SetLightShadowOp)
CEREAL_REGISTER_TYPE(neurus::SetPositionOp)
CEREAL_REGISTER_TYPE(neurus::SetRotationOp)
CEREAL_REGISTER_TYPE(neurus::SetScaleOp)
CEREAL_REGISTER_TYPE(neurus::SetVisibilityOp)
CEREAL_REGISTER_TYPE(neurus::SetLightRadiusOp)
CEREAL_REGISTER_TYPE(neurus::SetLightCutoffOp)
CEREAL_REGISTER_TYPE(neurus::SetLightOuterCutoffOp)
CEREAL_REGISTER_TYPE(neurus::SetMeshShadowOp)
CEREAL_REGISTER_TYPE(neurus::SetMeshMaterialOp)
CEREAL_REGISTER_TYPE(neurus::SetEnvIntensityOp)
CEREAL_REGISTER_TYPE(neurus::SetEnvRotationOp)
CEREAL_REGISTER_TYPE(neurus::SetDebugColorOp)
CEREAL_REGISTER_TYPE(neurus::SetDebugOpacityOp)
CEREAL_REGISTER_TYPE(neurus::SetDebugXRayOp)
CEREAL_REGISTER_TYPE(neurus::SetDebugLineWidthOp)
CEREAL_REGISTER_TYPE(neurus::SetDebugStippleOp)
CEREAL_REGISTER_TYPE(neurus::SetDebugSmoothOp)
CEREAL_REGISTER_TYPE(neurus::SetDebugPointTypeOp)
CEREAL_REGISTER_TYPE(neurus::SetDebugPointScaleOp)
CEREAL_REGISTER_TYPE(neurus::SetDebugProjectionModeOp)
CEREAL_REGISTER_TYPE(neurus::SetDebugPositionsOp)
CEREAL_REGISTER_TYPE(neurus::CameraTransformOp)
CEREAL_REGISTER_TYPE(neurus::CameraZoomOp)
CEREAL_REGISTER_TYPE(neurus::CameraFovOp)
CEREAL_REGISTER_TYPE(neurus::SetSelectionOp)
CEREAL_REGISTER_TYPE(neurus::SceneObjectAddOp)
CEREAL_REGISTER_TYPE(neurus::CompositeOp)
CEREAL_REGISTER_TYPE(neurus::SetRenderConfigOp)
CEREAL_REGISTER_TYPE(neurus::SetShaderCodeOp)
CEREAL_REGISTER_TYPE(neurus::SetShaderFieldOp)
CEREAL_REGISTER_TYPE(neurus::AddShaderFieldOp)
CEREAL_REGISTER_TYPE(neurus::ShaderLinkOp)

// --- Polymorphic relations (Operation base → concrete derived) ---
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetLightPowerOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetLightColorOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetLightShadowOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetPositionOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetRotationOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetScaleOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetVisibilityOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetLightRadiusOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetLightCutoffOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetLightOuterCutoffOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetMeshShadowOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetMeshMaterialOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetEnvIntensityOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetEnvRotationOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetDebugColorOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetDebugOpacityOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetDebugXRayOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetDebugLineWidthOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetDebugStippleOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetDebugSmoothOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetDebugPointTypeOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetDebugPointScaleOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetDebugProjectionModeOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetDebugPositionsOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::CameraTransformOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::CameraZoomOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::CameraFovOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetSelectionOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SceneObjectAddOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::CompositeOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetRenderConfigOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetShaderCodeOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::SetShaderFieldOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::AddShaderFieldOp)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::Operation, neurus::ShaderLinkOp)

// Forces this TU's registration to link when built into a static library.
// Serialization sites include OperationRegistration.h (CEREAL_FORCE_DYNAMIC_INIT)
// to odr-use this symbol so the registrations above are never dead-stripped.
CEREAL_REGISTER_DYNAMIC_INIT(neurus_operations)
