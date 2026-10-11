/**
 * @file OperationRegistration.h
 * @brief Forces linking of the operation polymorphic-type registrations.
 *
 * Any translation unit that (de)serializes an Operation pointer must include
 * this header so the CEREAL_FORCE_DYNAMIC_INIT below pulls the registration TU
 * in and runs its static initializers before serialization.
 */

#pragma once

#include <cereal/types/polymorphic.hpp>

CEREAL_FORCE_DYNAMIC_INIT(neurus_operations)
