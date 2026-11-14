#include "scene/Mesh.h"
#include "core/Log.h"
#include "asset/data/MeshData.h"
#include "asset/data/shader/ShaderData.h"

namespace neurus
{

Mesh::Mesh()
{
	o_type = ObjectID::GOType::GO_MESH;
	o_name = "Mesh";
}

Mesh::Mesh(std::shared_ptr<MeshData> meshData)
	: Mesh()
{
	o_mesh = std::move(meshData);
	if (o_mesh)
		o_meshDataId = o_mesh->GetObjectID();
}

Mesh::~Mesh()
{
}

void Mesh::SetObjShader(std::shared_ptr<ShaderData> shader)
{
	o_shader = std::move(shader);
	o_shaderId = o_shader ? o_shader->GetObjectID() : 0;
}

void* Mesh::GetShaderUnit(int shaderType) const
{
	if (!o_shader) return nullptr;
	auto type = static_cast<ShaderType>(shaderType);
	if (!o_shader->HasStage(type)) return nullptr;
	return const_cast<ShaderUnit*>(&o_shader->GetStage(type));
}

} // namespace neurus
