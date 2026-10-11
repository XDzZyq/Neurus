/**
 * @file DataRegistration.h
 * @brief Forces linking of the pooled data-resource polymorphic registrations.
 *
 * Include this at any serialization site that touches the resource pool so
 * the static-library registration TU is not dead-stripped.
 */

#pragma once

#include <cereal/types/polymorphic.hpp>

CEREAL_FORCE_DYNAMIC_INIT(neurus_data_types)
