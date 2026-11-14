/**
 * @file ShaderCreation.cpp
 * @brief Editor-side shader asset loading and (later) mesh shader creation.
 */

#include "editor/ShaderCreation.h"

#include "asset/data/shader/ShaderData.h"
#include "asset/data/shader/ShaderStage.h"
#include "core/Log.h"
#include "core/ResourceManager.h"
#include "render/shaders/ComputeShader.h"
#include "render/shaders/RenderShader.h"

#include <exception>
#include <memory>

namespace neurus
{

ShaderRequestResult LoadShaderAsset(ResourceManager& resources, const std::string& path,
                                    const std::string& name)
{
	ShaderRequestResult result;

	const auto stage = ShaderTypeFromPath(path);
	if (!stage)
	{
		result.error = "unsupported shader extension: " + path;
		return result;
	}
	if (*stage == ShaderType::GEOMETRY)
	{
		result.error = "geometry shaders are not supported: " + path;
		return result;
	}

	const std::string shaderName = name.empty() ? path : name;

	try
	{
		std::shared_ptr<ShaderData> shader;

		if (*stage == ShaderType::COMPUTE)
		{
			auto compute = resources.Load<ComputeShader>(shaderName, path);
			if (!compute->ParseAndGenerate())
			{
				resources.Remove(compute->GetObjectID());
				result.error = "failed to parse shader: " + path;
				return result;
			}
			shader = compute;
		}
		else
		{
			const std::string vertPath = *stage == ShaderType::VERTEX ? path : kDefaultVertexPath;
			const std::string fragPath = *stage == ShaderType::FRAGMENT ? path : kDefaultFragmentPath;
			auto render = resources.Load<RenderShader>(shaderName, vertPath, fragPath);
			if (!render->ParseAndGenerate())
			{
				resources.Remove(render->GetObjectID());
				result.error = "failed to parse shader: " + path;
				return result;
			}
			shader = render;
		}

		const std::string unique = resources.SetName(shader->GetObjectID(), shaderName);
		shader->SetName(unique.empty() ? shaderName : unique);
		result.shaderUid = shader->GetObjectID();
	}
	catch (const std::exception& e)
	{
		result.error = e.what();
	}

	return result;
}

} // namespace neurus
