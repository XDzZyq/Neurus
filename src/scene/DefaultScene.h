/**
 * @file DefaultScene.h
 * @brief Default scene factory function.
 *
 * Provides CreateDefaultScene(), which constructs a pre-configured Scene
 * with one camera, one sphere mesh and one point light.
 */

#pragma once

#include <memory>
#include <string>

#include "Scene.h"

namespace neurus {

/**
 * @brief Creates the default scene: 1 camera, 1 sphere mesh, 1 point light.
 *
 * @param objPath Absolute or relative path to the sphere.obj file.
 * @return Shared pointer to the fully populated Scene instance.
 *
 * @note Returns shared_ptr (not value) because Scene inherits from UID,
 *       which is non-copyable and non-movable.
 */
std::shared_ptr<Scene> CreateDefaultScene(const std::string& objPath);

} // namespace neurus
