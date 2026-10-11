/**
 * @file MeshBindings.cpp
 * @brief Render-layer implementation of Mesh material methods.
 *
 * These methods are declared in scene/Mesh.h but defined here so that the
 * scene layer does not depend on the render layer.
 */

#include "scene/Mesh.h"
#include "scene/Material.h"

namespace neurus
{

// -----------------------------------------------------------------------
// Texture / material parameters
// -----------------------------------------------------------------------

void Mesh::SetTex(int /*_type*/, const std::string& /*_name*/)
{
	// Stub for MVP. Full implementation requires a Vulkan device to load
	// textures, which is not available in the scene layer.
}

void Mesh::SetMatColor(int _type, float _val)
{
	if (!o_material)
	{
		return;
	}
	o_material->SetMatParam(static_cast<Material::MatParaType>(_type), _val);
}

void Mesh::SetMatColor(int _type, const glm::vec3& _col)
{
	if (!o_material)
	{
		return;
	}
	o_material->SetMatParam(static_cast<Material::MatParaType>(_type), _col);
}

} // namespace neurus
