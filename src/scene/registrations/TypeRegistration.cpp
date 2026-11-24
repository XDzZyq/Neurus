/**
 * @file TypeRegistration.cpp
 * @brief Cereal polymorphic type registration for scene objects.
 *
 * Registers all serializable scene types so derived types can be serialized
 * through base-class pointers. The macros generate template specializations at
 * global scope, so they must live in a .cpp file to avoid ODR violations.
 */

#include <cereal/archives/json.hpp>
#include "core/ResourceType.hpp"

#include "core/UID.hpp"
#include "scene/Camera.hpp"
#include "scene/DebugLine.hpp"
#include "scene/DebugMesh.hpp"
#include "scene/DebugPoints.hpp"
#include "scene/Environment.hpp"
#include "scene/Light.hpp"
#include "scene/Mesh.hpp"
#include "scene/ObjectID.hpp"
#include "scene/Sprite.hpp"

// --- Base type registration ---
NEURUS_REGISTER_RESOURCE_TYPE(UID)
NEURUS_REGISTER_RESOURCE_TYPE(ObjectID)

// --- Derived type registration ---
NEURUS_REGISTER_RESOURCE_TYPE(Camera)
NEURUS_REGISTER_RESOURCE_TYPE(Mesh)
NEURUS_REGISTER_RESOURCE_TYPE(Light)
NEURUS_REGISTER_RESOURCE_TYPE(Sprite)
NEURUS_REGISTER_RESOURCE_TYPE(DebugLine)
NEURUS_REGISTER_RESOURCE_TYPE(DebugPoints)
NEURUS_REGISTER_RESOURCE_TYPE(DebugMesh)
NEURUS_REGISTER_RESOURCE_TYPE(Environment)

// --- Polymorphic relations (UID base -> all pooled scene types) ---
// The pool stores shared_ptr<UID>, so every level of the hierarchy that can
// appear in it must be registered.
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
