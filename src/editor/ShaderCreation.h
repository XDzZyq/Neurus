#pragma once

#include "editor/events/ShaderEvents.h"

namespace neurus
{

class ResourceManager;

/**
 * @brief Loads one shader source file into the pool.
 *
 * `.vert` and `.frag` build a RenderShader whose missing side is the default
 * G-buffer source; `.comp` builds a ComputeShader. The shader is parsed and
 * generated before it is returned; on failure it is removed from the pool.
 *
 * @param resources Pool that owns the new shader.
 * @param path Source path, absolute or relative.
 * @param name Registered name; empty uses @p path (uniquified by the pool).
 * @return The pooled shader UID, or 0 with a reason.
 */
ShaderRequestResult LoadShaderAsset(ResourceManager& resources, const std::string& path,
                                    const std::string& name);

} // namespace neurus
