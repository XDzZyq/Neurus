/**
 * @file TypeRegistration.cpp
 * @brief Cereal polymorphic type registration for scene objects.
 *
 * Registers all serializable scene types so derived types can be serialized
 * through base-class pointers. The macros generate template specializations at
 * global scope, so they must live in a .cpp file to avoid ODR violations.
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
