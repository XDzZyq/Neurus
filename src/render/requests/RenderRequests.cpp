/**
 * @file RenderRequests.cpp
 * @brief Snapshot helpers for the render-request value types.
 */

#include "RenderRequests.h"

#include "shaders/Shader.h"

namespace neurus {

EnsurePipelineRequest MakeEnsurePipelineRequest(const Shader& shader)
{
	// Id and version must come from the same Shader, the same reasoning as
	// GeometryPass::EnsureMeshPipeline: keying the mesh's id with a shader's
	// version cannot discriminate a mesh relinked between two shaders that
	// happen to share a version.
	EnsurePipelineRequest request;
	request.shaderId = shader.GetObjectID();
	request.version  = shader.GetVersion();

	// Copy the SPIR-V by value so a recompile landing before the drain cannot
	// change the code under the version it no longer belongs to. A stage the
	// shader does not carry leaves the corresponding vector empty, which the
	// executor skips exactly as a live Shader with an empty stage would.
	if (shader.HasStage(ShaderType::VERTEX))
		request.vertexSpv = shader.GetStage(ShaderType::VERTEX).spv;
	if (shader.HasStage(ShaderType::FRAGMENT))
		request.fragmentSpv = shader.GetStage(ShaderType::FRAGMENT).spv;

	return request;
}

} // namespace neurus
