#pragma once

#include <string>

namespace neurus {

/** @brief Emitted when a mesh file is selected for import (Edit -> Add -> Mesh...). */
struct MeshImportEvent { std::string path; };

/**
 * @brief Reply slot for a shader request; filled by the Editor synchronously.
 *
 * A zero @p shaderUid means failure and carries the reason in @p error.
 */
struct ShaderRequestResult
{
	int         shaderUid = 0;  ///< Pooled shader UID; 0 on failure.
	std::string error;          ///< Reason on failure.
};

/**
 * @brief Imports a shader source file into the pool as a RenderShader or ComputeShader.
 *
 * `.vert.glsl` and `.frag.glsl` build a RenderShader whose missing side is the
 * default G-buffer source; `.comp.glsl` builds a ComputeShader. `.geom.glsl` and
 * unknown extensions are rejected. @p result is borrowed for the synchronous dispatch.
 */
struct ShaderImportRequested
{
	std::string          path;              ///< Shader source path, absolute or relative.
	std::string          name;              ///< Registered name; empty uses the path.
	ShaderRequestResult* result = nullptr;  ///< Optional reply slot, borrowed.
};

/** @brief Emitted when a new camera should be added to the scene (Edit -> Add -> Camera). */
struct CameraAddEvent {};

/** @brief Emitted when a new point light should be added to the scene (Edit -> Add -> Light). */
struct LightAddEvent {};

/** @brief Emitted when a new sun light should be added to the scene. */
struct SunLightAddEvent {};

/** @brief Emitted when a new spot light should be added to the scene. */
struct SpotLightAddEvent {};

} // namespace neurus
