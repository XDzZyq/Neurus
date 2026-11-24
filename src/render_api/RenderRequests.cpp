/**
 * @file RenderRequests.cpp
 * @brief Snapshot helpers for the render-request value types.
 */

#include "render_api/RenderRequests.hpp"

#include "asset/data/ShaderData.hpp"

#include <array>

namespace neurus {

UploadShaderRequest MakeUploadShaderRequest(const ShaderData& shader)
{
	UploadShaderRequest request;
	request.shaderId = shader.RequireObjectID();
	request.version = shader.GetVersion();
	request.primaryType = shader.GetType();
	for (const auto type : {ShaderType::VERTEX, ShaderType::FRAGMENT,
	                        ShaderType::COMPUTE, ShaderType::GEOMETRY})
	{
		if (const auto* stage = shader.FindStage(type))
		{
			ShaderStageUpload upload;
			upload.spv = stage->spv;
			upload.parsed.uniform_list = stage->parsed.uniform_list;
			upload.parsed.ubuffer_list = stage->parsed.ubuffer_list;
			upload.parsed.SB_list = stage->parsed.SB_list;
			upload.parsed.push_constants = stage->parsed.push_constants;
			request.stages.emplace(type, std::move(upload));
		}
	}
	const std::array<const ShaderData*, 1> shaders = {&shader};
	request.ports = ExtractPassPorts({}, shaders);
	return request;
}

EnsurePipelineRequest MakeEnsurePipelineRequest(const ShaderData& shader)
{
	// Id and version must come from the same Shader, the same reasoning as
	// GeometryPass::EnsureMeshPipeline: keying the mesh's id with a shader's
	// version cannot discriminate a mesh relinked between two shaders that
	// happen to share a version.
	EnsurePipelineRequest request;
	request.shaderId = shader.RequireObjectID();
	request.version  = shader.GetVersion();


	return request;
}

LightSnapshot MakeLightSnapshot(const Light& light)
{
	LightSnapshot snapshot;
	snapshot.objectId = light.RequireObjectID();
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
	const std::unordered_map<int, ResourceRef<Light>>& lights)
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
