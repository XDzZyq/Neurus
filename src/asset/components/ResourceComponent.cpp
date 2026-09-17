/**
 * @file ResourceComponent.cpp
 * @brief ResourceManager pool persistence (see ResourceComponent.h).
 */

#include "asset/components/ResourceComponent.h"
#include "core/Log.h"
#include "core/ResourceManager.h"

// Types needed for the pooled data-reference wiring in Load().
#include "asset/data/ImageData.h"
#include "asset/data/MeshData.h"
#include "scene/Environment.h"
#include "scene/DebugMesh.h"
#include "scene/Mesh.h"
#include "render/shaders/Shader.h"

// Force-link the polymorphic registration TUs (static libraries) so the pool's
// UID-derived types are registered before serialization runs.
#include "scene/registrations/TypeRegistration.h"
#include "asset/registrations/DataRegistration.h"
#include "render/registrations/ShaderRegistration.h"

namespace neurus::project
{

ResourceComponent::ResourceComponent(ResourceManager& resources)
	: m_resources(&resources)
{
}

void ResourceComponent::Save(cereal::JSONOutputArchive& ar) const
{
	ar(cereal::make_nvp("m_resources", *m_resources));
}

void ResourceComponent::Load(cereal::JSONInputArchive& ar)
{
	try
	{
		ar(cereal::make_nvp("m_resources", *m_resources));
	}
	catch (const cereal::Exception& e)
	{
		// Legacy project file (no "m_resources" node): leave the pool empty.
		// The Scene's ID lists will also fail to load and degrade to the
		// default-camera fallback.
		NEURUS_ERR("ResourceComponent::Load: " << e.what() << " - using empty resource pool.");
		m_resources->Clear();
	}

	// Wire data references for EVERY pooled mesh/environment, not just the
	// scene members: pooled objects stay referenced by undo history after
	// deletion, so their data refs must be wired for on-demand GPU uploads
	// (SceneObjectGpuUploadRequested) after a reload. The pool's data-resource
	// entries have already reloaded their content (their own serialize(load)),
	// so this is pure pointer wiring - no disk I/O.
	m_resources->ForEach<Mesh>([&](const std::shared_ptr<Mesh>& mesh) {
		if (!mesh) return;
		if (mesh->o_meshDataId != 0)
		{
			auto meshData = m_resources->Get<MeshData>(mesh->o_meshDataId);
			if (meshData)
				mesh->o_mesh = meshData;
			else
				NEURUS_ERR("[ResourceComponent] stale MeshData UID " << mesh->o_meshDataId);
		}
		if (mesh->o_shaderId != 0)
		{
			auto shader = m_resources->Get<Shader>(mesh->o_shaderId);
			if (shader)
				mesh->o_shader = shader;
			else
				NEURUS_ERR("[ResourceComponent] stale Shader UID " << mesh->o_shaderId);
		}
	});
	// DebugMesh carries the same MeshData reference as Mesh but is not a Mesh,
	// so ForEach<Mesh> above never reaches it. Without this block a reloaded
	// project would leave o_mesh null and DebugPass would skip the wireframe.
	m_resources->ForEach<DebugMesh>([&](const std::shared_ptr<DebugMesh>& dMesh) {
		if (!dMesh || dMesh->o_meshDataId == 0) return;
		auto meshData = m_resources->Get<MeshData>(dMesh->o_meshDataId);
		if (meshData)
			dMesh->o_mesh = meshData;
		else
			NEURUS_ERR("[ResourceComponent] stale MeshData UID " << dMesh->o_meshDataId
			           << " on DebugMesh");
	});
	m_resources->ForEach<Environment>([&](const std::shared_ptr<Environment>& env) {
		if (!env || env->o_imageDataId == 0) return;
		auto imageData = m_resources->Get<ImageData>(env->o_imageDataId);
		if (!imageData)
		{
			// Keep o_imageDataId (do not zero it) so the broken reference is
			// visible and reported instead of being silently dropped forever.
			NEURUS_ERR("[ResourceComponent] stale ImageData UID " << env->o_imageDataId);
			return;
		}
		env->SetEquirectData(imageData);
	});
}

} // namespace neurus::project
