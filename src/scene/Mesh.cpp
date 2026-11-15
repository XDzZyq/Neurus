#include "scene/Mesh.h"
#include "core/Log.h"
#include "asset/data/MeshData.h"
#include "asset/data/ShaderData.h"

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

const ShaderUnit* Mesh::GetShaderUnit(int shaderType) const
{
	if (!o_shader) return nullptr;
	return o_shader->FindStage(static_cast<ShaderType>(shaderType));
}

} // namespace neurus
