#include "scene/Mesh.hpp"
#include "core/Log.hpp"
#include "asset/data/MeshData.hpp"
#include "asset/data/ShaderData.hpp"

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
}

bool Mesh::AcceptShader(const ShaderData& shader)
{
	return shader.GetType() == ShaderType::VERTEX && shader.IsValid();
}

const ShaderUnit* Mesh::GetShaderUnit(int shaderType) const
{
	if (!o_shader) return nullptr;
	return o_shader->FindStage(static_cast<ShaderType>(shaderType));
}

} // namespace neurus
