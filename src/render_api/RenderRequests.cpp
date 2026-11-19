/**
 * @file RenderRequests.cpp
 * @brief Snapshot helpers for the render-request value types.
 */

#include "render_api/RenderRequests.hpp"

#include "asset/data/ShaderData.hpp"

namespace neurus {

EnsurePipelineRequest MakeEnsurePipelineRequest(const ShaderData& shader)
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

LightSnapshot MakeLightSnapshot(const Light& light)
{
	LightSnapshot snapshot;
	snapshot.objectId = light.GetObjectID();
	snapshot.type = light.light_type;
	snapshot.position = light.GetPosition();
	snapshot.direction = light.GetDirection();
	snapshot.color = light.light_color;
	snapshot.power = light.light_power;
	snapshot.radius = light.light_radius;
	snapshot.spotCutoff = light.spot_cutoff;
	snapshot.spotOuterCutoff = light.spot_outer_cutoff;
	snapshot.areaRatio = light.area_ratio;
	snapshot.useShadow = light.use_shadow;
	snapshot.visibleInViewport = light.is_viewport;
	snapshot.rendered = light.is_rendered;
	return snapshot;
}

UpdateLightingRequest MakeUpdateLightingRequest(
	const std::unordered_map<int, std::shared_ptr<Light>>& lights)
{
	UpdateLightingRequest request;
	for (const auto& [uid, light] : lights)
	{
		if (light && light->is_viewport && light->is_rendered)
			request.lights.emplace(uid, MakeLightSnapshot(*light));
	}
	return request;
}

} // namespace neurus
