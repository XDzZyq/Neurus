#pragma once

#include "editor/events/ShaderEvents.h"

namespace neurus
{

class ResourceManager;
class Mesh;
class IEventQueue;
class IOperationSink;

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

/**
 * @brief Creates or replaces a mesh's RenderShader atomically.
 *
 * Each side is a source path or a pooled ShaderData UID whose matching stage is
 * copied; empty sides use the default G-buffer pair. The new shader is pooled
 * and compiled before the old reference is dropped, so a failure leaves the
 * mesh untouched. One ShaderReplaceOp records the link.
 *
 * @param resources Pool that owns the new shader.
 * @param mesh Target mesh.
 * @param input Paths/source UIDs from ShaderCreateRequested.
 * @param events Queue for ShaderCompiled on success.
 * @param ops Operation sink for the ShaderReplaceOp.
 * @return The new shader UID, or 0 with a reason.
 */
ShaderRequestResult CreateShader(ResourceManager& resources, Mesh& mesh,
                                 const ShaderCreateRequested& input,
                                 IEventQueue& events, IOperationSink& ops);

} // namespace neurus
