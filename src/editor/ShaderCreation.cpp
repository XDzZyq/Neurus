/**
 * @file ShaderCreation.cpp
 * @brief Editor-side shader asset loading and (later) mesh shader creation.
 */

#include "editor/ShaderCreation.h"

#include "asset/data/shader/ShaderData.h"
#include "asset/data/shader/ShaderStage.h"
#include "core/Log.h"
#include "core/ResourceManager.h"
#include "editor/events/IEventQueue.h"
#include "editor/operations/IOperationSink.h"
#include "editor/operations/ShaderOperations.h"
#include "render/shaders/ComputeShader.h"
#include "render/shaders/RenderShader.h"
#include "render/shaders/ShaderGenerator.h"
#include "render/shaders/ShaderLibrary.h"
#include "render/shaders/ShaderParser.h"
#include "scene/Mesh.h"

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

namespace
{

/**
 * @brief Resolves one stage from a pooled source or a source path.
 *
 * A pooled source contributes the stage matching @p type. A path must carry the
 * matching extension; an empty path uses @p defaultPath. The parsed IR and
 * generated GLSL are stored in @p out.
 */
bool ResolveStage(ResourceManager& resources, int sourceUid, const std::string& path,
                  ShaderType type, const std::string& defaultPath,
                  ShaderUnit& out, std::string& error)
{
	if (sourceUid != 0)
	{
		auto source = resources.Get<ShaderData>(sourceUid);
		if (!source)
		{
			error = "shader source not found: " + std::to_string(sourceUid);
			return false;
		}
		const ShaderUnit* unit = source->FindStage(type);
		if (!unit)
		{
			error = "shader source has no " + ShaderData::TypeToString(type) + " stage";
			return false;
		}
		out = *unit;
		return true;
	}

	const std::string sourcePath = path.empty() ? defaultPath : path;
	if (!path.empty())
	{
		const auto inferred = ShaderTypeFromPath(path);
		if (!inferred || *inferred != type)
		{
			error = "shader path is not a " + ShaderData::TypeToString(type) + " file: " + path;
			return false;
		}
	}

	out.path = sourcePath;
	out.parsed = ShaderParser::ParseShaderFile(sourcePath, type);
	if (out.parsed.IsEmpty())
	{
		error = "failed to parse shader: " + sourcePath;
		return false;
	}
	out.code = ShaderGenerator::Generate(out.parsed);
	return true;
}

} // anonymous namespace

ShaderRequestResult CreateShader(ResourceManager& resources, Mesh& mesh,
                                 const ShaderCreateRequested& input,
                                 IEventQueue& events, IOperationSink& ops)
{
	ShaderRequestResult result;
	const int oldShaderId = mesh.o_shaderId;

	ShaderUnit vert;
	ShaderUnit frag;
	if (!ResolveStage(resources, input.vertexShaderUid, input.vertexPath,
	                  ShaderType::VERTEX, kDefaultVertexPath, vert, result.error))
	{
		return result;
	}
	if (!ResolveStage(resources, input.fragmentShaderUid, input.fragmentPath,
	                  ShaderType::FRAGMENT, kDefaultFragmentPath, frag, result.error))
	{
		return result;
	}

	const std::string shaderName = "MeshShader_" + std::to_string(mesh.GetObjectID());
	auto shader = resources.Load<RenderShader>(shaderName, vert.path, frag.path);
	shader->GetStage(ShaderType::VERTEX) = std::move(vert);
	shader->GetStage(ShaderType::FRAGMENT) = std::move(frag);

	// Compile before touching the mesh: a failure removes the new pooled shader
	// and leaves the old reference intact.
	std::string compileError;
	bool ok = true;
	for (const ShaderType type : { ShaderType::VERTEX, ShaderType::FRAGMENT })
	{
		auto& unit = shader->GetStage(type);
		unit.spv = ShaderLibrary::Compile(unit, type, shaderName, &compileError);
		if (unit.spv.empty())
		{
			ok = false;
			break;
		}
		unit.BumpVersion();
	}

	if (!ok)
	{
		resources.Remove(shader->GetObjectID());
		result.error = compileError.empty() ? "shader compile failed" : compileError;
		return result;
	}

	shader->BumpVersion();
	const std::string unique = resources.SetName(shader->GetObjectID(), shaderName);
	shader->SetName(unique.empty() ? shaderName : unique);

	mesh.SetObjShader(shader);
	ops.Submit(std::make_unique<ShaderReplaceOp>(
		mesh.GetObjectID(), oldShaderId, shader->GetObjectID()));
	events.Emit(ShaderCompiled{ shader->GetObjectID() });

	result.shaderUid = shader->GetObjectID();
	return result;
}

} // namespace neurus
