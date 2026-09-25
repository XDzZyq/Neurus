/**
 * @file TypeRegistration.cpp
 * @brief Cereal polymorphic type registration for scene objects.
 *
 * Registers all serializable scene types with cereal's polymorphic
 * serialization system. This enables serializing derived types through
 * base class pointers (e.g. shared_ptr<UID> in the ResourceManager pool,
 * shared_ptr<ObjectID> in Scene::obj_list).
 *
 * These macros generate template specializations at global scope.
 * They MUST reside in a .cpp file (not a header) to avoid ODR violations.
 *
 * NOTE: the ResourceManager pool holds shared_ptr<UID>, so the UID-level
 * relations below are what make pool serialization load-bearing. Include
 * scene/registrations/TypeRegistration.h at every serialization site so this
 * static-library TU is not dead-stripped (see the memory note in
 * operation-system.instructions.md).
 */

#include <cereal/archives/json.hpp>
#include <cereal/types/polymorphic.hpp>

#include "core/UID.h"
#include "scene/Camera.h"
#include "scene/DebugLine.h"
#include "scene/DebugMesh.h"
#include "scene/DebugPoints.h"
#include "scene/Environment.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "scene/ObjectID.h"
#include "scene/Sprite.h"

// --- Base type registration ---
CEREAL_REGISTER_TYPE(neurus::UID)
CEREAL_REGISTER_TYPE(neurus::ObjectID)

// --- Derived type registration ---
CEREAL_REGISTER_TYPE(neurus::Camera)
CEREAL_REGISTER_TYPE(neurus::Mesh)
CEREAL_REGISTER_TYPE(neurus::Light)
CEREAL_REGISTER_TYPE(neurus::Sprite)
CEREAL_REGISTER_TYPE(neurus::DebugLine)
CEREAL_REGISTER_TYPE(neurus::DebugPoints)
CEREAL_REGISTER_TYPE(neurus::DebugMesh)
CEREAL_REGISTER_TYPE(neurus::Environment)

// --- Polymorphic relations (UID base -> all pooled scene types) ---
// The ResourceManager pool stores shared_ptr<UID>; cereal looks up the
// binding by (static base type, dynamic type) pair, so every level of the
// hierarchy that can appear in the pool must be registered.
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::UID, neurus::ObjectID)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::UID, neurus::Camera)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::UID, neurus::Mesh)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::UID, neurus::Light)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::UID, neurus::Sprite)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::UID, neurus::DebugLine)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::UID, neurus::DebugPoints)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::UID, neurus::DebugMesh)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::UID, neurus::Environment)

// --- Polymorphic relations (ObjectID base -> derived) ---
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::ObjectID, neurus::Camera)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::ObjectID, neurus::Mesh)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::ObjectID, neurus::Light)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::ObjectID, neurus::Sprite)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::ObjectID, neurus::DebugLine)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::ObjectID, neurus::DebugPoints)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::ObjectID, neurus::DebugMesh)
CEREAL_REGISTER_POLYMORPHIC_RELATION(neurus::ObjectID, neurus::Environment)

CEREAL_REGISTER_DYNAMIC_INIT(neurus_scene_types)
